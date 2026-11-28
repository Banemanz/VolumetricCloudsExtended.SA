// This test uses the actual plugin's D3D9 upload, FVF, sampler and draw path.
// It does NOT mock a device or emulate the rasterizer. If the machine has
// no usable D3D9 device, CTest marks it skipped (not passed).
#include "../CloudD3D9.hpp"
#include "../CloudBillboards.hpp"
#include "../CloudCanopyVolume.hpp"
#include "../CloudProfiler.hpp"
#include <windows.h>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include "d3d9_cloud_benchmark.hpp"
#ifdef VC_GPU_VERTEX
#include "CloudBankReferenceVS.hpp"
#endif

using namespace vc;
namespace {
template<class T> struct Com {
    T* p{};
    ~Com(){if(p)p->Release();}
    T* operator->() const{return p;}
};
int fail(const char* step,HRESULT hr=E_FAIL) {
    std::fprintf(stderr,"%s: HRESULT 0x%08lX\n",step,static_cast<unsigned long>(hr));return 1;
}
int run(IDirect3DDevice9* device,const D3DCAPS9& caps) {
    D3DDEVICE_CREATION_PARAMETERS creation{};
    if(FAILED(device->GetCreationParameters(&creation)))return fail("GetCreationParameters");
    std::printf("Device flags=0x%08lX (%s)\n",static_cast<unsigned long>(creation.BehaviorFlags),
        creation.BehaviorFlags&D3DCREATE_PUREDEVICE?"PURE":"non-pure");
    if(!(caps.TextureCaps&D3DPTEXTURECAPS_VOLUMEMAP)||caps.MaxVolumeExtent<16) {
        std::puts("SKIP: device has no volume textures.");return 77;
    }
    banks::Volume volume;
    volume.width=int(std::min<DWORD>(128,caps.MaxVolumeExtent));
    // Keep power-of-two dimensions as in the actual plugin.
    int pw=16;while(pw*2<=volume.width)pw*=2;
    volume.width=pw;volume.depth=std::min(32,pw);
    // RGB and opacity intentionally change along W independently of U/V.
    // Weather also changes both values; a 2D sampler or wrong LERP fails.
    for(int layer=0;layer<2;++layer) {
        volume.pixels[layer].resize(std::size_t(volume.width)*volume.width*volume.depth);
        for(int z=0;z<volume.depth;++z)for(int y=0;y<volume.width;++y)for(int x=0;x<volume.width;++x) {
            const auto grey=static_cast<std::uint8_t>(layer?(z<volume.depth/2?128:224):(z<volume.depth/2?64:192));
            volume.pixels[layer][volume.index(x,y,z)]={grey,grey,grey,std::uint8_t(layer?192:128)};
        }
    }
    std::array<Com<IDirect3DVolumeTexture9>,2> owner;
    std::array<IDirect3DVolumeTexture9*,2> textures{};
    for(int layer=0;layer<2;++layer) {
        auto result=d3d9::upload(device,volume,layer,UINT(volume.width),UINT(volume.depth),&owner[layer].p);
        if(!result)return fail(result.operation,result.hr);
        textures[layer]=owner[layer].p;
    }
    const auto opts=d3d9::options(caps);
    const auto transform=d3d9::identity();
    // Render two side-by-side quads with different third texture coordinate.
    const d3d9::Vertex vertices[]={
        {-.9f,-.8f,.5f,0xFFFFFFFF,.5f,.5f,.25f},
        {-.1f,-.8f,.5f,0xFFFFFFFF,.5f,.5f,.25f},
        {-.9f, .8f,.5f,0xFFFFFFFF,.5f,.5f,.25f},
        {-.1f, .8f,.5f,0xFFFFFFFF,.5f,.5f,.25f},
        { .1f,-.8f,.5f,0xFFFFFFFF,.5f,.5f,.75f},
        { .9f,-.8f,.5f,0xFFFFFFFF,.5f,.5f,.75f},
        { .1f, .8f,.5f,0xFFFFFFFF,.5f,.5f,.75f},
        { .9f, .8f,.5f,0xFFFFFFFF,.5f,.5f,.75f}
    };
    const std::uint16_t indices[]={0,1,2,1,3,2,4,5,6,5,7,6};
    Com<IDirect3DSurface9> back,readback;
    HRESULT hr=device->GetRenderTarget(0,&back.p);
    if(FAILED(hr))return fail("GetRenderTarget",hr);
    D3DSURFACE_DESC desc{};
    if(FAILED(hr=back->GetDesc(&desc)))return fail("GetDesc",hr);
    if(desc.Format!=D3DFMT_X8R8G8B8&&desc.Format!=D3DFMT_A8R8G8B8)
        return fail("Unsupported readback format");
    if(FAILED(hr=device->CreateOffscreenPlainSurface(desc.Width,desc.Height,desc.Format,D3DPOOL_SYSTEMMEM,&readback.p,nullptr)))
        return fail("CreateOffscreenPlainSurface",hr);
    // Read actual rendered RGB rather than merely comparing source bytes.
    const auto capture=[&](std::vector<DWORD>& image)->bool {
        if(FAILED(device->GetRenderTargetData(back.p,readback.p)))return false;
        D3DLOCKED_RECT lock{};
        if(FAILED(readback->LockRect(&lock,nullptr,D3DLOCK_READONLY)))return false;
        image.resize(std::size_t(desc.Width)*desc.Height);
        for(UINT y=0;y<desc.Height;++y)
            std::memcpy(image.data()+std::size_t(y)*desc.Width,
                static_cast<const char*>(lock.pBits)+std::size_t(y)*lock.Pitch,desc.Width*sizeof(DWORD));
        return SUCCEEDED(readback->UnlockRect());
    };
    const auto compare=[&](const char* name,const std::vector<DWORD>& expected)->bool {
        std::vector<DWORD> actual;if(!capture(actual)||actual.size()!=expected.size())return false;
        unsigned maxError=0;std::uint64_t sum=0;
        for(std::size_t i=0;i<actual.size();++i)for(int shift:{0,8,16}) {
            const unsigned diff=unsigned(std::abs(int((actual[i]>>shift)&255)-int((expected[i]>>shift)&255)));
            maxError=std::max(maxError,diff);sum+=diff;
        }
        std::printf("%s: compact vs RGBA max RGB error %u/255, mean %.4f/255\n",
            name,maxError,double(sum)/(3*actual.size()));
        return maxError<=2;
    };
    const auto exact=[&](const char* name,const std::vector<DWORD>& expected)->bool {
        std::vector<DWORD> actual;if(!capture(actual)||actual.size()!=expected.size())return false;
        unsigned differences=0;
        for(std::size_t i=0;i<actual.size();++i)differences+=((actual[i]^expected[i])&0x00FFFFFFu)!=0;
        std::printf("%s: %u differing RGB pixels\n",name,differences);
        return differences==0;
    };
    for(float coverage:{0.f,.5f,1.f}) {
        if(coverage==.5f&&!opts.dual){std::puts("No dual-volume blend support; endpoints only.");continue;}
        if(FAILED(hr=device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xFF000000,1.f,0)))
            return fail("Clear",hr);
        if(FAILED(hr=device->BeginScene()))return fail("BeginScene",hr);
        const auto result=d3d9::draw(device,textures,opts,vertices,8,indices,4,coverage,false,&transform,&transform);
        const HRESULT end=device->EndScene();
        if(!result)return fail(result.operation,result.hr);
        if(FAILED(end))return fail("EndScene",end);
        if(FAILED(hr=device->GetRenderTargetData(back.p,readback.p)))return fail("GetRenderTargetData",hr);
        D3DLOCKED_RECT lock{};
        if(FAILED(hr=readback->LockRect(&lock,nullptr,D3DLOCK_READONLY)))return fail("LockRect",hr);
        int observed[2];
        for(int i=0;i<2;++i) {
            const auto* row=static_cast<const char*>(lock.pBits)+std::size_t(desc.Height/2)*lock.Pitch;
            DWORD colour=0;
            std::memcpy(&colour,row+std::size_t(desc.Width*(i?3:1)/4)*sizeof(DWORD),sizeof(colour));
            observed[i]=colour&255;
        }
        readback->UnlockRect();
        const float alpha=vc::mix(128.f,192.f,coverage)/255.f;
        const int expected[]={int(std::round(vc::mix(64.f,128.f,coverage)*alpha)),
                              int(std::round(vc::mix(192.f,224.f,coverage)*alpha))};
        std::printf("Coverage %.2f: sampled pixels %d %d, expected %d %d\n",
            coverage,observed[0],observed[1],expected[0],expected[1]);
        if(std::abs(observed[0]-expected[0])>6||std::abs(observed[1]-expected[1])>6)
            return fail("Incorrect 3D texture or alpha/weather blend result");
    }
    // Test the actual pretransformed path with a short scene far plane:
    // far clouds survive clipping, have independent colour fog, and do
    // not punch through a foreground depth value. The native fog state is
    // deliberately incompatible; cloud rendering must override/restore it.
    D3DVIEWPORT9 viewport{};
    if(FAILED(hr=device->GetViewport(&viewport)))return fail("Pure-device GetViewport",hr);
    // Randomized triangles straddling near, viewport and scene-far
    // planes. Compare all actually referenced vertex bytes and order.
    {
        std::uint32_t seed=0x29165u;
        const auto rnd=[&](){seed=1664525u*seed+1013904223u;return float(seed>>8)/16777216.f;};
        for(int batch=0;batch<32;++batch) {
            d3d9::ProjectedMesh ref,fast;std::vector<std::uint16_t> ix;
            for(int i=0;i<2048;++i)ref.clip.push_back({(rnd()-.5f)*16,(rnd()-.5f)*16,
                (rnd()-.2f)*8,(rnd()-.2f)*8,rnd(),rnd(),rnd(),rnd(),rnd()});
            fast.clip=ref.clip;
            for(int i=0;i<8190;++i)ix.push_back(std::uint16_t(rnd()*2048));
            const RGB tint{rnd()*255,rnd()*255,rnd()*255};const float opacity=rnd();
            d3d9::projectMesh<false>(ref,viewport,tint,opacity,ix.data(),ix.size());
            d3d9::projectMesh(fast,viewport,tint,opacity,ix.data(),ix.size());
            if(ref.indices!=fast.indices||ref.masks!=fast.masks)return fail("Projection clip/order parity");
            for(auto i:ref.indices)if(std::memcmp(&ref.vertices[i],&fast.vertices[i],sizeof(d3d9::ScreenVertex)))
                return fail("Projection indexed vertex parity");
        }
        std::puts("PASS: 87360 randomized projected triangles, referenced vertices byte-exact");
    }
    d3d9::ProjectedMesh projected;
    for(int i=0;i<8;++i) {
        const auto& v=vertices[i];
        projected.clip.push_back({v.x*2,v.y*2,2.1f,2.f,v.u,v.v,v.w,1.f,i<4?1.f:0.f});
    }
    d3d9::projectMesh(projected,viewport,{255,255,255},1,indices,12);
    if(projected.indices.size()!=12||projected.vertices[0].z!=viewport.MaxZ||
        std::abs(projected.vertices[0].rhw-.5f)>.0001f)return fail("Far projection/clamp");
    if(FAILED(hr=device->SetRenderState(D3DRS_FOGENABLE,FALSE)))return fail("Initial fog state",hr);
    if(FAILED(hr=device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xFF000000,1.f,0)))return fail("Far clear",hr);
    const D3DRECT occluder{0,0,LONG(desc.Width/2),LONG(desc.Height/2)};
    if(FAILED(hr=device->Clear(1,&occluder,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xFF008000,.5f,0)))return fail("Foreground clear",hr);
    if(FAILED(hr=device->BeginScene()))return fail("Far scene begin",hr);
    auto farResult=d3d9::drawProjected(device,{textures[0],textures[1]},opts,projected,0,true,{0,0,240});
    if(!farResult)return fail(farResult.operation,farResult.hr);
    if(FAILED(hr=device->EndScene()))return fail("Far scene end",hr);
    if(FAILED(hr=device->GetRenderTargetData(back.p,readback.p)))return fail("Far readback",hr);
    D3DLOCKED_RECT farLock{};
    if(FAILED(hr=readback->LockRect(&farLock,nullptr,D3DLOCK_READONLY)))return fail("Far lock",hr);
    const auto pixel=[&](UINT x,UINT y) {
        DWORD value=0;std::memcpy(&value,static_cast<const char*>(farLock.pBits)+std::size_t(y)*farLock.Pitch+x*4,4);return value&0xFFFFFF;
    };
    const DWORD occluded=pixel(desc.Width/4,desc.Height/4),nearFog=pixel(desc.Width/4,desc.Height*3/4),
        fullFog=pixel(desc.Width*3/4,desc.Height*3/4);
    readback->UnlockRect();
    std::printf("Projected far-depth/fog: occluded=%06lX unfogged=%06lX fogged=%06lX\n",
        static_cast<unsigned long>(occluded),static_cast<unsigned long>(nearFog),static_cast<unsigned long>(fullFog));
    if(occluded!=0x008000 || std::abs(int(nearFog&255)-32)>6)return fail("Clouds beyond far plane or scene depth isolation");
    if(opts.vertexFog && (std::abs(int(fullFog&255)-120)>6 || ((fullFog>>16)&255)>6))
        return fail("Projected cloud fog not applied independently of game fog");
    // A pure device cannot use GetRenderState. Draw the far quad again
    // manually with the fog setting left exactly as restored by the scope;
    // its colour must remain grey despite its zero fog factor.
    if(FAILED(hr=device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xFF000000,1.f,0)))return fail("State clear",hr);
    if(FAILED(hr=device->BeginScene()))return fail("State begin",hr);
    if(!(creation.BehaviorFlags&D3DCREATE_PUREDEVICE)) {
        DWORD state=TRUE;
        if(FAILED(hr=device->GetRenderState(D3DRS_FOGENABLE,&state))||state!=FALSE)return fail("Fog state restore",hr);
    }
    auto set=[&](HRESULT result){if(FAILED(result)&&SUCCEEDED(hr))hr=result;};
    hr=S_OK;
    set(device->SetVertexShader(nullptr));set(device->SetPixelShader(nullptr));
    set(device->SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_SPECULAR|D3DFVF_TEX1|D3DFVF_TEXCOORDSIZE3(0)));
    set(device->SetRenderState(D3DRS_LIGHTING,FALSE));set(device->SetRenderState(D3DRS_SPECULARENABLE,FALSE));
    set(device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE));set(device->SetRenderState(D3DRS_ZENABLE,D3DZB_TRUE));
    set(device->SetRenderState(D3DRS_ZFUNC,D3DCMP_LESSEQUAL));set(device->SetRenderState(D3DRS_ZWRITEENABLE,FALSE));
    set(device->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE));set(device->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE));
    set(device->SetTexture(0,textures[0]));
    set(device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1));set(device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE));
    set(device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1));set(device->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE));
    set(device->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_DISABLE));set(device->SetTextureStageState(1,D3DTSS_ALPHAOP,D3DTOP_DISABLE));
    set(device->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_POINT));set(device->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_POINT));
    set(device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST,0,UINT(projected.vertices.size()),UINT(projected.indices.size()/3),
        projected.indices.data(),D3DFMT_INDEX16,projected.vertices.data(),sizeof(d3d9::ScreenVertex)));
    set(device->EndScene());
    if(FAILED(hr))return fail("Fog restore sentinel draw",hr);
    if(FAILED(hr=device->GetRenderTargetData(back.p,readback.p)))return fail("Fog restore readback",hr);
    if(FAILED(hr=readback->LockRect(&farLock,nullptr,D3DLOCK_READONLY)))return fail("Fog restore lock",hr);
    const DWORD restored=pixel(desc.Width*3/4,desc.Height*3/4);readback->UnlockRect();
    if(std::abs(int(restored&255)-192)>6 || std::abs(int((restored>>16)&255)-192)>6)return fail("Fog state leaked into next draw");
    std::printf("Post-restore native draw: %06lX\n",static_cast<unsigned long>(restored));
    // Also verify a normal mipmapped 2D texture uses the very same
    // independent haze and far-depth path.
    Com<IDirect3DTexture9> canopyTex;
    std::vector<canopy::Pixel> white(16*16,{200,200,200,128});
    auto canopyResult=d3d9::uploadCanopy(device,white,16,&canopyTex.p);
    if(!canopyResult)return fail(canopyResult.operation,canopyResult.hr);
    if(canopyTex->GetLevelCount()!=5)return fail("Canopy mip chain");
    if(FAILED(hr=device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xFF000000,1.f,0)))return fail("2D clear",hr);
    if(FAILED(hr=device->BeginScene()))return fail("2D begin",hr);
    auto canopyOpts=opts;canopyOpts.dual=false;
    const DWORD linear=D3DPTFILTERCAPS_MINFLINEAR|D3DPTFILTERCAPS_MAGFLINEAR;
    canopyOpts.filter=(caps.TextureFilterCaps&linear)==linear?D3DTEXF_LINEAR:D3DTEXF_POINT;
    canopyOpts.mip=(caps.TextureFilterCaps&D3DPTFILTERCAPS_MIPFLINEAR)!=0;
    canopyResult=d3d9::drawProjected(device,{canopyTex.p,canopyTex.p},canopyOpts,projected,0,false,{0,0,240});
    if(!canopyResult)return fail(canopyResult.operation,canopyResult.hr);
    if(FAILED(hr=device->EndScene()))return fail("2D end",hr);
    if(FAILED(hr=device->GetRenderTargetData(back.p,readback.p)))return fail("2D readback",hr);
    if(FAILED(hr=readback->LockRect(&farLock,nullptr,D3DLOCK_READONLY)))return fail("2D lock",hr);
    const DWORD sheet=pixel(desc.Width/4,desc.Height/2);
    readback->UnlockRect();
    if(std::abs(int(sheet&255)-100)>6)return fail("Projected canopy texture");
    // Exact grayscale + alpha textures, including alpha-weighted mipmaps.
    // Use varying luminance/alpha and enough repeats to exercise minification.
    if(d3d9::compactFormat(device,D3DRTYPE_TEXTURE,canopyOpts.filter==D3DTEXF_LINEAR||canopyOpts.mip)) {
        std::vector<canopy::Pixel> pattern(64*64);
        for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x) {
            const auto l=std::uint8_t((x*13+y*19)&255);
            pattern[y*64+x]={l,l,l,std::uint8_t((x*7+y*11)&255)};
        }
        std::array<Com<IDirect3DTexture9>,2> pair;
        for(int i=0;i<2;++i) {
            const auto result=d3d9::uploadCanopy(device,pattern,64,&pair[i].p,false,true,
                i?D3DFMT_A8L8:D3DFMT_A8R8G8B8);
            if(!result)return fail(result.operation,result.hr);
        }
        auto mipBatch=projected;
        for(auto& v:mipBatch.vertices){v.u=v.x*.15f;v.v=v.y*.15f;}
        std::vector<DWORD> reference;
        for(int i=0;i<2;++i) {
            if(FAILED(hr=device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xFF000000,1.f,0)))return fail("Compact canopy clear",hr);
            if(FAILED(hr=device->BeginScene()))return fail("Compact canopy begin",hr);
            const auto result=d3d9::drawProjected(device,{pair[i].p,pair[i].p},canopyOpts,mipBatch,0,false,{0,0,240});
            if(FAILED(hr=device->EndScene()))return fail("Compact canopy end",hr);
            if(!result)return fail(result.operation,result.hr);
            if(i?!compare("Mipmapped canopy",reference):!capture(reference))return fail("Compact canopy visual parity");
        }
    } else std::puts("A8L8 2D format/filter unavailable: RGBA fallback.");
    // Render the generated density and actual mesh through the same backend,
    // with a perspective camera near the altitude/position in the flight
    // recording. The test above isolates the sampler; this checks that the
    // full scene has both visible cloud relief and unobscured sky.
    const int uploadWidth=volume.width,uploadDepth=volume.depth;
    Settings settings;
    canopy::Map map;map.build(canopy::textureSize(settings),settings.seed);
    volume.build(settings,map);
    for(int layer=0;layer<2;++layer) {
        owner[layer].p->Release();owner[layer].p=nullptr;
        const auto result=d3d9::upload(device,volume,layer,uploadWidth,uploadDepth,&owner[layer].p);
        if(!result)return fail(result.operation,result.hr);
        textures[layer]=owner[layer].p;
    }
    std::array<Com<IDirect3DVolumeTexture9>,2> compactOwner;
    std::array<IDirect3DVolumeTexture9*,2> compactVolume{};
    D3DFORMAT volumeFormat=D3DFMT_UNKNOWN;
    const auto compactResult=d3d9::uploadPair(compactVolume,
        d3d9::compactFormat(device,D3DRTYPE_VOLUMETEXTURE,opts.filter==D3DTEXF_LINEAR),volumeFormat,
        [&](int layer,IDirect3DVolumeTexture9** out,bool managed,D3DFORMAT format) {
            return d3d9::upload(device,volume,layer,uploadWidth,uploadDepth,out,managed,format);
        });
    for(int i=0;i<2;++i)compactOwner[i].p=compactVolume[i];
    if(!compactResult)return fail(compactResult.operation,compactResult.hr);
    std::puts(volumeFormat==D3DFMT_A8L8?"Testing lossless A8L8 volume.":"A8L8 volume unavailable: testing RGBA fallback.");
    const float wx=.94f,wy=.53f;
    const Vec forward=unit(Vec{.96f,-.27f,-.17f});
    const Vec right=unit(cross(forward,{0,0,1})),up=cross(right,forward);
    d3d9::GpuStream hw,referenceHw;
#ifdef VC_GPU_VERTEX
    const bool expectHardware=caps.VertexShaderVersion>=D3DVS_VERSION(2,0)&&
        (creation.BehaviorFlags&D3DCREATE_HARDWARE_VERTEXPROCESSING);
    if(expectHardware&&!hw.create(device,caps,creation.BehaviorFlags))return fail("VS2 hardware stream creation");
    static_assert(sizeof(cloudBankReferenceVS)%sizeof(DWORD)==0);
    std::array<DWORD,sizeof(cloudBankReferenceVS)/sizeof(DWORD)> referenceCode{};
    std::memcpy(referenceCode.data(),cloudBankReferenceVS,sizeof(cloudBankReferenceVS));
    if(expectHardware&&!referenceHw.create(device,caps,creation.BehaviorFlags,referenceCode.data()))
        return fail("Reference VS2 stream creation");
    std::printf("VS2 bytecode: previous %zu bytes, new %zu bytes\n",sizeof(cloudBankReferenceVS),sizeof(cloudBankVS));
#endif
    // Ground and climb: render the actual shared-density canopy and its
    // limited nearby 3D complement with a short scene far clip.
    canopyVolume::MaskPair underside;
    canopyVolume::project(settings,volume,canopy::textureSize(settings),underside);
    canopyVolume::Relief relief;relief.build(settings,volume);
    std::array<Com<IDirect3DTexture9>,2> undersideTextures;
    for(int i=0;i<2;++i) {
        const auto result=d3d9::uploadCanopy(device,underside[i],canopy::textureSize(settings),&undersideTextures[i].p,false,true);
        if(!result)return fail(result.operation,result.hr);
    }
    auto undersideOpts=canopyOpts;undersideOpts.dual=opts.dual;
    for(float height:{30.f,443.f,535.f}) {
        const Vec pos{2239,-1262,height},f=unit(Vec{.55f,.7f,.4f}),rt=unit(cross(f,{0,0,1})),u=cross(rt,f);
        const View cam{pos,rt,u,f,450,1};
        auto view=d3d9::identity();
        view._11=rt.x;view._21=rt.y;view._31=rt.z;view._41=-dot(pos,rt);
        view._12=u.x;view._22=u.y;view._32=u.z;view._42=-dot(pos,u);
        view._13=f.x;view._23=f.y;view._33=f.z;view._43=-dot(pos,f);
        D3DMATRIX pr{};
        pr._11=1.f/wx;pr._22=1.f/wy;pr._33=cam.farClip/(cam.farClip-cam.nearClip);
        pr._34=1.f;pr._43=-cam.nearClip*pr._33;
        const auto matrix=d3d9::multiply(view,pr);
        const auto full=cloudRange(settings);
        const float handover=canopy::handover(settings,height);
        const auto local=canopyVolume::volumeRange(settings,full,height,handover);
        canopyVolume::Mesh proxy;
        canopyVolume::mesh(proxy,relief,settings,cam,full,{},wx,wy,handover,&local);
        d3d9::ProjectedMesh sheet;
        for(const auto& v:proxy.vertices) {
            const float fog=cloudFogFactor(settings,v.distance);
            sheet.clip.push_back(d3d9::clipVertex(v.pos,v.u,v.v,0.f,v.alpha*(undersideOpts.vertexFog?1.f:fog),fog,matrix));
        }
        d3d9::projectMesh(sheet,viewport,{208,208,208},1.f,proxy.indices.data(),proxy.indices.size());
        std::vector<DWORD> clear;
        const D3DRECT depthSentinel{0,0,LONG(desc.Width/4),LONG(desc.Height/4)};
        for(float coverage:{0.f,1.f}) {
            banks::Mesh close;
            banks::buildMesh(close,settings,cam,local,{},wx,wy,&volume,coverage,false);
            d3d9::ProjectedMesh nearBatch;
            for(const auto& v:close.vertices)
                nearBatch.clip.push_back(d3d9::clipVertex(v.pos,v.u,v.v,v.w,v.alpha*(opts.vertexFog?1.f:v.fog),v.fog,matrix));
            d3d9::projectMesh(nearBatch,viewport,{208,208,208},1.f,close.indices.data(),close.indices.size());
            if(FAILED(hr=device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xFF000000,1.f,0)))return fail("Shared canopy clear",hr);
            if(FAILED(hr=device->Clear(1,&depthSentinel,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xFF008000,.0001f,0)))return fail("Shared canopy depth",hr);
            if(FAILED(hr=device->BeginScene()))return fail("Shared canopy begin",hr);
            d3d9::StateScope scope;if(!scope.capture(device))return fail("Shared canopy capture");
            d3d9::DrawState states;
            const auto c=d3d9::drawProjected(device,{undersideTextures[0].p,undersideTextures[1].p},undersideOpts,
                sheet,coverage,false,{90,120,160},true,false,&states);
            const auto b=d3d9::drawProjected(device,{compactVolume[0],compactVolume[1]},opts,
                nearBatch,coverage,true,{90,120,160},true,false,&states);
            if(!scope.restore())return fail("Shared canopy restore");
            if(FAILED(hr=device->EndScene()))return fail("Shared canopy end",hr);
            if(!c||!b)return fail("Shared canopy draw");
            std::vector<DWORD> actual;if(!capture(actual))return fail("Shared canopy readback");
            unsigned visible=0;
            for(UINT y=0;y<desc.Height;++y)for(UINT x=0;x<desc.Width;++x) {
                const auto pixel=actual[std::size_t(y)*desc.Width+x]&0xFFFFFFu;
                if(x<desc.Width/4&&y<desc.Height/4) {
                    if(pixel!=0x008000u)return fail("Shared canopy covers foreground");
                } else visible+=(pixel&255)>3;
            }
            if(visible<16)return fail("Shared canopy/near volume not visible");
            if(coverage==0)clear=std::move(actual);
            else if(clear==actual)return fail("Shared canopy weather endpoints are identical");
            std::printf("Shared canopy Z %.0f coverage %.0f: %zu canopy triangles, %zu near volume triangles, %u visible RGB pixels\n",
                height,coverage,proxy.indices.size()/3,close.indices.size()/3,visible);
        }
    }
    for(const Vec pos:{Vec{2164,-1265,673},Vec{2255,-1341,672.7f},Vec{2266,-1271,490}}) {
        View camera{pos,right,up,forward,450,1};
        auto view=d3d9::identity();
        view._11=right.x;view._21=right.y;view._31=right.z;view._41=-dot(pos,right);
        view._12=up.x;view._22=up.y;view._32=up.z;view._42=-dot(pos,up);
        view._13=forward.x;view._23=forward.y;view._33=forward.z;view._43=-dot(pos,forward);
        D3DMATRIX projection{};
        projection._11=1.f/wx;projection._22=1.f/wy;
        projection._33=camera.farClip/(camera.farClip-camera.nearClip);
        projection._34=1;projection._43=-camera.nearClip*projection._33;
        const float coverage=opts.dual?(pos.z<500?1.f:pos.x>2200?.5f:0.f):0.f;
        banks::Mesh mesh;banks::buildMesh(mesh,settings,camera,cloudRange(settings),{},wx,wy,
            uploadWidth==volume.width&&uploadDepth==volume.depth?&volume:nullptr,coverage);
        if(mesh.indices.empty())return fail("No occupied 3D geometry in flight view");
        d3d9::ProjectedMesh batch;const auto matrix=d3d9::multiply(view,projection);
        for(const auto& v:mesh.vertices)batch.clip.push_back(d3d9::clipVertex(v.pos,v.u,v.v,v.w,v.alpha*(opts.vertexFog?1.f:v.fog),v.fog,matrix));
        d3d9::projectMesh(batch,viewport,{208,208,208},1.f,mesh.indices.data(),mesh.indices.size());
        if(FAILED(hr=device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xFF000000,1.f,0)))return fail("Scene Clear",hr);
        const D3DRECT sentinel{0,0,LONG(desc.Width/4),LONG(desc.Height/4)};
        if(FAILED(hr=device->Clear(1,&sentinel,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xFF008000,.5f,0)))return fail("Scene occlusion sentinel",hr);
        if(FAILED(hr=device->BeginScene()))return fail("Scene BeginScene",hr);
        const auto result=d3d9::drawProjected(device,{textures[0],textures[1]},opts,batch,coverage,true,{90,120,160});
        const auto end=device->EndScene();
        if(!result)return fail(result.operation,result.hr);
        if(FAILED(end))return fail("Scene EndScene",end);
        if(FAILED(hr=device->GetRenderTargetData(back.p,readback.p)))return fail("Scene readback",hr);
        D3DLOCKED_RECT lock{};
        if(FAILED(hr=readback->LockRect(&lock,nullptr,D3DLOCK_READONLY)))return fail("Scene LockRect",hr);
        unsigned visible=0,empty=0,peak=0;
        std::vector<DWORD> reference(std::size_t(desc.Width)*desc.Height);
        for(UINT y=0;y<desc.Height;++y)for(UINT x=0;x<desc.Width;++x) {
            DWORD colour=0;
            std::memcpy(&colour,static_cast<const char*>(lock.pBits)+std::size_t(y)*lock.Pitch+x*sizeof(DWORD),sizeof(DWORD));
            reference[std::size_t(y)*desc.Width+x]=colour;
            const unsigned value=colour&255;
            visible+=value>8;empty+=value<=2;peak=std::max(peak,value);
        }
        readback->UnlockRect();
        std::printf("Flight scene x=%.1f y=%.1f z=%.1f: %d fixed slice patches, %u visible pixels, %u clear pixels, peak %u\n",
            pos.x,pos.y,pos.z,mesh.slices,visible,empty,peak);
        if(visible<desc.Width*desc.Height/64 || (pos.z>settings.maxSpawnZ&&empty<desc.Width*desc.Height/64) || peak<25 || peak>209)
            return fail("Flight scene readback has no cloud/sky contrast");
        if(FAILED(hr=device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xFF000000,1.f,0)))return fail("Compact scene clear",hr);
        if(FAILED(hr=device->Clear(1,&sentinel,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xFF008000,.5f,0)))return fail("Compact scene sentinel",hr);
        if(FAILED(hr=device->BeginScene()))return fail("Compact scene begin",hr);
        const auto compactDraw=d3d9::drawProjected(device,{compactVolume[0],compactVolume[1]},opts,batch,coverage,true,{90,120,160});
        if(FAILED(hr=device->EndScene()))return fail("Compact scene end",hr);
        if(!compactDraw)return fail(compactDraw.operation,compactDraw.hr);
        if(!compare("Volume flight scene",reference))return fail("Compact volume visual parity");
        if(hw.available()) {
            banks::GpuMesh gm;banks::buildMesh(gm,settings,camera,cloudRange(settings),{},wx,wy,
                uploadWidth==volume.width&&uploadDepth==volume.depth?&volume:nullptr,coverage);
            if(gm.triangleCount()!=mesh.indices.size()/3||gm.vertices.size()!=mesh.vertices.size())return fail("Hardware and CPU source geometry differ");
            for(std::size_t i=0;i<mesh.vertices.size();++i)
                if(length(gm.vertices[i].pos-mesh.vertices[i].pos)>.001f)return fail("Hardware and CPU world lattice differs");
            d3d9::GpuPasses passes;passes.stream=&hw;
            if(!passes.prepare(settings,camera,cloudRange(settings),{},true,opts.vertexFog,1,{208,208,208},matrix,projection,viewport))
                return fail("Hardware projection setup");
            const auto upload=hw.upload(gm);if(!upload)return fail(upload.operation,upload.hr);
            if(FAILED(hr=device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xFF000000,1.f,0)))return fail("HW scene clear",hr);
            if(FAILED(hr=device->Clear(1,&sentinel,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xFF008000,.5f,0)))return fail("HW occlusion sentinel",hr);
            if(FAILED(hr=device->BeginScene()))return fail("HW scene begin",hr);
            const auto draw=d3d9::drawRaw(device,{textures[0],textures[1]},opts,gm.vertices.data(),UINT(gm.vertices.size()),
                sizeof(banks::GpuVertex),0,nullptr,UINT(gm.triangleCount()),coverage,opts.vertexFog,true,false,
                D3DCOLOR_XRGB(90,120,160),nullptr,nullptr,true,&passes);
            if(FAILED(hr=device->EndScene()))return fail("HW scene end",hr);
            if(!draw)return fail(draw.operation,draw.hr);
            if(FAILED(hr=device->GetRenderTargetData(back.p,readback.p)))return fail("HW readback",hr);
            if(FAILED(hr=readback->LockRect(&lock,nullptr,D3DLOCK_READONLY)))return fail("HW lock",hr);
            std::uint64_t error=0;unsigned large=0;
            for(UINT y=0;y<desc.Height;++y)for(UINT x=0;x<desc.Width;++x) {
                DWORD colour=0;std::memcpy(&colour,static_cast<const char*>(lock.pBits)+std::size_t(y)*lock.Pitch+x*4,4);
                const auto old=reference[std::size_t(y)*desc.Width+x];
                if(x<desc.Width/4&&y<desc.Height/4&&(colour&0xFFFFFF)!=(old&0xFFFFFF)){
                    readback->UnlockRect();return fail("Hardware path leaked past foreground sentinel");
                }
                int maxError=0;
                for(int shift:{0,8,16}){const int diff=std::abs(int((colour>>shift)&255)-int((old>>shift)&255));error+=diff;maxError=std::max(maxError,diff);}
                large+=maxError>24;
            }
            readback->UnlockRect();
            const float mean=float(error)/(3*desc.Width*desc.Height);
            std::printf("Hardware vertex readback: mean RGB error %.3f /255, %u pixels with >24 error\n",mean,large);
            if(mean>6.f||large>desc.Width*desc.Height/12)return fail("Hardware vertex visual parity");
            // Compare actual clip-range submissions with the old full two
            // passes, including a short map clip and an extended modded clip.
            const auto renderHardware=[&](d3d9::GpuPasses& selected)->bool {
                if(FAILED(device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xFF000000,1.f,0)))return false;
                if(FAILED(device->Clear(1,&sentinel,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xFF008000,.5f,0)))return false;
                if(FAILED(device->BeginScene()))return false;
                const auto result=d3d9::drawRaw(device,{textures[0],textures[1]},opts,gm.vertices.data(),UINT(gm.vertices.size()),
                    sizeof(banks::GpuVertex),0,nullptr,UINT(gm.triangleCount()),coverage,opts.vertexFog,true,false,
                    D3DCOLOR_XRGB(90,120,160),nullptr,nullptr,true,&selected);
                return SUCCEEDED(device->EndScene())&&bool(result);
            };
            if(!referenceHw.available()||!referenceHw.upload(gm))return fail("Reference shader upload");
            std::size_t saved=0;
            for(float sceneDistance:{75.f,450.f,1800.f,12000.f}) {
                auto pr=projection;
                pr._33=sceneDistance/(sceneDistance-camera.nearClip);
                pr._43=-camera.nearClip*pr._33;
                const auto mat=d3d9::multiply(view,pr);
                d3d9::GpuPasses trimmed;trimmed.stream=&hw;
                if(!trimmed.prepare(settings,camera,cloudRange(settings),{},true,opts.vertexFog,1,{208,208,208},mat,pr,viewport))return fail("Trim projection setup");
                if(!renderHardware(trimmed))return fail("Full two-pass reference");
                std::vector<DWORD> full;if(!capture(full))return fail("Full two-pass capture");
                trimmed.restrictTo(gm);
                if(sceneDistance==450.f) {
                    banks::GpuMesh coarse;
                    banks::buildMesh(coarse,settings,camera,cloudRange(settings),{},wx,wy,
                        uploadWidth==volume.width&&uploadDepth==volume.depth?&volume:nullptr,coverage,true,true,false);
                    if(!hw.upload(coarse))return fail("Coarse occupancy reference upload");
                    auto old=trimmed;old.restrictedCount=0;
                    if(FAILED(device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xFF000000,1.f,0)))return fail("Coarse occupancy clear");
                    if(FAILED(device->Clear(1,&sentinel,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xFF008000,.5f,0)))return fail("Coarse occupancy sentinel");
                    if(FAILED(device->BeginScene()))return fail("Coarse occupancy begin");
                    const auto oldDraw=d3d9::drawRaw(device,{textures[0],textures[1]},opts,nullptr,UINT(coarse.vertices.size()),
                        sizeof(banks::GpuVertex),0,nullptr,UINT(coarse.triangleCount()),coverage,opts.vertexFog,true,false,
                        D3DCOLOR_XRGB(90,120,160),nullptr,nullptr,true,&old);
                    if(FAILED(device->EndScene())||!oldDraw)return fail("Coarse occupancy draw");
                    if(!exact("Precise occupancy GPU readback",full))return fail("Precise occupancy altered visible pixels");
                    if(!hw.upload(gm))return fail("Restore precise occupancy VB");
                }
                const auto submitted=trimmed.ranges[0].count()+trimmed.ranges[1].count();
                saved+=2*gm.vertices.size()-submitted;
                if(!renderHardware(trimmed))return fail("Range-trimmed draw");
                std::printf("Scene clip %.0f: %u of %zu two-pass input vertices\n",sceneDistance,submitted,2*gm.vertices.size());
                if(!exact("Range-trimmed two-pass draw",full))return fail("Visual regression from pass range trimming");
                trimmed.stream=&referenceHw;
                if(!renderHardware(trimmed)||!exact("Simplified lattice shader",full))return fail("VS2 parity simplification changed RGB");
                trimmed.stream=&hw;
                if(sceneDistance==450.f) {
                    d3d9::FarLayer half;
                    trimmed.farLayer=&half;
                    if(!renderHardware(trimmed))return fail("Half-resolution distant draw");
                    if(trimmed.usedFarLayer) {
                        std::vector<DWORD> reduced;if(!capture(reduced))return fail("Half-resolution readback");
                        std::uint64_t total=0;unsigned large=0;
                        for(UINT y=0;y<desc.Height;++y)for(UINT x=0;x<desc.Width;++x) {
                            const auto i=std::size_t(y)*desc.Width+x;
                            if(x<desc.Width/4&&y<desc.Height/4&&(reduced[i]&0xFFFFFFu)!=(full[i]&0xFFFFFFu))
                                return fail("Half-resolution clouds leaked past foreground depth");
                            unsigned maximum=0;
                            for(int shift:{0,8,16}) {
                                const auto diff=unsigned(std::abs(int((reduced[i]>>shift)&255)-int((full[i]>>shift)&255)));
                                total+=diff;maximum=std::max(maximum,diff);
                            }
                            large+=maximum>32;
                        }
                        const double mean=double(total)/(3*reduced.size());
                        std::printf("Half-resolution distant layer: mean RGB delta %.3f/255; %u pixels >32; native depth sentinel preserved\n",mean,large);
                        if(mean>10.||large>reduced.size()/8)return fail("Half-resolution distant visual deviation");
                    } else std::puts("Half-resolution layer unavailable on this device/format; native fallback exercised.");
                    trimmed.farLayer=nullptr;
                    if(!renderHardware(trimmed)||!exact("Far layer restoration",full))return fail("Far-layer state/target restoration");
                }
                // Reuse a resident VB only for an exactly equal source mesh.
                // Exercise both the first DISCARD and the no-lock path.
                auto temp=gm;bool reused=false;
                if(!hw.uploadReusable(temp,&reused))return fail("Reusable VB upload");
                temp=gm;
                if(!hw.uploadReusable(temp,&reused)||!reused)return fail("Identical VB not reused");
                if(!renderHardware(trimmed)||!exact("Resident VB reuse",full))return fail("Resident VB changed RGB");
                // A one-bit lattice/position edit must invalidate reuse.
                temp=gm;temp.vertices[0].lattice^=1u;
                if(!hw.uploadReusable(temp,&reused)||reused)return fail("Changed VB incorrectly reused");
                if(!hw.upload(gm))return fail("Restore reference VB");
                if(sceneDistance==12000.f&&trimmed.ranges[0].count()!=0)return fail("Failed to omit empty distant pass");
            }
            if(!saved)return fail("Pass range trimming failed to reduce submissions");
        }
    }
    // Test the actual distant atlas + projected card pipeline on the pure
    // device, with far fog and weather blending. Only geometry beyond the
    // 5200m LOD start is submitted; scene far clip remains 450m.
    billboards::Atlas atlas;atlas.build(settings,volume);
    std::array<Com<IDirect3DTexture9>,2> farTextures;
    for(int layer=0;layer<2;++layer) {
        auto result=d3d9::uploadCanopy(device,atlas.pixels[layer],billboards::AtlasSize,&farTextures[layer].p,false,false);
        if(!result)return fail(result.operation,result.hr);
        if(farTextures[layer]->GetLevelCount()!=1)return fail("Atlas must not allocate bleeding mip levels");
    }
    std::array<Com<IDirect3DTexture9>,2> compactFarOwner;
    std::array<IDirect3DTexture9*,2> compactFar{};
    D3DFORMAT atlasFormat=D3DFMT_UNKNOWN;
    const auto atlasUpload=d3d9::uploadPair(compactFar,
        d3d9::compactFormat(device,D3DRTYPE_TEXTURE,canopyOpts.filter==D3DTEXF_LINEAR),atlasFormat,
        [&](int layer,IDirect3DTexture9** out,bool managed,D3DFORMAT format) {
            return d3d9::uploadCanopy(device,atlas.pixels[layer],billboards::AtlasSize,out,managed,false,format);
        });
    for(int i=0;i<2;++i)compactFarOwner[i].p=compactFar[i];
    if(!atlasUpload)return fail(atlasUpload.operation,atlasUpload.hr);
    std::puts(atlasFormat==D3DFMT_A8L8?"Testing lossless A8L8 atlas.":"A8L8 atlas unavailable; testing RGBA fallback.");
    View distant{{2164,-1265,490},{1,0,0},{0,0,1},{0,1,0},450,1};
    banks::Mesh cards;billboards::buildMesh(cards,settings,distant,cloudRange(settings),{},atlas,wx,wy,.5f);
    if(cards.indices.empty())return fail("No far LOD geometry");
    auto v=d3d9::identity();v._11=1;v._22=0;v._23=1;v._32=1;v._33=0;
    v._41=-distant.pos.x;v._42=-distant.pos.z;v._43=-distant.pos.y;
    D3DMATRIX proj{};proj._11=1.f/wx;proj._22=1.f/wy;proj._33=450.f/449.f;proj._34=1;proj._43=-proj._33;
    const auto transformFar=d3d9::multiply(v,proj);
    d3d9::ProjectedMesh farBatch;
    for(const auto& p:cards.vertices)farBatch.clip.push_back(d3d9::clipVertex(p.pos,p.u,p.v,p.w,p.alpha,p.fog,transformFar));
    d3d9::projectMesh(farBatch,viewport,{240,240,240},1.f,cards.indices.data(),cards.indices.size());
    if(farBatch.indices.empty())return fail("Far LOD clipped at scene far plane");
    auto atlasOpts=canopyOpts;atlasOpts.dual=opts.dual;atlasOpts.mip=false;
    for(float coverage:{0.f,.5f,1.f}) {
        if(FAILED(hr=device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xFF000000,1.f,0)))return fail("Atlas clear",hr);
        if(FAILED(hr=device->BeginScene()))return fail("Atlas begin",hr);
        auto result=d3d9::drawProjected(device,{farTextures[0].p,farTextures[1].p},atlasOpts,farBatch,coverage,false,{90,120,160});
        if(FAILED(hr=device->EndScene()))return fail("Atlas end",hr);
        if(!result)return fail(result.operation,result.hr);
        if(FAILED(hr=device->GetRenderTargetData(back.p,readback.p)))return fail("Atlas readback",hr);
        D3DLOCKED_RECT lock{};
        if(FAILED(hr=readback->LockRect(&lock,nullptr,D3DLOCK_READONLY)))return fail("Atlas lock",hr);
        unsigned visible=0;
        for(UINT y=0;y<desc.Height;++y)for(UINT x=0;x<desc.Width;++x) {
            DWORD colour=0;std::memcpy(&colour,static_cast<const char*>(lock.pBits)+std::size_t(y)*lock.Pitch+x*4,4);
            visible+=(colour&255)>2;
        }
        readback->UnlockRect();
        std::printf("Far billboard coverage %.2f: %zu cards, %u visible pixels beyond scene far plane\n",coverage,cards.vertices.size()/4,visible);
        if(visible<16)return fail("Far LOD not visible on real D3D9 device");
        std::vector<DWORD> reference;
        if(!capture(reference))return fail("Atlas capture");
        if(FAILED(hr=device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xFF000000,1.f,0)))return fail("Compact atlas clear",hr);
        if(FAILED(hr=device->BeginScene()))return fail("Compact atlas begin",hr);
        result=d3d9::drawProjected(device,{compactFar[0],compactFar[1]},atlasOpts,farBatch,coverage,false,{90,120,160});
        if(FAILED(hr=device->EndScene()))return fail("Compact atlas end",hr);
        if(!result)return fail(result.operation,result.hr);
        if(!compare("Distant cloud atlas",reference))return fail("Compact atlas visual parity");
    }
    {
        const Vec f=unit(Vec{0,1,.38f}),right=unit(cross(f,{0,0,1})),up=cross(right,f);
        const View ground{{2266,-1271,30},right,up,f,450,1};
        const auto r=cloudRange(settings);
        const auto lod=banks::hybridLod(settings,r,ground.pos.z,ground.pos.z);
        banks::Mesh groundCards;
        billboards::buildMesh(groundCards,settings,ground,r,{},atlas,wx,wy,.5f,&lod);
        auto camera=d3d9::identity();
        camera._11=right.x;camera._21=right.y;camera._31=right.z;camera._41=-dot(ground.pos,right);
        camera._12=up.x;camera._22=up.y;camera._32=up.z;camera._42=-dot(ground.pos,up);
        camera._13=f.x;camera._23=f.y;camera._33=f.z;camera._43=-dot(ground.pos,f);
        const auto mat=d3d9::multiply(camera,proj);
        d3d9::ProjectedMesh batch;
        for(const auto& p:groundCards.vertices)batch.clip.push_back(d3d9::clipVertex(p.pos,p.u,p.v,p.w,p.alpha,p.fog,mat));
        d3d9::projectMesh(batch,viewport,{240,240,240},1.f,groundCards.indices.data(),groundCards.indices.size());
        if(batch.indices.empty())return fail("No directional ground canopy");
        const D3DRECT sentinel{0,0,LONG(desc.Width/4),LONG(desc.Height/4)};
        d3d9::StateScope scope;d3d9::FarLayer layer;
        std::vector<DWORD> full,nested,nestedSequence;
        bool nestedUsed=false,sequenceUsed=false;
        for(int half=0;half<5;++half) {
            if(FAILED(device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xFF50779F,1.f,0)))return fail("Directional ground clear");
            if(FAILED(device->Clear(1,&sentinel,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xFF008000,.1f,0)))return fail("Directional ground sentinel");
            if(FAILED(device->BeginScene())||!scope.capture(device))return fail("Directional ground begin");
            bool used=false;d3d9::DrawState cache;
            const auto result=d3d9::drawFarProjected(device,{compactFar[0],compactFar[1]},atlasOpts,batch,.5f,
                {90,120,160},viewport,half?&layer:nullptr,&cache,&used,half==1||half==3);
            if(!result)return fail("Directional ground draw",result.hr);
            // Draw another full native proxy after the reduced pass. The
            // outer-owned path must correctly re-emit affected state.
            if(half>=3) {
                const auto next=d3d9::drawProjected(device,{compactFar[0],compactFar[1]},atlasOpts,batch,.45f,false,
                    {90,120,160},true,false,&cache);
                if(!next)return fail("Directional ground following draw",next.hr);
            }
            if(!scope.restore()||FAILED(device->EndScene())||!result)return fail("Directional ground draw");
            std::vector<DWORD> pixels;if(!capture(pixels))return fail("Directional ground capture");
            if(!half){full=std::move(pixels);continue;}
            if(half==1){nested=pixels;nestedUsed=used;}
            else if(half==3){nestedSequence=std::move(pixels);sequenceUsed=used;continue;}
            else {
                const auto& expected=half==2?nested:nestedSequence;
                if(used!=(half==2?nestedUsed:sequenceUsed))return fail("Outer-owned state scope path parity");
                for(std::size_t i=0;i<pixels.size();++i)if((pixels[i]&0xFFFFFFu)!=(expected[i]&0xFFFFFFu))
                    return fail("Outer-owned state scope visual parity");
                if(half==4)std::puts("PASS: nested versus outer-only state scope RGB exact, including following native draw");
                continue;
            }
            unsigned large=0;std::uint64_t error=0;
            for(UINT y=0;y<desc.Height;++y)for(UINT x=0;x<desc.Width;++x) {
                const auto i=std::size_t(y)*desc.Width+x;
                if(x<desc.Width/4&&y<desc.Height/4&&(pixels[i]&0xFFFFFFu)!=(full[i]&0xFFFFFFu))
                    return fail("Reduced directional proxy crossed foreground depth");
                int maximum=0;
                for(int shift:{0,8,16}) {
                    const int e=std::abs(int((pixels[i]>>shift)&255)-int((full[i]>>shift)&255));
                    error+=e;maximum=std::max(maximum,e);
                }
                large+=maximum>32;
            }
            const double mean=double(error)/(3*pixels.size());
            std::printf("Directional canopy half-res: used=%d, mean RGB delta %.3f/255, %u pixels >32; native depth preserved\n",int(used),mean,large);
            if((used&&(mean>5.||large>pixels.size()/16))||(!used&&error))return fail("Directional proxy visual deviation");
        }
    }
    // Compare a complete mixed sequence with and without the within-scope
    // state cache. Includes stage 1/2 enable/disable, filter/fog toggles,
    // different texture types and repeated coverage. Never use state getters.
    std::vector<DWORD> uncached;
    d3d9::StateScope sequenceScope;
    for(int fast=0;fast<2;++fast) {
        if(FAILED(hr=device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xFF000000,1.f,0)))return fail("State sequence clear",hr);
        if(FAILED(hr=device->BeginScene()))return fail("State sequence begin",hr);
        // Different game state on each frame. A stale snapshot must not
        // survive recapture, even on a pure device.
        const DWORD mask=fast?D3DCOLORWRITEENABLE_RED:D3DCOLORWRITEENABLE_GREEN;
        if(FAILED(hr=device->SetRenderState(D3DRS_COLORWRITEENABLE,mask)))return fail("State sequence sentinel",hr);
        if(!sequenceScope.capture(device))return fail("State sequence capture");
        d3d9::DrawState states;
        auto* cache=fast?&states:nullptr;
        for(int i=0;i<2;++i) {
            const auto result=d3d9::drawProjected(device,{canopyTex.p,canopyTex.p},canopyOpts,projected,0,false,{90,120,160},true,false,cache);
            if(!result)return fail(result.operation,result.hr);
        }
        for(float coverage:{.5f,1.f,0.f,.25f}) {
            auto result=d3d9::drawProjected(device,{farTextures[0].p,farTextures[1].p},atlasOpts,farBatch,coverage,false,{90,120,160},true,false,cache);
            if(!result)return fail(result.operation,result.hr);
            result=d3d9::drawProjected(device,{textures[0],textures[1]},opts,projected,coverage,true,{90,120,160},false,false,cache);
            if(!result)return fail(result.operation,result.hr);
        }
        if(!sequenceScope.restore())return fail("State sequence restore");
        if(!(creation.BehaviorFlags&D3DCREATE_PUREDEVICE)) {
            DWORD restoredMask=0;
            if(FAILED(hr=device->GetRenderState(D3DRS_COLORWRITEENABLE,&restoredMask))||restoredMask!=mask)
                return fail("Recaptured game write mask not restored",hr);
        }
        if(FAILED(hr=device->EndScene()))return fail("State sequence end",hr);
        if(fast) {
            if(!exact("Scoped render-state deduplication",uncached))return fail("State deduplication visual regression");
            std::printf("State calls: %u of %u sent, %u skipped\n",states.calls,states.calls+states.skipped,states.skipped);
            if(states.skipped<=states.calls)return fail("State deduplication failed to remove repeated calls");
        } else if(!capture(uncached))return fail("Uncached state sequence readback");
        // The colour write mask is not touched by this native sentinel.
        // It can test the restored state with no forbidden pure-device gets.
        if(FAILED(hr=device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xFF000000,1.f,0)))return fail("Snapshot sentinel clear",hr);
        if(FAILED(hr=device->BeginScene()))return fail("Snapshot sentinel begin",hr);
        set(device->SetVertexShader(nullptr));set(device->SetPixelShader(nullptr));
        set(device->SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE));
        set(device->SetRenderState(D3DRS_LIGHTING,FALSE));set(device->SetRenderState(D3DRS_FOGENABLE,FALSE));
        set(device->SetRenderState(D3DRS_ZENABLE,FALSE));set(device->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE));
        set(device->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE));set(device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE));
        set(device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1));
        set(device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_DIFFUSE));
        set(device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1));
        set(device->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE));
        set(device->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_DISABLE));
        struct Screen {float x,y,z,w;DWORD rgba;};
        const Screen tri[]={{0,0,.5f,1,0xFFFFFFFF},{float(desc.Width),0,.5f,1,0xFFFFFFFF},{0,float(desc.Height),.5f,1,0xFFFFFFFF}};
        set(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST,1,tri,sizeof(Screen)));
        set(device->EndScene());
        if(FAILED(hr))return fail("Snapshot sentinel draw",hr);
        std::vector<DWORD> native;if(!capture(native))return fail("Snapshot sentinel capture");
        const auto value=native[std::size_t(desc.Height/4)*desc.Width+desc.Width/4]&0xFFFFFFu;
        if(value!=(fast?0xFF0000u:0x00FF00u))return fail("State block recapture leaked game state");
        std::printf("Refreshed state block native sentinel: %06lX\n",static_cast<unsigned long>(value));
    }
    return 0;
}
}
int main(int argc,char** argv) {
    const bool referenceBench=argc==4&&std::strcmp(argv[1],"--benchmark-half-reference")==0;
    const bool halfBench=referenceBench||(argc==4&&std::strcmp(argv[1],"--benchmark-half")==0);
    const bool bench=halfBench||(argc==4&&std::strcmp(argv[1],"--benchmark")==0);
    const UINT width=bench?UINT(std::atoi(argv[2])):320u,height=bench?UINT(std::atoi(argv[3])):240u;
    if(!width||!height||width>1920||height>1080)return 1;
    const HWND window=CreateWindowExA(0,"STATIC","D3D9 volume test",WS_OVERLAPPEDWINDOW,
        0,0,128,128,nullptr,nullptr,GetModuleHandleA(nullptr),nullptr);
    if(!window){std::puts("SKIP: cannot create test window.");return 77;}
    struct Window {HWND w;~Window(){DestroyWindow(w);}} cleanup{window};
    Com<IDirect3D9> api;api.p=Direct3DCreate9(D3D_SDK_VERSION);
    if(!api.p){std::puts("SKIP: no Direct3D9 runtime.");return 77;}
    // First reproduce the exact pure-device creation flags used by SA.
    // Non-pure HAL and software REF are secondary coverage, not proof of PURE.
    bool any=false,pure=false;
    for(auto type:{D3DDEVTYPE_HAL,D3DDEVTYPE_REF}) {
        if(bench&&type==D3DDEVTYPE_REF)continue;
        D3DCAPS9 caps{};
        if(FAILED(api->GetDeviceCaps(D3DADAPTER_DEFAULT,type,&caps)))continue;
        for(DWORD flags:{DWORD(D3DCREATE_HARDWARE_VERTEXPROCESSING|D3DCREATE_PUREDEVICE),
                         DWORD(D3DCREATE_HARDWARE_VERTEXPROCESSING),
                         DWORD(D3DCREATE_SOFTWARE_VERTEXPROCESSING)}) {
            D3DPRESENT_PARAMETERS pp{};
            pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=window;
            pp.BackBufferWidth=width;pp.BackBufferHeight=height;pp.BackBufferFormat=D3DFMT_X8R8G8B8;
            pp.EnableAutoDepthStencil=TRUE;pp.AutoDepthStencilFormat=D3DFMT_D16;
            Com<IDirect3DDevice9> device;
            const auto hr=api->CreateDevice(D3DADAPTER_DEFAULT,type,window,flags,&pp,&device.p);
            if(FAILED(hr)) {
                std::printf("CreateDevice type=%d flags=0x%lx unavailable: 0x%08lX\n",
                    int(type),static_cast<unsigned long>(flags),static_cast<unsigned long>(hr));
                continue;
            }
            D3DCAPS9 actual{};
            if(FAILED(device->GetDeviceCaps(&actual)))return fail("GetDeviceCaps");
            const int result=bench?(referenceBench?synthetic::benchmark<false>(device.p,actual,halfBench):synthetic::benchmark(device.p,actual,halfBench)):run(device.p,actual);
            if(result==77)continue;
            if(result!=0)return result;
            any=true;pure|=(flags&D3DCREATE_PUREDEVICE)!=0;
            if(pure)break;
        }
        if(any)break;
    }
    if(!any){std::puts("SKIP: no available D3D9 volume-capable device.");return 77;}
    if(bench)return 0;
    std::puts(pure?"PASS: actual pure-device 3D texture rendering/readback.":"PASS: actual non-pure 3D rendering/readback; PURE was unavailable.");
    return 0;
}
