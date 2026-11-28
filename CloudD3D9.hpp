#pragma once
// Standalone D3D9 volume backend, also compiled by the Win32 device test.
// No RenderWare dependency, pixel shaders, or direct pipeline-state getters.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d9.h>
#include <cstring>
#include <bitset>
#include "CloudBanks.hpp"
#ifdef VC_GPU_VERTEX
#include "CloudBankVS.hpp"
#endif

namespace vc::d3d9 {
struct Result {
    HRESULT hr{S_OK};
    const char* operation{"ok"};
    explicit operator bool() const {return SUCCEEDED(hr);}
};
inline Result failed(HRESULT hr,const char* operation) {return {hr,operation};}
struct Vertex {float x,y,z;DWORD rgba;float u,v,w;};
static_assert(sizeof(Vertex)==28);
inline std::uint8_t byte(float x) {return canopy::byte(x);}
inline D3DMATRIX identity() {
    D3DMATRIX m{};m._11=m._22=m._33=m._44=1.f;return m;
}
// Density/shading textures in mode 2 are greyscale + 8-bit opacity.
// A8L8 expands luminance to RGB at sampling time. Unlike DXT compression,
// this retains all 256 possible values in both channels.
inline bool compactFormat(IDirect3DDevice9* device,D3DRESOURCETYPE type,bool filter) {
    D3DDEVICE_CREATION_PARAMETERS creation{};
    if(FAILED(device->GetCreationParameters(&creation)))return false;
    IDirect3D9* api=nullptr;
    if(FAILED(device->GetDirect3D(&api))||!api){if(api)api->Release();return false;}
    D3DDISPLAYMODE mode{};
    const bool result=SUCCEEDED(api->GetAdapterDisplayMode(creation.AdapterOrdinal,&mode))&&
        SUCCEEDED(api->CheckDeviceFormat(creation.AdapterOrdinal,creation.DeviceType,mode.Format,0,type,D3DFMT_A8L8))&&
        (!filter||SUCCEEDED(api->CheckDeviceFormat(creation.AdapterOrdinal,creation.DeviceType,mode.Format,
            D3DUSAGE_QUERY_FILTER,type,D3DFMT_A8L8)));
    api->Release();return result;
}
inline UINT texelBytes(D3DFORMAT format) {return format==D3DFMT_A8L8?2u:4u;}
inline bool putTexel(char* dst,canopy::Pixel p,D3DFORMAT format) {
    if(format==D3DFMT_A8L8) {
        // Never silently convert a future non-greyscale bake.
        if(p.r!=p.g||p.r!=p.b)return false;
        const std::uint16_t al=std::uint16_t((unsigned(p.a)<<8)|p.r);
        std::memcpy(dst,&al,sizeof(al));
    } else {
        const DWORD argb=D3DCOLOR_ARGB(p.a,p.r,p.g,p.b);
        std::memcpy(dst,&argb,sizeof(argb));
    }
    return true;
}
inline Result fill(IDirect3DVolumeTexture9* texture,const banks::Volume& volume,int layer,UINT w,UINT d,
                   D3DFORMAT format=D3DFMT_A8R8G8B8) {
    D3DLOCKED_BOX lock{};
    const auto hr=texture->LockBox(0,&lock,nullptr,0);
    if(FAILED(hr))return failed(hr,"LockBox");
    const UINT bytes=texelBytes(format);
    if(!lock.pBits || lock.RowPitch<int(w*bytes) ||
       lock.SlicePitch<std::int64_t(lock.RowPitch)*w) {
        texture->UnlockBox(0);
        return failed(E_FAIL,"invalid volume lock pitches");
    }
    const bool exact=w==UINT(volume.width)&&d==UINT(volume.depth);
    for(UINT z=0;z<d;++z)for(UINT y=0;y<w;++y) {
        auto* dst=static_cast<char*>(lock.pBits)+std::size_t(z)*lock.SlicePitch+std::size_t(y)*lock.RowPitch;
        for(UINT x=0;x<w;++x) {
            const auto p=exact
                ?volume.pixels[layer][(std::size_t(z)*w+y)*w+x]
                :volume.sample(layer,(x+.5f)/w,(y+.5f)/w,(z+.5f)/d);
            if(!putTexel(dst+x*bytes,p,format)){
                texture->UnlockBox(0);return failed(E_FAIL,"non-greyscale A8L8 volume");
            }
        }
    }
    const auto unlock=texture->UnlockBox(0);
    return failed(unlock,"UnlockBox");
}
inline Result upload(IDirect3DDevice9* device,const banks::Volume& volume,int layer,
                     UINT w,UINT d,IDirect3DVolumeTexture9** out,bool managed=false,D3DFORMAT format=D3DFMT_A8R8G8B8) {
    *out=nullptr;
    IDirect3DVolumeTexture9* staging=nullptr;
    const auto pool=managed?D3DPOOL_MANAGED:D3DPOOL_SYSTEMMEM;
    HRESULT hr=device->CreateVolumeTexture(w,w,d,1,0,format,pool,&staging,nullptr);
    if(FAILED(hr))return failed(hr,managed?"CreateVolumeTexture(MANAGED)":"CreateVolumeTexture(SYSTEMMEM)");
    auto result=fill(staging,volume,layer,w,d,format);
    if(!result){staging->Release();return result;}
    if(managed){*out=staging;return {};}
    hr=device->CreateVolumeTexture(w,w,d,1,0,format,D3DPOOL_DEFAULT,out,nullptr);
    if(FAILED(hr)){staging->Release();return failed(hr,"CreateVolumeTexture(DEFAULT)");}
    hr=device->UpdateTexture(staging,*out);
    staging->Release();
    if(FAILED(hr)){(*out)->Release();*out=nullptr;return failed(hr,"UpdateTexture");}
    return {};
}
// Build the distant 2D layer with ordinary RGBA mipmaps, without relying on
// private RenderWare raster structures. Alpha-weighted colour avoids halos.
inline Result uploadCanopy(IDirect3DDevice9* device,const std::vector<canopy::Pixel>& pixels,
                          UINT size,IDirect3DTexture9** out,bool managed=false,bool mipmaps=true,D3DFORMAT format=D3DFMT_A8R8G8B8) {
    *out=nullptr;
    // No scratch copy for atlas/no-mip uploads. Preserve the exact source
    // channels, including the alpha-weighted mip reduction for canopy.
    std::vector<canopy::Pixel> mip;
    if(mipmaps)mip=pixels;
    const UINT bytes=texelBytes(format);
    IDirect3DTexture9* stage=nullptr;
    HRESULT hr=device->CreateTexture(size,size,mipmaps?0:1,0,format,
        managed?D3DPOOL_MANAGED:D3DPOOL_SYSTEMMEM,&stage,nullptr);
    if(FAILED(hr))return failed(hr,"CreateTexture(canopy)");
    UINT n=size;
    for(UINT level=0;level<stage->GetLevelCount();++level) {
        D3DLOCKED_RECT lock{};
        if(FAILED(hr=stage->LockRect(level,&lock,nullptr,0))){stage->Release();return failed(hr,"LockRect(canopy)");}
        if(!lock.pBits||lock.Pitch<int(n*bytes)) {
            stage->UnlockRect(level);stage->Release();return failed(E_FAIL,"canopy pitch");
        }
        for(UINT y=0;y<n;++y)for(UINT x=0;x<n;++x) {
            const auto p=(mipmaps?mip:pixels)[std::size_t(y)*n+x];
            if(!putTexel(static_cast<char*>(lock.pBits)+std::size_t(y)*lock.Pitch+x*bytes,p,format)){
                stage->UnlockRect(level);stage->Release();return failed(E_FAIL,"non-greyscale A8L8 texture");
            }
        }
        if(FAILED(hr=stage->UnlockRect(level))){stage->Release();return failed(hr,"UnlockRect(canopy)");}
        if(n==1||level+1==stage->GetLevelCount())break;
        // In-place 2x2 downsample: each source quartet is read before
        // overwriting an earlier destination. All channels are greyscale.
        const UINT next=n/2;
        for(UINT y=0;y<next;++y)for(UINT x=0;x<next;++x) {
            unsigned alpha=0,r=0,g=0,b=0;
            for(UINT dy=0;dy<2;++dy)for(UINT dx=0;dx<2;++dx) {
                const auto p=mip[std::size_t(y*2+dy)*n+x*2+dx];
                alpha+=p.a;r+=p.r*p.a;g+=p.g*p.a;b+=p.b*p.a;
            }
            mip[std::size_t(y)*next+x]={byte(alpha?float(r)/alpha:255.f),byte(alpha?float(g)/alpha:255.f),
                byte(alpha?float(b)/alpha:255.f),std::uint8_t((alpha+2)/4)};
        }
        n=next;
    }
    if(managed){*out=stage;return {};}
    hr=device->CreateTexture(size,size,mipmaps?0:1,0,format,D3DPOOL_DEFAULT,out,nullptr);
    if(FAILED(hr)){stage->Release();return failed(hr,"CreateTexture(DEFAULT canopy)");}
    hr=device->UpdateTexture(stage,*out);stage->Release();
    if(FAILED(hr)){(*out)->Release();*out=nullptr;return failed(hr,"UpdateTexture(canopy)");}
    return {};
}
// Only select a compact format when advertised for the actual resource and
// filter type. If either endpoint fails upload, retry the entire pair at
// identical resolution in RGBA; no half-installed mixed weather textures.
template<class Texture,class Upload>
inline Result uploadPair(std::array<Texture*,2>& pair,bool preferCompact,D3DFORMAT& selected,Upload&& uploader) {
    Result last=failed(E_FAIL,"texture pair");
    for(auto format:{D3DFMT_A8L8,D3DFMT_A8R8G8B8}) {
        if(format==D3DFMT_A8L8&&!preferCompact)continue;
        bool success=true;
        for(int i=0;i<2;++i) {
            last=uploader(i,&pair[i],false,format);
            if(!last)last=uploader(i,&pair[i],true,format);
            if(!last){success=false;break;}
        }
        if(success){selected=format;return {};}
        for(auto& t:pair){if(t)t->Release();t=nullptr;}
    }
    return last;
}
struct ScreenVertex {float x,y,z,rhw;DWORD rgba,fog;float u,v,w;};
static_assert(sizeof(ScreenVertex)==36);
struct ClipVertex {
    float x,y,z,w; // homogeneous clip coords using the unchanged game projection
    float u,v,t,alpha,fog;
};
inline ClipVertex lerp(ClipVertex a,ClipVertex b,float t) {
    return {mix(a.x,b.x,t),mix(a.y,b.y,t),mix(a.z,b.z,t),mix(a.w,b.w,t),
        mix(a.u,b.u,t),mix(a.v,b.v,t),mix(a.t,b.t,t),mix(a.alpha,b.alpha,t),mix(a.fog,b.fog,t)};
}
inline D3DMATRIX multiply(const D3DMATRIX& a,const D3DMATRIX& b) {
    D3DMATRIX c{};
    for(int y=0;y<4;++y)for(int x=0;x<4;++x)for(int k=0;k<4;++k)c.m[y][x]+=a.m[y][k]*b.m[k][x];
    return c;
}
inline ClipVertex clipVertex(Vec pos,float u,float v,float w,float alpha,float fog,const D3DMATRIX& m) {
    return {pos.x*m._11+pos.y*m._21+pos.z*m._31+m._41,
        pos.x*m._12+pos.y*m._22+pos.z*m._32+m._42,
        pos.x*m._13+pos.y*m._23+pos.z*m._33+m._43,
        pos.x*m._14+pos.y*m._24+pos.z*m._34+m._44,u,v,w,alpha,fog};
}
inline ScreenVertex projectPacked(const ClipVertex& v,const D3DVIEWPORT9& vp,DWORD rgb,float visibility) {
    const float inv=1.f/v.w;
    // Retain actual XY projection and reciprocal W, even outside the scene
    // far plane. Clamp ONLY Z to the cleared far depth. Distant clouds can
    // occupy sky pixels but not overwrite any nearer scene geometry.
    return {float(vp.X)+(v.x*inv+1.f)*.5f*vp.Width,
        float(vp.Y)+(1.f-v.y*inv)*.5f*vp.Height,
        mix(vp.MinZ,vp.MaxZ,sat(v.z*inv)),inv,
        (DWORD(byte(v.alpha*visibility*255.f))<<24)|rgb,
        D3DCOLOR_ARGB(byte(v.fog*255.f),0,0,0),v.u,v.v,v.t};
}
inline ScreenVertex project(const ClipVertex& v,const D3DVIEWPORT9& vp,RGB colour,float visibility) {
    return projectPacked(v,vp,D3DCOLOR_ARGB(0,byte(colour.r),byte(colour.g),byte(colour.b)),visibility);
}
struct ProjectedMesh {
    std::vector<ClipVertex> clip;
    std::vector<unsigned> masks;
    std::vector<ScreenVertex> vertices;
    std::vector<std::uint16_t> indices;
};
// For the canopy, clip the near plane AND viewport boundaries explicitly;
// unlike hardware XYZ, XYZRHW cannot clip behind-eye points correctly.
// For volume rays, the near-plane condition is normally already satisfied.
// Keep ordinary indexed geometry; generate new triangles only for crossings.
inline float planeDistance(const ClipVertex& v,int plane) {
    switch(plane){
        case 0:return v.z;
        case 1:return v.w-.001f;
        case 2:return v.x+v.w;
        case 3:return v.w-v.x;
        case 4:return v.y+v.w;
        default:return v.w-v.y;
    }
}
template<bool Fast=true>
inline unsigned clipMask(const ClipVertex& v) {
    if constexpr(Fast) {
        const float edge=-.0001f*std::abs(v.w);
        return unsigned(v.z<0.f)|(unsigned(v.w-.001f<0.f)<<1)|
            (unsigned(v.x+v.w<edge)<<2)|(unsigned(v.w-v.x<edge)<<3)|
            (unsigned(v.y+v.w<edge)<<4)|(unsigned(v.w-v.y<edge)<<5);
    }

    unsigned mask=0;for(int i=0;i<6;++i)if(planeDistance(v,i)<(i<2?0.f:-.0001f*std::abs(v.w)))mask|=1u<<i;
    return mask;
}
template<bool Fast=true>
inline void projectMesh(ProjectedMesh& mesh,const D3DVIEWPORT9& vp,RGB color,float visibility,
                        const std::uint16_t* indices,std::size_t count) {
    mesh.vertices.resize(mesh.clip.size());mesh.indices.clear();mesh.indices.reserve(count);
    // Unreferenced invalid vertices are never drawn.
    constexpr unsigned unvisited=~0u;
    auto& masks=mesh.masks;masks.assign(mesh.clip.size(),unvisited);
    const DWORD rgb=D3DCOLOR_ARGB(0,byte(color.r),byte(color.g),byte(color.b));
    const auto projectVertex=[&](const ClipVertex& v) {
        if constexpr(Fast)return projectPacked(v,vp,rgb,visibility);
        else return project(v,vp,color,visibility);
    };
    const auto visit=[&](std::uint16_t i) {
        if(masks[i]!=unvisited)return;
        masks[i]=clipMask<Fast>(mesh.clip[i]);
        if(!masks[i])mesh.vertices[i]=projectVertex(mesh.clip[i]);
    };
    for(std::size_t i=0;i+2<count;i+=3) {
        const auto a=indices[i],b=indices[i+1],c=indices[i+2];
        visit(a);visit(b);visit(c);
        if(masks[a]&masks[b]&masks[c])continue;
        const unsigned planes=masks[a]|masks[b]|masks[c];
        const bool farA=mesh.clip[a].z>mesh.clip[a].w,farB=mesh.clip[b].z>mesh.clip[b].w,farC=mesh.clip[c].z>mesh.clip[c].w;
        const bool crossFar=farA!=farB||farA!=farC;
        if(!planes&&!crossFar){mesh.indices.insert(mesh.indices.end(),{a,b,c});continue;}
        // Ping-pong the active polygon; do not zero or copy 24 vertices
        // for every crossing triangle. Only elements [0,n) are read.
        ClipVertex storage[2][12];
        if constexpr(!Fast)std::memset(storage,0,sizeof(storage));
        auto* poly=storage[0];auto* next=storage[1];
        poly[0]=mesh.clip[a];poly[1]=mesh.clip[b];poly[2]=mesh.clip[c];int n=3;
        for(int p=0;p<6&&n>=3;++p)if(planes&(1u<<p)) {
            int out=0;
            for(int j=0;j<n;++j) {
                const auto& start=poly[j];const auto& end=poly[(j+1)%n];
                const float d=planeDistance(start,p),e=planeDistance(end,p);
                if(d>=0)next[out++]=start;
                if((d<0)!=(e<0))next[out++]=lerp(start,end,d/(d-e));
            }
            if constexpr(Fast)std::swap(poly,next);
            else std::copy_n(next,12,poly);
            n=out;
        }
        const auto emit=[&](const auto& polygon,int nVertices) {
            if(nVertices<3||mesh.vertices.size()+std::size_t(nVertices)>=65536)return;
            const auto base=std::uint16_t(mesh.vertices.size());
            for(int j=0;j<nVertices;++j)mesh.vertices.push_back(projectVertex(polygon[j]));
            for(int j=1;j+1<nVertices;++j)mesh.indices.insert(mesh.indices.end(),{base,std::uint16_t(base+j),std::uint16_t(base+j+1)});
        };
        if(!crossFar){emit(poly,n);continue;}
        // Separately triangulate each side of the scene far plane. Otherwise
        // interpolating a clamped depth with a near one can let a far cloud
        // incorrectly cover a distant foreground object.
        for(float sign:{1.f,-1.f}) {
            int out=0;
            for(int j=0;j<n;++j) {
                const auto& start=poly[j];const auto& end=poly[(j+1)%n];
                const float d=(start.w-start.z)*sign,e=(end.w-end.z)*sign;
                if(d>=0)next[out++]=start;
                if((d<0)!=(e<0))next[out++]=lerp(start,end,d/(d-e));
            }
            emit(next,out);
        }
    }
}
struct Options {
    D3DTEXTUREFILTERTYPE filter{D3DTEXF_LINEAR};
    D3DTEXTUREOP blendOp{D3DTOP_LERP};
    bool dual{true},mip{true},vertexFog{true};
};
inline Options options(const D3DCAPS9& caps) {
    Options o;
    const DWORD linear=D3DPTFILTERCAPS_MINFLINEAR|D3DPTFILTERCAPS_MAGFLINEAR;
    o.filter=(caps.VolumeTextureFilterCaps&linear)==linear?D3DTEXF_LINEAR:D3DTEXF_POINT;
    o.vertexFog=(caps.RasterCaps&D3DPRASTERCAPS_FOGVERTEX)!=0;
    o.blendOp=(caps.TextureOpCaps&D3DTEXOPCAPS_LERP)?D3DTOP_LERP:D3DTOP_BLENDFACTORALPHA;
    o.dual=caps.MaxSimultaneousTextures>=2&&caps.MaxTextureBlendStages>=3&&
        (caps.TextureOpCaps&(D3DTEXOPCAPS_LERP|D3DTEXOPCAPS_BLENDFACTORALPHA));
    return o;
}
struct StateScope {
    IDirect3DStateBlock9* block=nullptr;
    bool active=false;
    Result capture(IDirect3DDevice9* d) {
        // The caller owns this snapshot for exactly one device lifetime.
        // Capture refreshes every state: never assume the game left a state
        // unchanged across frames. Re-create if a driver rejects recapture.
        if(active)return failed(E_FAIL,"nested cloud state capture");
        HRESULT hr=block?block->Capture():E_FAIL;
        if(FAILED(hr)) {
            release();
            hr=d->CreateStateBlock(D3DSBT_ALL,&block);
        }
        active=SUCCEEDED(hr);
        return failed(hr,"Capture/CreateStateBlock(ALL)");
    }
    Result restore() {
        if(!active)return {};
        active=false;
        const auto hr=block->Apply();
        if(FAILED(hr))release();
        return failed(hr,"StateBlock::Apply");
    }
    void release() {
        if(active)restore();
        if(block){block->Release();block=nullptr;}
    }
    ~StateScope(){release();}
    StateScope()=default;
    StateScope(const StateScope&)=delete;
    StateScope& operator=(const StateScope&)=delete;
};
// Only used between capture/restore of one cloud group. All first uses
// are sent to D3D (the game/mods may have changed anything). DrawUP/VS
// changes don't touch these states. No cross-frame guesses or device gets.
struct DrawState {
    template<std::size_t N> struct Values {
        std::array<DWORD,N> value;
        std::bitset<N> known;
        template<class Setter> HRESULT set(std::size_t index,DWORD v,Setter&& setter,unsigned& calls,unsigned& skipped) {
            if(index<N&&known[index]&&value[index]==v){++skipped;return S_OK;}
            ++calls;const HRESULT hr=setter();
            if(SUCCEEDED(hr)&&index<N){known[index]=true;value[index]=v;}
            return hr;
        }
    };
    Values<256> render;
    Values<4*33> stage;
    Values<2*14> sampler;
    unsigned calls=0,skipped=0;
    void invalidate() {render.known.reset();stage.known.reset();sampler.known.reset();}
    HRESULT rs(IDirect3DDevice9* d,D3DRENDERSTATETYPE st,DWORD v) {
        return render.set(unsigned(st),v,[&]{return d->SetRenderState(st,v);},calls,skipped);
    }
    HRESULT ts(IDirect3DDevice9* d,DWORD i,D3DTEXTURESTAGESTATETYPE st,DWORD v) {
        return stage.set(i*33+unsigned(st),v,[&]{return d->SetTextureStageState(i,st,v);},calls,skipped);
    }
    HRESULT ss(IDirect3DDevice9* d,DWORD i,D3DSAMPLERSTATETYPE st,DWORD v) {
        return sampler.set(i*14+unsigned(st),v,[&]{return d->SetSamplerState(i,st,v);},calls,skipped);
    }
};
// Optional distant-only half-resolution accumulation. The original scene
// depth buffer is never copied, sampled, cleared or downsampled. It is
// rebound for the final premultiplied quad at the cleared far depth.
// X8 destinations only: avoid altering alpha-bearing/deferred attachments.
struct FarLayer {
    IDirect3DTexture9* texture{};
    IDirect3DSurface9* surface{};
    UINT width{},height{};
    bool disabled{};
    IDirect3DDevice9* capsDevice{};
    D3DCAPS9 caps{};
    StateScope snapshot;
    std::vector<ScreenVertex> scaled; // reusable projected proxy scratch
    void release() {
        snapshot.release();
        if(surface)surface->Release();
        if(texture)texture->Release();
        surface=nullptr;texture=nullptr;width=height=0;disabled=false;capsDevice=nullptr;
    }
    ~FarLayer(){release();}
    bool ensure(IDirect3DDevice9* d,UINT w,UINT h) {
        if(disabled)return false;
        if(texture&&width==w&&height==h)return true;
        release();
        if(FAILED(d->CreateTexture(w,h,1,D3DUSAGE_RENDERTARGET,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&texture,nullptr))||
           FAILED(texture->GetSurfaceLevel(0,&surface))) {
            release();disabled=true;return false;
        }
        width=w;height=h;return true;
    }
    template<class Draw>
    Result render(IDirect3DDevice9* d,const D3DVIEWPORT9& vp,Draw&& draw,bool& used,const RECT* bounds=nullptr,DrawState* states=nullptr,bool* mutated=nullptr) {
        used=false;if(mutated)*mutated=false;
        if(disabled||vp.Width<320||vp.Height<240)return {};
        if(capsDevice!=d) {
            if(FAILED(d->GetDeviceCaps(&caps)))return {};
            capsDevice=d;
        }
        if(!(caps.PrimitiveMiscCaps&D3DPMISCCAPS_SEPARATEALPHABLEND)||
           (caps.TextureFilterCaps&(D3DPTFILTERCAPS_MINFLINEAR|D3DPTFILTERCAPS_MAGFLINEAR))!=
            (D3DPTFILTERCAPS_MINFLINEAR|D3DPTFILTERCAPS_MAGFLINEAR))return {};
        struct SurfaceRef {IDirect3DSurface9* p{};~SurfaceRef(){if(p)p->Release();}};
        SurfaceRef rt,depth;
        if(FAILED(d->GetRenderTarget(0,&rt.p))||FAILED(d->GetDepthStencilSurface(&depth.p)))return {};
        D3DSURFACE_DESC desc{};
        if(FAILED(rt.p->GetDesc(&desc))||desc.Format!=D3DFMT_X8R8G8B8||desc.MultiSampleType!=D3DMULTISAMPLE_NONE)return {};
        for(DWORD i=1;i<caps.NumSimultaneousRTs;++i) {
            SurfaceRef extra;
            const auto result=d->GetRenderTarget(i,&extra.p);
            if((SUCCEEDED(result)&&extra.p)||(FAILED(result)&&result!=D3DERR_NOTFOUND))return {};
        }
        if(!ensure(d,(vp.Width+1)/2,(vp.Height+1)/2))return {};
        capsDevice=d;
        if(!states&&!snapshot.capture(d))return {};
        HRESULT hr=S_OK;
        const auto check=[&](HRESULT value){if(SUCCEEDED(hr)&&FAILED(value))hr=value;};
        const auto rs=[&](D3DRENDERSTATETYPE st,DWORD val){if(SUCCEEDED(hr))check(states?states->rs(d,st,val):d->SetRenderState(st,val));};
        const auto ts=[&](DWORD i,D3DTEXTURESTAGESTATETYPE st,DWORD val){if(SUCCEEDED(hr))check(states?states->ts(d,i,st,val):d->SetTextureStageState(i,st,val));};
        const auto ss=[&](D3DSAMPLERSTATETYPE st,DWORD val){if(SUCCEEDED(hr))check(states?states->ss(d,0,st,val):d->SetSamplerState(0,st,val));};
        if(mutated)*mutated=true;
        check(d->SetDepthStencilSurface(nullptr));
        if(SUCCEEDED(hr))check(d->SetRenderTarget(0,surface));
        const D3DVIEWPORT9 reducedViewport{0,0,width,height,0,1};
        if(SUCCEEDED(hr))check(d->SetViewport(&reducedViewport));
        if(SUCCEEDED(hr))check(d->Clear(0,nullptr,D3DCLEAR_TARGET,0,1,0));
        rs(D3DRS_ZENABLE,D3DZB_FALSE);
        rs(D3DRS_SEPARATEALPHABLENDENABLE,TRUE);
        rs(D3DRS_SRCBLENDALPHA,D3DBLEND_ONE);
        rs(D3DRS_DESTBLENDALPHA,D3DBLEND_INVSRCALPHA);
        rs(D3DRS_BLENDOPALPHA,D3DBLENDOP_ADD);
        if(SUCCEEDED(hr))check(draw());
        // Restore surfaces explicitly: D3DSBT_ALL does not capture them.
        // Even an unsuccessful accumulation must leave the scene intact.
        const auto restoreRT=d->SetRenderTarget(0,rt.p);
        const auto restoreDepth=d->SetDepthStencilSurface(depth.p);
        const auto restoreViewport=d->SetViewport(&vp);
        if(FAILED(restoreRT)||FAILED(restoreDepth)||FAILED(restoreViewport)) {
            snapshot.restore();
            return failed(FAILED(restoreRT)?restoreRT:FAILED(restoreDepth)?restoreDepth:restoreViewport,"far layer surface restoration");
        }
        if(FAILED(hr)) {
            const auto restored=snapshot.restore();disabled=true;
            const auto vpResult=d->SetViewport(&vp);
            return !restored?restored:failed(vpResult,"far layer viewport restore");
        }
        // Premultiplied alpha over the original colour target. The far
        // depth test preserves solid world geometry and the original viewport.
        check(d->SetVertexShader(nullptr));check(d->SetPixelShader(nullptr));
        check(d->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1));
        check(d->SetTexture(0,texture));
        rs(D3DRS_ZENABLE,D3DZB_TRUE);rs(D3DRS_ZWRITEENABLE,FALSE);
        rs(D3DRS_ZFUNC,D3DCMP_LESSEQUAL);rs(D3DRS_FOGENABLE,FALSE);
        rs(D3DRS_ALPHATESTENABLE,FALSE);rs(D3DRS_SRCBLEND,D3DBLEND_ONE);
        rs(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
        rs(D3DRS_SEPARATEALPHABLENDENABLE,FALSE);
        rs(D3DRS_COLORWRITEENABLE,D3DCOLORWRITEENABLE_RED|D3DCOLORWRITEENABLE_GREEN|D3DCOLORWRITEENABLE_BLUE);
        ts(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1);ts(0,D3DTSS_COLORARG1,D3DTA_TEXTURE);
        ts(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1);ts(0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE);
        ts(1,D3DTSS_COLOROP,D3DTOP_DISABLE);ts(1,D3DTSS_ALPHAOP,D3DTOP_DISABLE);
        ss(D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);ss(D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);
        ss(D3DSAMP_MINFILTER,D3DTEXF_LINEAR);ss(D3DSAMP_MAGFILTER,D3DTEXF_LINEAR);
        ss(D3DSAMP_MIPFILTER,D3DTEXF_NONE);
        struct QuadVertex {float x,y,z,rhw,u,v;};
        const float left=bounds?float(bounds->left):float(vp.X),top=bounds?float(bounds->top):float(vp.Y);
        const float right=bounds?float(bounds->right):float(vp.X+vp.Width),bottom=bounds?float(bounds->bottom):float(vp.Y+vp.Height);
        const float x=left-.5f,y=top-.5f,r=right-.5f,b=bottom-.5f,z=vp.MaxZ;
        const float u0=(left-vp.X)/vp.Width,v0=(top-vp.Y)/vp.Height,u1=(right-vp.X)/vp.Width,v1=(bottom-vp.Y)/vp.Height;
        const QuadVertex quad[]={{x,y,z,1,u0,v0},{r,y,z,1,u1,v0},{x,b,z,1,u0,v1},{r,b,z,1,u1,v1}};
        if(SUCCEEDED(hr)) {
            // Never repeat a potentially partial write into the game target.
            used=true;
            check(d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,quad,sizeof(QuadVertex)));
        }
        const auto restored=snapshot.restore();
        if(!restored)return restored;
        const auto vpResult=d->SetViewport(&vp);
        if(FAILED(vpResult))return failed(vpResult,"far layer viewport restore");
        if(FAILED(hr)&&!used){disabled=true;return {};}
        return failed(hr,"far layer composite");
    }
};
// Dynamic streaming avoids a driver-side UP copy; DISCARD lets the driver
// rename busy buffers rather than stall the CPU waiting for the previous frame.
struct GpuStream {
    IDirect3DVertexShader9* shader{};
    IDirect3DVertexDeclaration9* declaration{};
    IDirect3DVertexBuffer9* vb{};
    IDirect3DIndexBuffer9* ib{};
    std::vector<banks::GpuVertex> resident;
    bool residentValid=false;
    bool available() const {return shader&&declaration&&vb&&ib;}
    void release() {
        if(vb)vb->Release();if(ib)ib->Release();
        if(shader)shader->Release();if(declaration)declaration->Release();
        vb=nullptr;ib=nullptr;shader=nullptr;declaration=nullptr;
        residentValid=false;resident.clear();
    }
    ~GpuStream(){release();}
    bool create(IDirect3DDevice9* d,const D3DCAPS9& caps,DWORD behavior,const DWORD* comparisonShader=nullptr) {
        release();
#ifdef VC_GPU_VERTEX
        if(caps.VertexShaderVersion<D3DVS_VERSION(2,0)||!(behavior&D3DCREATE_HARDWARE_VERTEXPROCESSING))return false;
        const D3DVERTEXELEMENT9 elements[]={
            {0,0,D3DDECLTYPE_FLOAT3,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},
            {0,12,D3DDECLTYPE_D3DCOLOR,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,0},
            D3DDECL_END()
        };
        // The FXC header is a BYTE array: copy to aligned DWORD storage.
        static_assert(sizeof(cloudBankVS)%sizeof(DWORD)==0);
        std::array<DWORD,sizeof(cloudBankVS)/sizeof(DWORD)> code{};
        std::memcpy(code.data(),cloudBankVS,sizeof(cloudBankVS));
        if(SUCCEEDED(d->CreateVertexShader(comparisonShader?comparisonShader:code.data(),&shader))&&
           SUCCEEDED(d->CreateVertexDeclaration(elements,&declaration))&&
           SUCCEEDED(d->CreateVertexBuffer(UINT(banks::MaxVertices*sizeof(banks::GpuVertex)),D3DUSAGE_DYNAMIC|D3DUSAGE_WRITEONLY,0,D3DPOOL_DEFAULT,&vb,nullptr))&&
           SUCCEEDED(d->CreateIndexBuffer(UINT(banks::MaxVertices/4*6*sizeof(std::uint16_t)),D3DUSAGE_WRITEONLY,D3DFMT_INDEX16,D3DPOOL_DEFAULT,&ib,nullptr))) {
            // Every patch has the same six indices. The tail of this static
            // reversed index buffer matches any active vertex count exactly.
            void* data=nullptr;
            if(SUCCEEDED(ib->Lock(0,0,&data,0))) {
                auto* dst=static_cast<std::uint16_t*>(data);
                for(std::size_t p=banks::MaxVertices/4;p>0;--p) {
                    const auto i=std::uint16_t((p-1)*4);
                    const std::uint16_t pattern[]={std::uint16_t(i+2),std::uint16_t(i+3),std::uint16_t(i+1),
                        std::uint16_t(i+2),std::uint16_t(i+1),i};
                    std::memcpy(dst,pattern,sizeof(pattern));dst+=6;
                }
                if(SUCCEEDED(ib->Unlock()))return true;
            }
        }
        release();
#else
        (void)d;(void)caps;(void)behavior;(void)comparisonShader;
#endif
        return false;
    }
    Result upload(const banks::GpuMesh& mesh) {
        residentValid=false; // a DISCARD invalidates any previous shadow
        void* data=nullptr;
        const UINT vbytes=UINT(mesh.vertices.size()*sizeof(banks::GpuVertex));
        HRESULT hr=vb->Lock(0,vbytes,&data,D3DLOCK_DISCARD);
        if(FAILED(hr))return failed(hr,"cloud VB lock");
        std::memcpy(data,mesh.vertices.data(),vbytes);
        return failed(vb->Unlock(),"cloud VB unlock");
    }
    // The input vertices are a world-fixed position + lattice code. They
    // can remain identical for many frames even while weather, lighting,
    // camera and wind uniforms change. Keep exact bytes (not a hash).
    // After a successful upload swap CPU arrays; next build reuses the
    // old allocation without copying into a second shadow buffer.
    Result uploadReusable(banks::GpuMesh& mesh,bool* reused=nullptr) {
        if(reused)*reused=false;
        if(residentValid&&mesh.vertices.size()==resident.size()&&
           (resident.empty()||std::memcmp(mesh.vertices.data(),resident.data(),resident.size()*sizeof(banks::GpuVertex))==0)) {
            if(reused)*reused=true;
            return {};
        }
        const auto result=upload(mesh);
        if(result){resident.swap(mesh.vertices);residentValid=true;mesh.geometryCached=false;}
        return result;
    }
};
struct GpuPasses {
    GpuStream* stream{};
    FarLayer* farLayer{};
    bool usedFarLayer{},nestedScope{};
    std::array<std::array<float,4>,11> constants{};
    D3DMATRIX scene{},distant{};
    D3DVIEWPORT9 viewport{};
    std::array<banks::VertexRange,2> ranges{};
    UINT restrictedCount=0;
    // Hardware clips both sides of the old far plane with complementary
    // projections. The distant pass flattens only viewport depth; XY and W
    // remain identical, so clouds never move and near scene geometry wins.
    bool prepare(const Settings& s,const View& v,const Range& r,const canopy::Drift& drift,
                 bool farReady,bool vertexFog,float visibility,RGB color,const D3DMATRIX& transform,
                 const D3DMATRIX& projection,const D3DVIEWPORT9& vp,const banks::Lod* sharedLod=nullptr) {
        if(std::abs(projection._34-1.f)>.0001f||std::abs(projection._44)>.0001f||
           projection._33<=1.f||projection._43>=0.f||std::abs(projection._13)>.0001f||std::abs(projection._23)>.0001f)return false;
        const float farClip=projection._43/(1.f-projection._33);
        if(!std::isfinite(farClip)||farClip<=0.f)return false;
        scene=transform;distant=transform;viewport=vp;restrictedCount=0;
        const float extended=std::max(farClip+100.f,2.f*r.end+farClip);
        const float a=extended/(extended-farClip),b=-a*farClip;
        distant._13=a*scene._14;distant._23=a*scene._24;distant._33=a*scene._34;distant._43=a*scene._44+b;
        const auto tex=banks::textureOrigin(s,v,drift);
        const auto lod=sharedLod?*sharedLod:banks::lod(s,r,farReady,v.pos.z-(s.minSpawnZ+s.maxSpawnZ)*.5f);
        constants[4]={v.pos.x,v.pos.y,v.pos.z,visibility};
        constants[5]={1.f/tex.tile,tex.u,tex.v,1.f/tex.band};
        constants[6]={v.nearClip,1.f/std::max(80.f,s.nearFadeDistance),s.interiorHaze*.22f,s.minSpawnZ};
        constants[7]={r.end,1.f/(r.end-r.begin),lod.start,lod.end>lod.start?1.f/(lod.end-lod.start):0.f};
        constants[8]={s.cloudFogStart,s.cloudFogEnd-s.cloudFogStart>1.f?1.f/(s.cloudFogEnd-s.cloudFogStart):0.f,vertexFog?1.f:0.f,0.f};
        constants[9]={byte(color.r)/255.f,byte(color.g)/255.f,byte(color.b)/255.f,1.f};
        const auto step=banks::latticeSpacing(s);constants[10]={step.x,step.y,step.z,0.f};
        return true;
    }
    void restrictTo(const banks::GpuMesh& mesh) {
        banks::ClipPlane distantPlane{},scenePlane{};
        for(int i=0;i<4;++i) {
            const double z=scene.m[i][2],w=scene.m[i][3],dz=distant.m[i][2];
            distantPlane.coefficient[i]=dz;distantPlane.absolute[i]=std::abs(dz);
            scenePlane.coefficient[i]=w-z;scenePlane.absolute[i]=std::abs(w)+std::abs(z);
        }
        ranges={mesh.rangeFor(distantPlane),mesh.rangeFor(scenePlane)};
        restrictedCount=UINT(mesh.vertices.size());
    }
    template<class Configure>
    Result draw(IDirect3DDevice9* d,UINT count,UINT triangles,DrawState* states,Configure&& configure) {
        HRESULT hr=S_OK;
        const auto bindStream=[&]()->Result {
            HRESULT status;
            if(FAILED(status=d->SetStreamSource(0,stream->vb,0,sizeof(banks::GpuVertex))))return failed(status,"cloud stream");
            if(FAILED(status=d->SetStreamSourceFreq(0,1)))return failed(status,"cloud stream frequency");
            return failed(d->SetIndices(stream->ib),"cloud indices");
        };
        const auto bound=bindStream();if(!bound)return bound;
        bool constantsSet=false,viewportChanged=false;
        for(int pass=0;pass<2;++pass) {
            const auto range=restrictedCount==count?ranges[pass]:banks::VertexRange{0,count};
            if(!range.count())continue;
            const auto& m=pass?scene:distant;
            for(int i=0;i<4;++i)constants[i]={m.m[0][i],m.m[1][i],m.m[2][i],m.m[3][i]};
            // All non-matrix uniforms are identical between passes.
            if(FAILED(hr=d->SetVertexShaderConstantF(0,constants[0].data(),constantsSet?4u:UINT(constants.size()))))break;
            constantsSet=true;
            // The common scene-only path already has the correct viewport.
            // Restore it only if the distant pass actually changed it.
            if(!pass||viewportChanged) {
                auto vp=viewport;if(!pass)vp.MinZ=vp.MaxZ;
                if(FAILED(hr=d->SetViewport(&vp)))break;
                viewportChanged=!pass;
            }
            // Reversed IB: a subrange's last (farthest) vertex selects
            // its first index. No primitives are reordered or split.
            const auto draw=[&]{return d->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,range.begin,range.count(),
                UINT((banks::MaxVertices-range.end)/4*6),restrictedCount==count?range.count()/2:triangles);};
            if(!pass&&farLayer&&range.count()>=1024) {
                auto* group=nestedScope?nullptr:states;
                bool mutated=false;
                const auto result=farLayer->render(d,viewport,draw,usedFarLayer,nullptr,group,&mutated);
                if(!result){hr=result.hr;break;}
                // A far-only volume needs no restoration of cloud state:
                // the outer group snapshot will restore the game once.
                // If a near draw or native fallback is required, rebind
                // only changed render state through the cache, plus the
                // shader/stream/texture bindings overwritten by DrawUP.
                viewportChanged=false;
                if(group&&mutated) {
                    if(usedFarLayer&&restrictedCount==count&&!ranges[1].count())continue;
                    if(!usedFarLayer)group->invalidate();
                    const auto restored=configure();if(!restored)return restored;
                    const auto rebound=bindStream();if(!rebound)return rebound;
                    constantsSet=false;
                    if(!usedFarLayer) {
                        if(FAILED(hr=d->SetVertexShaderConstantF(0,constants[0].data(),UINT(constants.size()))))break;
                        constantsSet=true;
                    }
                }
                // render() restores the ordinary scene viewport, including
                // on recoverable failure. A native fallback must flatten it.
                if(usedFarLayer)continue;
                auto vp=viewport;vp.MinZ=vp.MaxZ;
                if(FAILED(hr=d->SetViewport(&vp)))break;
                viewportChanged=true;
            }
            if(FAILED(hr=draw()))break;
        }
        const auto restore=viewportChanged?d->SetViewport(&viewport):S_OK;
        return FAILED(restore)?failed(restore,"cloud viewport restore"):failed(hr,"hardware cloud draw");
    }
};
inline Result drawRaw(IDirect3DDevice9* d,const std::array<IDirect3DBaseTexture9*,2>& textures,
                   const Options& opts,const void* vertices,UINT count,UINT stride,DWORD fvf,
                   const std::uint16_t* indices,UINT triangles,float coverage,bool fog,
                   bool projected,bool mipmaps,DWORD fogColor,
                   const D3DMATRIX* view=nullptr,const D3DMATRIX* projection=nullptr,bool ownState=true,GpuPasses* gpu=nullptr,DrawState* states=nullptr,bool accumulate=false) {
    if(!count||!triangles)return {};
    StateScope scoped;
    if(ownState)states=nullptr; // a standalone scope may restore different states
    if(ownState){const auto capture=scoped.capture(d);if(!capture)return capture;}
    // CreateStateBlock already captures state, including on pure devices.
    // Do not use GetTransform, GetRenderState, etc., which pure devices prohibit.
    Result result;
    const auto check=[&](HRESULT hr,const char* op){if(!SUCCEEDED(hr)&&result)result=failed(hr,op);};
    const auto rs=[&](D3DRENDERSTATETYPE st,DWORD value){check(states?states->rs(d,st,value):d->SetRenderState(st,value),"SetRenderState");};
    const auto ts=[&](DWORD stage,D3DTEXTURESTAGESTATETYPE st,DWORD value){check(states?states->ts(d,stage,st,value):d->SetTextureStageState(stage,st,value),"SetTextureStageState");};
    const auto ss=[&](DWORD stage,D3DSAMPLERSTATETYPE st,DWORD value){check(states?states->ss(d,stage,st,value):d->SetSamplerState(stage,st,value),"SetSamplerState");};
    const auto configure=[&]()->Result {
        const auto world=identity();
        if(!projected)check(d->SetTransform(D3DTS_WORLD,&world),"SetTransform(WORLD)");
        if(view)check(d->SetTransform(D3DTS_VIEW,view),"SetTransform(VIEW)");
        if(projection)check(d->SetTransform(D3DTS_PROJECTION,projection),"SetTransform(PROJECTION)");
        check(d->SetVertexShader(gpu?gpu->stream->shader:nullptr),"SetVertexShader");
        check(d->SetPixelShader(nullptr),"SetPixelShader");
        if(gpu)check(d->SetVertexDeclaration(gpu->stream->declaration),"SetVertexDeclaration");
        else check(d->SetFVF(fvf),"SetFVF(clouds)");
        rs(D3DRS_LIGHTING,FALSE);rs(D3DRS_SPECULARENABLE,FALSE);
        rs(D3DRS_VERTEXBLEND,D3DVBF_DISABLE);rs(D3DRS_INDEXEDVERTEXBLENDENABLE,FALSE);
        rs(D3DRS_FOGENABLE,fog?TRUE:FALSE);
        if(projected) {
            // For transformed/lit vertices D3D9 consumes the explicitly supplied
            // fog factor (specular alpha), not the game's table fog or fog start.
            rs(D3DRS_FOGTABLEMODE,D3DFOG_NONE);rs(D3DRS_FOGVERTEXMODE,D3DFOG_NONE);
            rs(D3DRS_RANGEFOGENABLE,FALSE);rs(D3DRS_FOGCOLOR,fogColor);
        }
        rs(D3DRS_ZENABLE,accumulate?D3DZB_FALSE:D3DZB_TRUE);rs(D3DRS_ZWRITEENABLE,FALSE);
        rs(D3DRS_ZFUNC,D3DCMP_LESSEQUAL);rs(D3DRS_ALPHABLENDENABLE,TRUE);
        rs(D3DRS_ALPHATESTENABLE,TRUE);rs(D3DRS_ALPHAFUNC,D3DCMP_GREATER);rs(D3DRS_ALPHAREF,0);
        rs(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);rs(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
        rs(D3DRS_BLENDOP,D3DBLENDOP_ADD);rs(D3DRS_SEPARATEALPHABLENDENABLE,accumulate?TRUE:FALSE);
        if(accumulate) {
            rs(D3DRS_SRCBLENDALPHA,D3DBLEND_ONE);rs(D3DRS_DESTBLENDALPHA,D3DBLEND_INVSRCALPHA);
            rs(D3DRS_BLENDOPALPHA,D3DBLENDOP_ADD);
        }
        rs(D3DRS_CULLMODE,D3DCULL_NONE);rs(D3DRS_SHADEMODE,D3DSHADE_GOURAUD);
        rs(D3DRS_COLORWRITEENABLE,0xF);rs(D3DRS_SRGBWRITEENABLE,FALSE);rs(D3DRS_STENCILENABLE,FALSE);
        rs(D3DRS_SCISSORTESTENABLE,FALSE);rs(D3DRS_CLIPPING,TRUE);
        rs(D3DRS_CLIPPLANEENABLE,0);rs(D3DRS_WRAP0,0);rs(D3DRS_WRAP1,0);
        const auto factor=byte(coverage*255.f);
        rs(D3DRS_TEXTUREFACTOR,D3DCOLOR_ARGB(factor,factor,factor,factor));
        const bool blend=opts.dual&&factor>0&&factor<255;
        const DWORD first=blend?0:coverage>=.5f?1:0;
        check(d->SetTexture(0,textures[first]),"SetTexture(volume0)");
        for(DWORD i=0;i<(blend?2u:1u);++i) {
            ss(i,D3DSAMP_ADDRESSU,D3DTADDRESS_WRAP);ss(i,D3DSAMP_ADDRESSV,D3DTADDRESS_WRAP);
            ss(i,D3DSAMP_ADDRESSW,D3DTADDRESS_CLAMP);
            ss(i,D3DSAMP_MINFILTER,opts.filter);ss(i,D3DSAMP_MAGFILTER,opts.filter);
            ss(i,D3DSAMP_MIPFILTER,mipmaps?D3DTEXF_LINEAR:D3DTEXF_NONE);ss(i,D3DSAMP_SRGBTEXTURE,FALSE);
            ts(i,D3DTSS_TEXCOORDINDEX,0);ts(i,D3DTSS_TEXTURETRANSFORMFLAGS,D3DTTFF_DISABLE);
            ts(i,D3DTSS_RESULTARG,D3DTA_CURRENT);
        }
        ts(0,D3DTSS_COLOROP,blend?D3DTOP_SELECTARG1:D3DTOP_MODULATE);
        ts(0,D3DTSS_ALPHAOP,blend?D3DTOP_SELECTARG1:D3DTOP_MODULATE);
        ts(0,D3DTSS_COLORARG1,D3DTA_TEXTURE);ts(0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE);
        ts(0,D3DTSS_COLORARG2,D3DTA_DIFFUSE);ts(0,D3DTSS_ALPHAARG2,D3DTA_DIFFUSE);
        if(blend) {
            check(d->SetTexture(1,textures[1]),"SetTexture(volume1)");
            ts(1,D3DTSS_COLOROP,opts.blendOp);ts(1,D3DTSS_ALPHAOP,opts.blendOp);
            ts(1,D3DTSS_COLORARG0,D3DTA_TFACTOR);ts(1,D3DTSS_ALPHAARG0,D3DTA_TFACTOR);
            ts(1,D3DTSS_COLORARG1,D3DTA_TEXTURE);ts(1,D3DTSS_ALPHAARG1,D3DTA_TEXTURE);
            ts(1,D3DTSS_COLORARG2,D3DTA_CURRENT);ts(1,D3DTSS_ALPHAARG2,D3DTA_CURRENT);
            ts(2,D3DTSS_RESULTARG,D3DTA_CURRENT);
            ts(2,D3DTSS_TEXCOORDINDEX,0);ts(2,D3DTSS_TEXTURETRANSFORMFLAGS,D3DTTFF_DISABLE);
            ts(2,D3DTSS_COLOROP,D3DTOP_MODULATE);ts(2,D3DTSS_ALPHAOP,D3DTOP_MODULATE);
            ts(2,D3DTSS_COLORARG1,D3DTA_CURRENT);ts(2,D3DTSS_ALPHAARG1,D3DTA_CURRENT);
            ts(2,D3DTSS_COLORARG2,D3DTA_DIFFUSE);ts(2,D3DTSS_ALPHAARG2,D3DTA_DIFFUSE);
        }
        ts(blend?3:1,D3DTSS_COLOROP,D3DTOP_DISABLE);
        ts(blend?3:1,D3DTSS_ALPHAOP,D3DTOP_DISABLE);
        return result;
    };
    configure();
    if(result&&gpu)result=gpu->draw(d,count,triangles,states,configure);
    else if(result)check(d->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST,0,count,triangles,
        indices,D3DFMT_INDEX16,vertices,stride),"DrawIndexedPrimitiveUP");
    const auto restore=scoped.restore();
    // Restore failures are higher priority than draw failures.
    if(!restore)return restore;
    return result;
}
// Original XYZ entry point retained for the isolated sampler regression.
inline Result draw(IDirect3DDevice9* d,const std::array<IDirect3DVolumeTexture9*,2>& textures,
                   const Options& opts,const Vertex* vertices,UINT count,const std::uint16_t* indices,
                   UINT triangles,float coverage,bool fog,const D3DMATRIX* view=nullptr,const D3DMATRIX* projection=nullptr) {
    return drawRaw(d,{textures[0],textures[1]},opts,vertices,count,sizeof(Vertex),
        D3DFVF_XYZ|D3DFVF_DIFFUSE|D3DFVF_TEX1|D3DFVF_TEXCOORDSIZE3(0),indices,triangles,coverage,fog,false,false,0,view,projection);
}
inline Result drawProjected(IDirect3DDevice9* d,const std::array<IDirect3DBaseTexture9*,2>& textures,
                            const Options& opts,const ProjectedMesh& mesh,float coverage,bool volume,
                            RGB fogColor,bool fog=true,bool ownState=true,DrawState* states=nullptr) {
    return drawRaw(d,textures,opts,mesh.vertices.data(),UINT(mesh.vertices.size()),sizeof(ScreenVertex),
        D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_SPECULAR|D3DFVF_TEX1|D3DFVF_TEXCOORDSIZE3(0),
        mesh.indices.data(),UINT(mesh.indices.size()/3),coverage,fog&&opts.vertexFog,true,!volume&&opts.mip,
        D3DCOLOR_XRGB(byte(fogColor.r),byte(fogColor.g),byte(fogColor.b)),nullptr,nullptr,ownState,nullptr,states);
}
// Accelerate only a far-depth prefix of the already sorted directional
// proxy. The remaining geometry is drawn at native resolution, in exactly
// the original order. Never downsample/read the game depth buffer. A
// projected-area estimate avoids a costly RT switch for a few small cards.
inline Result drawFarProjected(IDirect3DDevice9* d,const std::array<IDirect3DBaseTexture9*,2>& textures,
        const Options& opts,const ProjectedMesh& mesh,float coverage,RGB fogColor,
        const D3DVIEWPORT9& vp,FarLayer* layer,DrawState* states=nullptr,bool* usedLayer=nullptr,bool nestedScope=false) {
    if(usedLayer)*usedLayer=false;
    if(!layer||mesh.indices.size()<96)
        return drawProjected(d,textures,opts,mesh,coverage,false,fogColor,true,false,states);
    std::size_t prefix=0;float area=0;
    float minX=float(vp.X+vp.Width),minY=float(vp.Y+vp.Height),maxX=float(vp.X),maxY=float(vp.Y);
    for(;prefix+2<mesh.indices.size();prefix+=3) {
        const auto& a=mesh.vertices[mesh.indices[prefix]];
        const auto& b=mesh.vertices[mesh.indices[prefix+1]];
        const auto& c=mesh.vertices[mesh.indices[prefix+2]];
        if(a.z!=vp.MaxZ||b.z!=vp.MaxZ||c.z!=vp.MaxZ)break;
        area+=.5f*std::abs((b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x));
        minX=std::min({minX,a.x,b.x,c.x});maxX=std::max({maxX,a.x,b.x,c.x});
        minY=std::min({minY,a.y,b.y,c.y});maxY=std::max({maxY,a.y,b.y,c.y});
    }
    // Two native pixels of guard include the bilinear footprint of a
    // half-size target. Constrain the compositing quad to its support.
    const RECT bounds{std::max(LONG(vp.X),LONG(std::floor(minX))-3),
        std::max(LONG(vp.Y),LONG(std::floor(minY))-3),
        std::min(LONG(vp.X+vp.Width),LONG(std::ceil(maxX))+3),
        std::min(LONG(vp.Y+vp.Height),LONG(std::ceil(maxY))+3)};
    const float compositeArea=float(bounds.right-bounds.left)*float(bounds.bottom-bounds.top);
    if(prefix<96||bounds.left>=bounds.right||bounds.top>=bounds.bottom||area<1.8f*compositeArea)
        return drawProjected(d,textures,opts,mesh,coverage,false,fogColor,true,false,states);
    constexpr DWORD fvf=D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_SPECULAR|D3DFVF_TEX1|D3DFVF_TEXCOORDSIZE3(0);
    const DWORD colour=D3DCOLOR_XRGB(byte(fogColor.r),byte(fogColor.g),byte(fogColor.b));
    // Reuse a scratch copy, leaving the original projected mesh untouched
    // for native fallback and the near tail. Allocation failure is harmless.
    try {layer->scaled=mesh.vertices;}
    catch(const std::bad_alloc&) {return drawProjected(d,textures,opts,mesh,coverage,false,fogColor,true,false,states);}
    bool used=false;
    auto* accumulationStates=nestedScope?nullptr:states;
    const auto result=layer->render(d,vp,[&]{
        const float sx=float(layer->width)/vp.Width,sy=float(layer->height)/vp.Height;
        // Unreferenced vertices can remain stale; only actual prefix
        // indices are consumed. Projected XY is affine in viewport size.
        for(auto& v:layer->scaled){v.x=(v.x-vp.X)*sx;v.y=(v.y-vp.Y)*sy;v.z=1.f;}
        return drawRaw(d,textures,opts,layer->scaled.data(),UINT(layer->scaled.size()),sizeof(ScreenVertex),fvf,
            mesh.indices.data(),UINT(prefix/3),coverage,opts.vertexFog,true,false,colour,
            nullptr,nullptr,false,nullptr,accumulationStates,true).hr;
    },used,&bounds,accumulationStates);
    // An outer group snapshot already captures the game state. Avoid a
    // nested full device Capture/Apply solely to perform the proxy pass.
    // Every render/stage/sampler setter is tracked through the same
    // group cache. Uncertain/failed accumulation invalidates it; scene
    // surfaces and viewport were restored independently even on failure.
    if(accumulationStates&&(!result||!used))accumulationStates->invalidate();
    if(!result)return result;
    if(!used)return drawProjected(d,textures,opts,mesh,coverage,false,fogColor,true,false,states);
    if(usedLayer)*usedLayer=true;
    if(prefix==mesh.indices.size())return {};
    return drawRaw(d,textures,opts,mesh.vertices.data(),UINT(mesh.vertices.size()),sizeof(ScreenVertex),fvf,
        mesh.indices.data()+prefix,UINT((mesh.indices.size()-prefix)/3),coverage,opts.vertexFog,true,false,colour,
        nullptr,nullptr,false,nullptr,states);
}
} // namespace vc::d3d9
