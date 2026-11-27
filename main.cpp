// GTA SA 1.0 US / Plugin-SDK. Replaces only the volumetric-cloud call in RenderEffects.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "plugin.h"
#include "CClouds.h"
#include "CClock.h"
#include "CGame.h"
#include "CWeather.h"
#include "CTimeCycle.h"
#include "CTimer.h"
#include "CTxdStore.h"
#include "CScene.h"
#include "CloudD3D9.hpp"
#include <cstdio>
#include <new>

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace {
constexpr std::uintptr_t CloudCall = 0x53E1B4;
constexpr std::uintptr_t NativeRenderer = 0x716380;
constexpr int BatchVertices = 480; // multiple of 6; comfortably below the RW Im3D buffer limit
vc::Settings settings;
vc::Field field;
bool haveFrame = false;
std::array<RwRaster*,2> canopyRasters{}; // owned during RW lifetime
std::array<bool,2> canopyMipmaps{},canopyHasPixels{};
bool canopyReady=false;
vc::canopy::Drift canopyDrift;
bool canopyAttempted=false,flightFieldActive=false;
unsigned int lastFrame = 0;
vc::banks::Volume bankVolume;
vc::banks::Mesh bankMesh;
std::array<IDirect3DVolumeTexture9*,2> bankTextures{};
IDirect3DDevice9* bankDevice=nullptr; // borrowed
bool bankFailed=false,bankPrepared=false,bankDrawLogged=false,bankEmptyLogged=false;
vc::d3d9::Options bankOptions;
IDirect3DQuery9* bankPixelQuery=nullptr; // One nonblocking diagnostic, not a per-frame readback.
bool bankPixelQueryAttempted=false,bankDownsampled=false;
int bankQueryFrames=0;
IDirect3DDevice9* CurrentD3DDevice() {
    // SA 1.0 US: _RwD3DDevice at C97C28. The SDK's public getter at
    // 7F9D50 clears RW's matrix/stream cache every call; do not call it.
    return *reinterpret_cast<IDirect3DDevice9**>(0xC97C28);
}
void ReleaseBankTextures() {
    if(bankPixelQuery){bankPixelQuery->Release();bankPixelQuery=nullptr;}
    bankPixelQueryAttempted=false;bankQueryFrames=0;bankDownsampled=false;
    for(auto& texture:bankTextures) {
        if(texture)texture->Release();
        texture=nullptr;
    }
    bankDevice=nullptr;bankFailed=false;bankDrawLogged=false;bankEmptyLogged=false;
}
void ReleaseBanks() {
    ReleaseBankTextures();
    bankVolume={};bankMesh={};bankPrepared=false;
}

std::filesystem::path ModulePath() {
    HMODULE module = nullptr;
    wchar_t path[32768]{};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                          GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&settings), &module)) {
        const DWORD len = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
        if (len > 0 && len < std::size(path))
            return std::filesystem::path(path).parent_path();
    }
    return L".";
}

void Log(const char* message) {
    std::ofstream out(ModulePath()/L"VolumetricCloudsExtended.log", std::ios::app);
    if (out) out << message << '\n';
}

void LoadIni() {
    const auto path = ModulePath()/L"VolumetricCloudsExtended.ini";
    std::ifstream in(path);
    if (in) {
        vc::load(in,settings);
        return;
    }
    std::error_code ec;
    // Never truncate an existing INI (including one that could not be opened).
    if (!std::filesystem::exists(path,ec) && !ec) {
        std::ofstream out(path);
        if (out) vc::write(out,settings);
    }
}

void FreeCanopyResources() {
    for(auto& raster:canopyRasters) {
        if(raster) RwRasterDestroy(raster);
        raster=nullptr;
    }
    canopyMipmaps={};canopyHasPixels={};canopyReady=false;
    ReleaseBanks();
}
void ReleaseCanopy() {
    FreeCanopyResources();
    canopyAttempted=false;
}
bool EnsureCanopy() {
    if(canopyAttempted) return canopyReady;
    canopyAttempted=true;
    const auto create=[&](int size) {
        try {
            vc::canopy::Map map;
            map.build(size,std::uint32_t(settings.seed));
            map.shadeWeatherLayers(settings);
            auto* image=RwImageCreate(size,size,32);
            if(!image) return false;
            struct FreeImage {RwImage* p;~FreeImage(){RwImageDestroy(p);}} freeImage{image};
            if(!RwImageAllocatePixels(image) || !RwImageGetPixels(image) || RwImageGetStride(image)<size*4) return false;
            const int mipFlags=rwRASTERFORMATMIPMAP|rwRASTERFORMATAUTOMIPMAP;
            for(int layer=0;layer<2;++layer) {
                const auto& pixels=layer?map.extraPixels:map.pixels;
                canopyHasPixels[layer]=std::any_of(pixels.begin(),pixels.end(),[](const auto& p){return p.a!=0;});
                if(!canopyHasPixels[layer]) continue;
                for(int y=0;y<size;++y)
                    std::memcpy(RwImageGetPixels(image)+y*RwImageGetStride(image),
                        pixels.data()+std::size_t(y)*size,size*sizeof(vc::canopy::Pixel));
                RwInt32 w=0,h=0,depth=0,format=0;
                if(RwImageFindRasterFormat(image,rwRASTERTYPETEXTURE,&w,&h,&depth,&format) && w==size && h==size) {
                    for(int flags:{mipFlags,0}) {
                        auto* raster=RwRasterCreate(w,h,depth,(format&~mipFlags)|rwRASTERTYPETEXTURE|flags);
                        if(raster && RwRasterSetFromImage(raster,image)) {
                            canopyRasters[layer]=raster;canopyMipmaps[layer]=RwRasterGetNumLevels(raster)>1;
                            break;
                        }
                        if(raster) RwRasterDestroy(raster);
                    }
                }
                if(!canopyRasters[layer]) {FreeCanopyResources();return false;}
            }
            if(settings.hybridClouds) {
                try {bankVolume.build(settings,map);bankPrepared=true;}
                catch(const std::bad_alloc&) {ReleaseBanks();bankFailed=true;Log("3D density allocation failed; using canopy.");}
            }
            return true;
        } catch(const std::bad_alloc&) {
            FreeCanopyResources();Log("Canopy: CPU allocation failed.");return false;
        }
    };
    const int requested=vc::canopy::textureSize(settings);
    bool ok=create(requested);
    if(!ok && requested!=vc::canopy::EconomySize) {
        Log("Canopy: detail textures unavailable; trying economy.");
        ok=create(vc::canopy::EconomySize);
    }
    canopyReady=ok;
    Log(ok?"Canopy: static weather masks generated (live coverage)."
          :"Canopy: texture creation failed; using crossed native textures.");
    return ok;
}

void LogD3DFailure(const char* phase,vc::d3d9::Result result) {
    char message[256];
    std::snprintf(message,sizeof(message),"3D cloud %s: %s failed (HRESULT 0x%08lX).",
        phase,result.operation,static_cast<unsigned long>(result.hr));
    Log(message);
}
bool EnsureBanks() {
    if(!settings.hybridClouds||!bankPrepared)return false;
    auto* device=CurrentD3DDevice();
    if(!device)return false;
    if(device!=bankDevice)ReleaseBankTextures();
    if(bankFailed)return false;
    if(bankPixelQuery) {
        DWORD samples=0;
        const auto hr=bankPixelQuery->GetData(&samples,sizeof(samples),0); // no FLUSH / wait
        if(hr==S_OK||FAILED(hr)||++bankQueryFrames>=120) {
            char info[160];
            if(hr==S_OK) {
                std::snprintf(info,sizeof(info),"3D cloud first draw: %lu depth-passing samples (GPU occlusion query).",
                    static_cast<unsigned long>(samples));Log(info);
            } else Log("3D cloud GPU query unavailable or timed out; submission status still logged.");
            bankPixelQuery->Release();bankPixelQuery=nullptr;
        }
    }
    if(bankTextures[0]&&bankTextures[1])return true;
    bankDevice=device;
    const auto fail=[](){ReleaseBankTextures();bankDevice=CurrentD3DDevice();bankFailed=true;Log("3D cloud bank unavailable; retaining canopy.");return false;};
    D3DCAPS9 caps{};
    D3DDEVICE_CREATION_PARAMETERS creation{};
    HRESULT hr=device->GetDeviceCaps(&caps);
    if(FAILED(hr)){LogD3DFailure("initialization",vc::d3d9::failed(hr,"GetDeviceCaps"));return fail();}
    const HRESULT creationResult=device->GetCreationParameters(&creation);
    if(FAILED(creationResult))LogD3DFailure("diagnostic",vc::d3d9::failed(creationResult,"GetCreationParameters"));
    bankOptions=vc::d3d9::options(caps);
    char info[384];
    std::snprintf(info,sizeof(info),"3D cloud device: flags=0x%08lX%s volume=%d maxExtent=%lu filters=0x%08lX textures=%lu stages=%lu ops=0x%08lX.",
        static_cast<unsigned long>(creation.BehaviorFlags),
        (creation.BehaviorFlags&D3DCREATE_PUREDEVICE)?" (pure device supported)":"",
        !!(caps.TextureCaps&D3DPTEXTURECAPS_VOLUMEMAP),static_cast<unsigned long>(caps.MaxVolumeExtent),
        static_cast<unsigned long>(caps.VolumeTextureFilterCaps),static_cast<unsigned long>(caps.MaxSimultaneousTextures),
        static_cast<unsigned long>(caps.MaxTextureBlendStages),static_cast<unsigned long>(caps.TextureOpCaps));
    Log(info);
    if(!(caps.TextureCaps&D3DPTEXTURECAPS_VOLUMEMAP)||caps.MaxVolumeExtent<16) {
        Log("3D cloud device does not advertise usable volume textures.");return fail();
    }
    // Power-of-two downsampling for older caps and allocation failures.
    // System-memory -> default works with D3D9Ex; managed fallback is only
    // attempted if the driver rejects staging/upload.
    if(!bankOptions.dual)Log("3D cloud device lacks dual-volume blending; using nearest weather endpoint.");
    if(bankOptions.filter==D3DTEXF_POINT)Log("3D cloud device lacks linear volume filtering; using point filtering.");
    for(UINT w=UINT(bankVolume.width);w>=16;w/=2) {
        if(w>caps.MaxVolumeExtent)continue;
        const UINT d=std::min(UINT(bankVolume.depth),w);
        bool ready=true;
        for(int layer=0;layer<2;++layer) {
            auto result=vc::d3d9::upload(device,bankVolume,layer,w,d,&bankTextures[layer]);
            if(!result) {
                LogD3DFailure("staged upload",result);
                result=vc::d3d9::upload(device,bankVolume,layer,w,d,&bankTextures[layer],true);
                if(!result){LogD3DFailure("managed upload",result);ready=false;break;}
            }
        }
        if(ready) {
            bankDownsampled=w!=UINT(bankVolume.width)||d!=UINT(bankVolume.depth);
            std::snprintf(info,sizeof(info),"3D cloud bank ready: %lux%lux%lu RGBA volumes, shader-free, %s weather, %s filtering.",
                static_cast<unsigned long>(w),static_cast<unsigned long>(w),static_cast<unsigned long>(d),
                bankOptions.dual?"interpolated":"endpoint",bankOptions.filter==D3DTEXF_LINEAR?"linear":"point");
            Log(info);return true;
        }
        for(auto& texture:bankTextures){if(texture)texture->Release();texture=nullptr;}
    }
    return fail();
}

void ResetField() {
    field.reset(settings);
    canopyDrift={};
    flightFieldActive=false;
    haveFrame=false;
}

vc::Vec Vec(const RwV3d& v) { return {v.x,v.y,v.z}; }

bool BadWeather(short wt) {
    switch (wt) {
    case WEATHER_CLOUDY_LA:
    case WEATHER_CLOUDY_SF:
    case WEATHER_RAINY_SF:
    case WEATHER_CLOUDY_VEGAS:
    case WEATHER_CLOUDY_COUNTRYSIDE:
    case WEATHER_RAINY_COUNTRYSIDE:
        return true;
    default: return false;
    }
}

// All texture pointers are borrowed for this pass only. No RwTextureRead fallback,
// no owned references, and no GPU objects that need to survive a device reset.
struct Textures {
    std::array<RwRaster*,16> rasters{};
    std::size_t count=0;
    void add(RwTexture* tex) {
        RwRaster* raster=tex?RwTextureGetRaster(tex):nullptr;
        if (raster && count<rasters.size() &&
            std::find(rasters.begin(),rasters.begin()+count,raster)==rasters.begin()+count)
            rasters[count++]=raster;
    }
    RwRaster* get(std::uint32_t key) const { return count?rasters[key%count]:nullptr; }
};

Textures GetTextures() {
    Textures out;
    RwTexture* original=CClouds::ms_vc.m_pTex;
    if (settings.includeOriginalTextureInRandom) out.add(original);
    if (settings.useAdditionalTextures && CTxdStore::ms_pTxdPool) {
        const int slot=CTxdStore::FindTxdSlot("particle");
        if (slot>=0) {
            // Access the loaded dictionary directly rather than mutating the global
            // current TXD or calling the game's loose-file texture loader.
            const auto* def=CTxdStore::ms_pTxdPool->GetAt(slot);
            if (def && def->m_pRwDictionary) {
                for (const auto& name : settings.textureNames)
                    out.add(RwTexDictionaryFindNamedTexture(def->m_pRwDictionary,name.c_str()));
            }
        }
    }
    if (!out.count) out.add(original);
    return out;
}

// Save every RenderWare state that this renderer explicitly changes. If a getter
// fails, skip the custom pass rather than writing an unrestorable state.
class RenderStates {
    struct Entry { RwRenderState key; void* value{}; };
    std::array<Entry,13> old{{
        {rwRENDERSTATEZWRITEENABLE},{rwRENDERSTATEZTESTENABLE},
        {rwRENDERSTATEVERTEXALPHAENABLE},{rwRENDERSTATESRCBLEND},
        {rwRENDERSTATEDESTBLEND},{rwRENDERSTATETEXTURERASTER},
        {rwRENDERSTATEFOGENABLE},{rwRENDERSTATECULLMODE},
        {rwRENDERSTATETEXTUREFILTER},{rwRENDERSTATETEXTUREADDRESSU},
        {rwRENDERSTATETEXTUREADDRESSV},{rwRENDERSTATEALPHATESTFUNCTION},
        {rwRENDERSTATESHADEMODE}
    }};
    bool active=false;
    static void set(RwRenderState state, std::uintptr_t value) {
        RwRenderStateSet(state,reinterpret_cast<void*>(value));
    }
public:
    bool begin() {
        for (auto& e:old) if (!RwRenderStateGet(e.key,&e.value)) return false;
        active=true;
        set(rwRENDERSTATEZWRITEENABLE,FALSE);
        set(rwRENDERSTATEZTESTENABLE,TRUE);
        set(rwRENDERSTATEVERTEXALPHAENABLE,TRUE);
        set(rwRENDERSTATESRCBLEND,rwBLENDSRCALPHA);
        set(rwRENDERSTATEDESTBLEND,rwBLENDINVSRCALPHA);
        set(rwRENDERSTATEFOGENABLE,settings.useGameFog?TRUE:FALSE);
        set(rwRENDERSTATECULLMODE,rwCULLMODECULLNONE);
        set(rwRENDERSTATETEXTUREFILTER,rwFILTERLINEAR);
        set(rwRENDERSTATETEXTUREADDRESSU,rwTEXTUREADDRESSCLAMP);
        set(rwRENDERSTATETEXTUREADDRESSV,rwTEXTUREADDRESSCLAMP);
        // The game's alpha-test threshold can otherwise slice off soft fade edges.
        set(rwRENDERSTATEALPHATESTFUNCTION,rwALPHATESTFUNCTIONALWAYS);
        set(rwRENDERSTATESHADEMODE,rwSHADEMODEGOURAUD);
        return true;
    }
    ~RenderStates() {
        if (active) for (auto it=old.rbegin();it!=old.rend();++it)
            RwRenderStateSet(it->key,it->value);
    }
};

unsigned char Byte(float x) { return static_cast<unsigned char>(vc::clamp(x,0.f,255.f)+.5f); }

bool Visible(RwCamera* camera, vc::Vec p, float radius) {
    RwSphere sphere{{p.x,p.y,p.z},radius};
    return RwCameraFrustumTestSphere(camera,&sphere)!=rwSPHEREOUTSIDE;
}

struct DrawCard {
    vc::Card card;
    float depth;
    int order;
    RwRaster* raster;
};
std::array<DrawCard,vc::MaxClouds*vc::MaxCards> drawList;
std::array<RwIm3DVertex,BatchVertices> vertices;
constexpr int BatchIndices=BatchVertices*4;
std::array<RwImVertexIndex,BatchIndices> modernIndices;
static_assert(sizeof(RwImVertexIndex)==sizeof(std::uint16_t));
void DrawCanopy(const vc::View& view,const vc::Range& range,float hybridFade,
                const vc::weather::State& weather,const vc::canopy::Light& light) {
    if(settings.opacity<=0.f || settings.cumulusDensity<=0.f || !settings.targetProcClouds) return;
    RenderStates states;
    if(!states.begin()) return;
    const auto mesh=vc::canopy::mesh(settings,view,range,canopyDrift,vc::banks::radius(settings,range),hybridFade,true);
    static_assert(vc::canopy::VertexCount<=BatchVertices && vc::canopy::IndexCount<=BatchIndices);
    const auto weights=vc::canopy::layerWeights(settings,weather);
    std::copy(mesh.indices.begin(),mesh.indices.end(),modernIndices.begin());
    RwRenderStateSet(rwRENDERSTATETEXTUREADDRESSU,reinterpret_cast<void*>(rwTEXTUREADDRESSWRAP));
    RwRenderStateSet(rwRENDERSTATETEXTUREADDRESSV,reinterpret_cast<void*>(rwTEXTUREADDRESSWRAP));
    for(int layer=0;layer<2;++layer) {
        const float alpha=layer?weights.extra:weights.base;
        if(!canopyHasPixels[layer] || !canopyRasters[layer] || alpha<=1.f/255.f) continue;
        bool visible=false;
        for(int i=0;i<vc::canopy::VertexCount;++i) {
            auto& v=vertices[i];
            const auto& p=mesh.vertices[i];
            visible|=p.alpha*alpha>1.f/255.f;
            RwIm3DVertexSetPos(&v,p.pos.x,p.pos.y,p.pos.z);
            RwIm3DVertexSetNormal(&v,0.f,0.f,1.f);
            RwIm3DVertexSetU(&v,p.u);RwIm3DVertexSetV(&v,p.v);
            RwIm3DVertexSetRGBA(&v,Byte(light.bright.r),Byte(light.bright.g),Byte(light.bright.b),Byte(p.alpha*alpha*255.f));
        }
        if(!visible) continue;
        RwRenderStateSet(rwRENDERSTATETEXTURERASTER,canopyRasters[layer]);
        RwRenderStateSet(rwRENDERSTATETEXTUREFILTER,reinterpret_cast<void*>(canopyMipmaps[layer]?rwFILTERLINEARMIPLINEAR:rwFILTERLINEAR));
        if(RwIm3DTransform(vertices.data(),vc::canopy::VertexCount,nullptr,
                            rwIM3D_VERTEXXYZ|rwIM3D_VERTEXUV|rwIM3D_VERTEXRGBA)) {
            RwIm3DRenderIndexedPrimitive(rwPRIMTYPETRILIST,modernIndices.data(),vc::canopy::IndexCount);
            RwIm3DEnd();
        }
    }
}
// RW uses 2D UV Im3D vertices. The sole direct-D3D draw carries the third
// coordinate; capture/restore all state, including streams and transforms,
// so the engine's cached state agrees with the device afterwards.
bool DrawBanks(RwCamera* camera,const vc::View& view,const vc::Range& range,
               const vc::weather::State& climate,const vc::canopy::Light& light) {
    if(!bankDevice||!bankTextures[0]||!bankTextures[1])return false;
    auto* window=RwCameraGetViewWindow(camera);
    if(!window)return false;
    const auto* offset=RwCameraGetViewOffset(camera);
    const float wx=window->x+(offset?std::abs(offset->x):0.f),
                wy=window->y+(offset?std::abs(offset->y):0.f);
    try {vc::banks::buildMesh(bankMesh,settings,view,range,canopyDrift,wx,wy,bankDownsampled?nullptr:&bankVolume,bankOptions.dual?climate.coverage:(climate.coverage>=.5f?1.f:0.f));}
    catch(const std::bad_alloc&) {bankFailed=true;Log("3D cloud slice allocation failed.");return false;}
    if(bankMesh.indices.empty()) {
        if(!bankEmptyLogged&&!bankDrawLogged){Log("3D cloud bank ready; no occupied slices in current camera frustum yet.");bankEmptyLogged=true;}
        return true;
    }
    static std::vector<vc::d3d9::Vertex> batch;
    try {batch.resize(bankMesh.vertices.size());}
    catch(const std::bad_alloc&) {Log("3D cloud vertex allocation failed.");return false;}
    const float visibility=vc::weather::visibility(climate)*vc::weather::clearOpacity(climate);
    if(visibility<=1.f/255.f)return true;
    const auto bankLight=vc::banks::bankLight(light,climate);
    for(std::size_t i=0;i<batch.size();++i) {
        const auto& v=bankMesh.vertices[i];
        batch[i]={v.pos.x,v.pos.y,v.pos.z,D3DCOLOR_ARGB(Byte(v.alpha*visibility*255.f),
            Byte(bankLight.r),Byte(bankLight.g),Byte(bankLight.b)),v.u,v.v,v.w};
    }
    // CameraBeginUpdate (7F8F20) writes these matrices before applying
    // them to D3D. Read the active camera matrices, not device GetTransform
    // or the RW cache (which other mods may invalidate).
    const auto& viewMatrix=*reinterpret_cast<const D3DMATRIX*>(0xC9BC80);
    const auto& projectionMatrix=*reinterpret_cast<const D3DMATRIX*>(0x8E2458);
    plugin::Call<0x7FC200>(); // flush pending RW state changes before snapshot
    if(!bankPixelQueryAttempted) {
        bankPixelQueryAttempted=true;
        if(SUCCEEDED(bankDevice->CreateQuery(D3DQUERYTYPE_OCCLUSION,&bankPixelQuery))) {
            if(FAILED(bankPixelQuery->Issue(D3DISSUE_BEGIN))) {
                bankPixelQuery->Release();bankPixelQuery=nullptr;
            }
        }
    }
    const bool queryThisDraw=bankPixelQuery&&bankQueryFrames==0&&!bankDrawLogged;
    const auto result=vc::d3d9::draw(bankDevice,bankTextures,bankOptions,batch.data(),UINT(batch.size()),
        bankMesh.indices.data(),UINT(bankMesh.indices.size()/3),climate.coverage,settings.useGameFog,&viewMatrix,&projectionMatrix);
    if(queryThisDraw && FAILED(bankPixelQuery->Issue(D3DISSUE_END))) {
        bankPixelQuery->Release();bankPixelQuery=nullptr;
    }
    if(!result){LogD3DFailure("draw",result);return false;}
    if(!bankDrawLogged) {
        char info[256];
        std::snprintf(info,sizeof(info),"3D cloud draw submitted: %d fixed-phase planes, %lu triangles, camera Z=%.1f, coverage=%.2f.",
            bankMesh.slices,static_cast<unsigned long>(bankMesh.indices.size()/3),view.pos.z,climate.coverage);
        Log(info);bankDrawLogged=true;
    }
    return true;
}

void Flush(int& count,RwRaster* raster) {
    if (!count) return;
    RwRenderStateSet(rwRENDERSTATETEXTURERASTER,raster);
    if (RwIm3DTransform(vertices.data(),count,nullptr,rwIM3D_VERTEXXYZ|rwIM3D_VERTEXUV|rwIM3D_VERTEXRGBA)) {
        RwIm3DRenderPrimitive(rwPRIMTYPETRILIST);
        RwIm3DEnd();
    }
    count=0;
}

void Append(const vc::Card& card,const vc::CardColours& colours,int& count) {
    // Two triangles, identical UV convention to SA: top-left (0,0).
    constexpr int corners[6]={0,1,2,1,3,2};
    const auto normal=vc::unit(vc::Vec{
        card.right.y*card.up.z-card.right.z*card.up.y,
        card.right.z*card.up.x-card.right.x*card.up.z,
        card.right.x*card.up.y-card.right.y*card.up.x});
    for (int index:corners) {
        const bool right=(index&1)!=0, bottom=(index&2)!=0;
        const auto p=card.center+card.right*(right?1.f:-1.f)+card.up*(bottom?-1.f:1.f);
        const auto& colour=bottom?colours.bottom:colours.top;
        auto& v=vertices[count++];
        RwIm3DVertexSetPos(&v,p.x,p.y,p.z);
        RwIm3DVertexSetNormal(&v,normal.x,normal.y,normal.z);
        RwIm3DVertexSetU(&v,right?1.f:0.f);
        RwIm3DVertexSetV(&v,bottom?1.f:0.f);
        RwIm3DVertexSetRGBA(&v,Byte(colour.r),Byte(colour.g),Byte(colour.b),Byte(card.alpha*255.f));
    }
}

void __cdecl RenderClouds() {
    if (!CGame::CanSeeOutSideFromCurrArea() || CWeather::UnderWaterness>.5f) return;
    RwCamera* camera=Scene.m_pCamera;
    if (!camera || RwCameraGetCurrentCamera()!=camera) return;
    RwFrame* frame=RwCameraGetFrame(camera);
    if (!frame) return;
    const RwMatrix* matrix=RwFrameGetLTM(frame);
    if (!matrix) return;
    const vc::View view{Vec(matrix->pos),vc::unit(Vec(matrix->right)),vc::unit(Vec(matrix->up)),
        vc::unit(Vec(matrix->at)),RwCameraGetFarClipPlane(camera),RwCameraGetNearClipPlane(camera)};
    if (!vc::finite(view.pos) || !vc::finite(view.forward) || vc::length(view.forward)<.9f ||
        !vc::finite(view.up) || !vc::finite(view.right) || vc::length(view.up)<.9f || vc::length(view.right)<.9f)
        return;
    const auto& cc=CTimeCycle::m_CurrentColours;
    const auto range=vc::range(settings,view.farClip,cc.m_fFarClip);
    if (range.end<=1.f) return;
    const vc::Palette palette{
        {float(cc.m_nSkyTopRed),float(cc.m_nSkyTopGreen),float(cc.m_nSkyTopBlue)},
        {float(cc.m_nSkyBottomRed),float(cc.m_nSkyBottomGreen),float(cc.m_nSkyBottomBlue)},
        {float(cc.m_nLowCloudsRed),float(cc.m_nLowCloudsGreen),float(cc.m_nLowCloudsBlue)},
        {float(cc.m_nFluffyCloudsBottomRed),float(cc.m_nFluffyCloudsBottomGreen),float(cc.m_nFluffyCloudsBottomBlue)}
    };
    const int requestedMode=settings.renderMode;
    struct RestoreMode { int mode; ~RestoreMode(){settings.renderMode=mode;} } restore{requestedMode};
    const auto frameId=CTimer::m_FrameCounter;
    const bool newFrame=!haveFrame || frameId!=lastFrame;
    const float dt=CTimer::m_UserPause || CTimer::m_CodePause ? 0.f : CTimer::ms_fTimeStep;
    const float weather=vc::weatherFactor(BadWeather(CWeather::OldWeatherType),BadWeather(CWeather::NewWeatherType),
        CWeather::InterpolationValue,CWeather::Rain);
    const auto& wind=CWeather::WindDir;
    const vc::Vec windVector=vc::Vec{wind.x,wind.y,0.f}*CWeather::Wind;
    bool modern=false;
    float flightAlpha=1.f;
    int desired=vc::target(settings,weather);
    auto cardRange=range;
    if (requestedMode==2) {
        if (!settings.targetProcClouds || settings.opacity<=0.f || settings.cumulusDensity<=0.f) return;
        const int index=std::clamp(CTimeCycle::m_CurrentStoredValue,0,15);
        const auto& sun=CTimeCycle::m_VectorToSun[index];
        static_assert(WEATHER_RAINY_SF==8 && WEATHER_RAINY_COUNTRYSIDE==16 && WEATHER_FOGGY_SF==9 && WEATHER_SANDSTORM_DESERT==19);
        const auto climate=vc::weather::sample({CWeather::OldWeatherType,CWeather::NewWeatherType,CWeather::InterpolationValue,
            CWeather::CloudCoverage,CWeather::Rain,CWeather::Foggyness,CWeather::Sandstorm,CWeather::ExtraSunnyness,
            CWeather::bScriptsForceRain,CWeather::LightningFlash});
        const auto light=vc::canopy::liveLighting(settings,palette,{sun.x,sun.y,sun.z},climate);
        modern=EnsureCanopy();
        if(modern && newFrame) canopyDrift.update(settings,windVector,dt);
        if (modern) {
            const bool useBanks=EnsureBanks();
            DrawCanopy(view,range,useBanks?1.f:0.f,climate,light);
            if(useBanks && !DrawBanks(camera,view,range,climate,light)) {
                Log("3D cloud draw failed; falling back to canopy.");
                bankFailed=true;
                // Restore the full canopy from the next frame; avoid double compositing this frame.
            }
            // Optional legacy fixed cards are localized to flight altitude;
            // the 3D bank does not depend on these native textures.
            flightAlpha=vc::canopy::flightWeight(settings,view)*.65f;
            desired=std::min(settings.canopyFlightClouds,vc::target(settings,climate.coverage));
            if (!desired || flightAlpha<=1.f/255.f) {
                if(newFrame) {
                    if (flightFieldActive) field.reset(settings);
                    flightFieldActive=false;haveFrame=true;lastFrame=frameId;
                }
                return;
            }
            flightFieldActive=true;
            cardRange=vc::range(settings,std::min(view.farClip,900.f),cc.m_fFarClip);
        }
        // Optional flight wisps and resource-failure fallback use native geometry.
        settings.renderMode=0;
    }
    Textures textures;
    if (modern) textures.add(CClouds::ms_vc.m_pTex);
    else textures=GetTextures();
    if (!textures.count) return;

    if (newFrame) {
        field.update(settings,view,cardRange,desired,windVector,dt);
        haveFrame=true;lastFrame=frameId;
    }

    int drawCount=0;
    for (const auto& c:field.clouds) {
        if (!c.used) continue;
        const float a=vc::cloudAlpha(settings,c,view,cardRange)*flightAlpha;
        if (a<1.f/255.f || !Visible(camera,c.position,vc::bound(c))) continue;
        vc::Card cards[vc::MaxCards];
        const int count=vc::cards(settings,c,view,a,cards);
        for (int i=0;i<count;++i) {
            auto& card=cards[i];
            if (card.alpha<1.f/255.f) continue;
            const float depth=vc::viewDepth(view,card.center);
            const float radius=vc::cardBound(card);
            if (depth+radius<view.nearClip ||
                depth-radius>view.farClip || !Visible(camera,card.center,radius)) continue;
            drawList[drawCount]={card,depth,drawCount,textures.get(card.textureKey)};
            ++drawCount;
        }
    }
    if (!drawCount) return;
    // Stable ties avoid shimmer between intersecting crossed cards; sort by view depth,
    // not radial distance, so translucent clouds compose correctly with camera rotation.
    // Insertion order is deterministic; explicit tie index avoids stable_sort heap allocations.
    std::sort(drawList.begin(),drawList.begin()+drawCount,[](const DrawCard& a,const DrawCard& b) {
        if (a.depth!=b.depth) return a.depth>b.depth;
        return a.order<b.order;
    });

    RenderStates states;
    if (!states.begin()) return;
    const vc::CloudLighting lighting(settings,palette);
    int count=0;
    RwRaster* raster=nullptr;
    for (int i=0;i<drawCount;++i) {
        const auto& d=drawList[i];
        // Batch only consecutive matching rasters: no texture regrouping that breaks alpha order.
        if (count+6>BatchVertices || raster!=d.raster) Flush(count,raster);
        raster=d.raster;
        const float distanceNorm=settings.lightingMode==1?vc::length(d.card.center-view.pos)/cardRange.end:0.f;
        Append(d.card,lighting.colours(d.card.shade,distanceNorm),count);
    }
    Flush(count,raster);
}

class VolumetricCloudsExtended {
public:
    VolumetricCloudsExtended() {
        LoadIni();
        if (!plugin::IsGameVersion10us()) {
            Log("Unsupported executable: expected GTA SA 1.0 US. No hooks installed.");
            return;
        }
        // Refuse to overwrite an unknown instruction or another mod's call redirect.
        const auto* code=reinterpret_cast<const unsigned char*>(CloudCall);
        std::int32_t displacement=0;
        std::memcpy(&displacement,code+1,sizeof(displacement));
        if (code[0]!=0xE8 || CloudCall+5+displacement!=NativeRenderer) {
            Log("Volumetric cloud call does not target 0x716380; left untouched.");
            return;
        }
        ResetField();
        plugin::patch::RedirectCall(CloudCall,RenderClouds);
        plugin::Events::initGameEvent += ResetField;
        plugin::Events::reInitGameEvent += ResetField;
        plugin::Events::shutdownRwEvent += ResetField;
        plugin::Events::shutdownRwEvent += ReleaseCanopy;
        plugin::Events::d3dLostEvent += ReleaseBankTextures;
        plugin::Events::d3dResetEvent += ReleaseBankTextures;
        Log(settings.renderMode==2 ? "Procedural volumetric cloud renderer installed (SA 1.0 US, world-locked D3D9 volume banks, pure-device compatible)." : settings.lightingMode==0
            ? "Procedural volumetric cloud renderer installed (SA 1.0 US, native lighting)."
            : "Procedural volumetric cloud renderer installed (SA 1.0 US, custom lighting).");
    }
} gPlugin;
} // namespace
