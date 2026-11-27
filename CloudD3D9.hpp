#pragma once
// Standalone D3D9 volume backend, also compiled by the Win32 device test.
// No RenderWare dependency, pixel shaders, or direct pipeline-state getters.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d9.h>
#include <cstring>
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
inline Result fill(IDirect3DVolumeTexture9* texture,const banks::Volume& volume,int layer,UINT w,UINT d) {
    D3DLOCKED_BOX lock{};
    const auto hr=texture->LockBox(0,&lock,nullptr,0);
    if(FAILED(hr))return failed(hr,"LockBox");
    if(!lock.pBits || lock.RowPitch<int(w*sizeof(DWORD)) ||
       lock.SlicePitch<std::int64_t(lock.RowPitch)*w) {
        texture->UnlockBox(0);
        return failed(E_FAIL,"invalid volume lock pitches");
    }
    for(UINT z=0;z<d;++z)for(UINT y=0;y<w;++y) {
        auto* dst=static_cast<char*>(lock.pBits)+std::size_t(z)*lock.SlicePitch+std::size_t(y)*lock.RowPitch;
        for(UINT x=0;x<w;++x) {
            const auto p=w==UINT(volume.width)&&d==UINT(volume.depth)
                ?volume.pixels[layer][volume.index(x,y,z)]
                :volume.sample(layer,(x+.5f)/w,(y+.5f)/w,(z+.5f)/d);
            const DWORD argb=D3DCOLOR_ARGB(p.a,p.r,p.g,p.b);
            // No alignment assumption on the driver's row offsets.
            std::memcpy(dst+x*sizeof(DWORD),&argb,sizeof(argb));
        }
    }
    const auto unlock=texture->UnlockBox(0);
    return failed(unlock,"UnlockBox");
}
inline Result upload(IDirect3DDevice9* device,const banks::Volume& volume,int layer,
                     UINT w,UINT d,IDirect3DVolumeTexture9** out,bool managed=false) {
    *out=nullptr;
    IDirect3DVolumeTexture9* staging=nullptr;
    const auto pool=managed?D3DPOOL_MANAGED:D3DPOOL_SYSTEMMEM;
    HRESULT hr=device->CreateVolumeTexture(w,w,d,1,0,D3DFMT_A8R8G8B8,pool,&staging,nullptr);
    if(FAILED(hr))return failed(hr,managed?"CreateVolumeTexture(MANAGED)":"CreateVolumeTexture(SYSTEMMEM)");
    auto result=fill(staging,volume,layer,w,d);
    if(!result){staging->Release();return result;}
    if(managed){*out=staging;return {};}
    hr=device->CreateVolumeTexture(w,w,d,1,0,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,out,nullptr);
    if(FAILED(hr)){staging->Release();return failed(hr,"CreateVolumeTexture(DEFAULT)");}
    hr=device->UpdateTexture(staging,*out);
    staging->Release();
    if(FAILED(hr)){(*out)->Release();*out=nullptr;return failed(hr,"UpdateTexture");}
    return {};
}
// Build the distant 2D layer with ordinary RGBA mipmaps, without relying on
// private RenderWare raster structures. Alpha-weighted colour avoids halos.
inline Result uploadCanopy(IDirect3DDevice9* device,const std::vector<canopy::Pixel>& pixels,
                          UINT size,IDirect3DTexture9** out,bool managed=false,bool mipmaps=true) {
    *out=nullptr;
    std::vector<canopy::Pixel> mip=pixels;
    IDirect3DTexture9* stage=nullptr;
    HRESULT hr=device->CreateTexture(size,size,mipmaps?0:1,0,D3DFMT_A8R8G8B8,
        managed?D3DPOOL_MANAGED:D3DPOOL_SYSTEMMEM,&stage,nullptr);
    if(FAILED(hr))return failed(hr,"CreateTexture(canopy)");
    UINT n=size;
    for(UINT level=0;level<stage->GetLevelCount();++level) {
        D3DLOCKED_RECT lock{};
        if(FAILED(hr=stage->LockRect(level,&lock,nullptr,0))){stage->Release();return failed(hr,"LockRect(canopy)");}
        if(!lock.pBits||lock.Pitch<int(n*sizeof(DWORD))) {
            stage->UnlockRect(level);stage->Release();return failed(E_FAIL,"canopy pitch");
        }
        for(UINT y=0;y<n;++y)for(UINT x=0;x<n;++x) {
            const auto p=mip[std::size_t(y)*n+x];
            const DWORD argb=D3DCOLOR_ARGB(p.a,p.r,p.g,p.b);
            std::memcpy(static_cast<char*>(lock.pBits)+std::size_t(y)*lock.Pitch+x*sizeof(DWORD),&argb,sizeof(argb));
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
    hr=device->CreateTexture(size,size,mipmaps?0:1,0,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,out,nullptr);
    if(FAILED(hr)){stage->Release();return failed(hr,"CreateTexture(DEFAULT canopy)");}
    hr=device->UpdateTexture(stage,*out);stage->Release();
    if(FAILED(hr)){(*out)->Release();*out=nullptr;return failed(hr,"UpdateTexture(canopy)");}
    return {};
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
inline ScreenVertex project(const ClipVertex& v,const D3DVIEWPORT9& vp,RGB colour,float visibility) {
    const float inv=1.f/v.w;
    // Retain actual XY projection and reciprocal W, even outside the scene
    // far plane. Clamp ONLY Z to the cleared far depth. Distant clouds can
    // occupy sky pixels but not overwrite any nearer scene geometry.
    return {float(vp.X)+(v.x*inv+1.f)*.5f*vp.Width,
        float(vp.Y)+(1.f-v.y*inv)*.5f*vp.Height,
        mix(vp.MinZ,vp.MaxZ,sat(v.z*inv)),inv,
        D3DCOLOR_ARGB(byte(v.alpha*visibility*255.f),byte(colour.r),byte(colour.g),byte(colour.b)),
        D3DCOLOR_ARGB(byte(v.fog*255.f),0,0,0),v.u,v.v,v.t};
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
inline unsigned clipMask(const ClipVertex& v) {
    unsigned mask=0;for(int i=0;i<6;++i)if(planeDistance(v,i)<(i<2?0.f:-.0001f*std::abs(v.w)))mask|=1u<<i;
    return mask;
}
inline void projectMesh(ProjectedMesh& mesh,const D3DVIEWPORT9& vp,RGB color,float visibility,
                        const std::uint16_t* indices,std::size_t count) {
    mesh.vertices.resize(mesh.clip.size());mesh.indices.clear();mesh.indices.reserve(count);
    // Unreferenced invalid vertices are never drawn.
    constexpr unsigned unvisited=~0u;
    auto& masks=mesh.masks;masks.assign(mesh.clip.size(),unvisited);
    const auto visit=[&](std::uint16_t i) {
        if(masks[i]!=unvisited)return;
        masks[i]=clipMask(mesh.clip[i]);
        if(!masks[i])mesh.vertices[i]=project(mesh.clip[i],vp,color,visibility);
    };
    for(std::size_t i=0;i+2<count;i+=3) {
        const auto a=indices[i],b=indices[i+1],c=indices[i+2];
        visit(a);visit(b);visit(c);
        if(masks[a]&masks[b]&masks[c])continue;
        const unsigned planes=masks[a]|masks[b]|masks[c];
        const bool farA=mesh.clip[a].z>mesh.clip[a].w,farB=mesh.clip[b].z>mesh.clip[b].w,farC=mesh.clip[c].z>mesh.clip[c].w;
        const bool crossFar=farA!=farB||farA!=farC;
        if(!planes&&!crossFar){mesh.indices.insert(mesh.indices.end(),{a,b,c});continue;}
        std::array<ClipVertex,12> poly{},next{};
        poly[0]=mesh.clip[a];poly[1]=mesh.clip[b];poly[2]=mesh.clip[c];int n=3;
        for(int p=0;p<6&&n>=3;++p)if(planes&(1u<<p)) {
            int out=0;
            for(int j=0;j<n;++j) {
                const auto& start=poly[j];const auto& end=poly[(j+1)%n];
                const float d=planeDistance(start,p),e=planeDistance(end,p);
                if(d>=0)next[out++]=start;
                if((d<0)!=(e<0))next[out++]=lerp(start,end,d/(d-e));
            }
            poly=next;n=out;
        }
        const auto emit=[&](const auto& polygon,int nVertices) {
            if(nVertices<3||mesh.vertices.size()+std::size_t(nVertices)>=65536)return;
            const auto base=std::uint16_t(mesh.vertices.size());
            for(int j=0;j<nVertices;++j)mesh.vertices.push_back(project(polygon[j],vp,color,visibility));
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
    Result capture(IDirect3DDevice9* d) {
        return failed(d->CreateStateBlock(D3DSBT_ALL,&block),"CreateStateBlock(ALL)");
    }
    Result restore() {
        if(!block)return {};
        const auto hr=block->Apply();block->Release();block=nullptr;
        return failed(hr,"StateBlock::Apply");
    }
    ~StateScope(){restore();}
};
// Dynamic streaming avoids a driver-side UP copy; DISCARD lets the driver
// rename busy buffers rather than stall the CPU waiting for the previous frame.
struct GpuStream {
    IDirect3DVertexShader9* shader{};
    IDirect3DVertexDeclaration9* declaration{};
    IDirect3DVertexBuffer9* vb{};
    IDirect3DIndexBuffer9* ib{};
    bool available() const {return shader&&declaration&&vb&&ib;}
    void release() {
        if(vb)vb->Release();if(ib)ib->Release();
        if(shader)shader->Release();if(declaration)declaration->Release();
        vb=nullptr;ib=nullptr;shader=nullptr;declaration=nullptr;
    }
    ~GpuStream(){release();}
    bool create(IDirect3DDevice9* d,const D3DCAPS9& caps,DWORD behavior) {
        release();
#ifdef VC_GPU_VERTEX
        if(caps.VertexShaderVersion<D3DVS_VERSION(2,0)||!(behavior&D3DCREATE_HARDWARE_VERTEXPROCESSING))return false;
        const D3DVERTEXELEMENT9 elements[]={
            {0,0,D3DDECLTYPE_FLOAT3,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},
            {0,12,D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,0},
            D3DDECL_END()
        };
        // The FXC header is a BYTE array: copy to aligned DWORD storage.
        static_assert(sizeof(cloudBankVS)%sizeof(DWORD)==0);
        std::array<DWORD,sizeof(cloudBankVS)/sizeof(DWORD)> code{};
        std::memcpy(code.data(),cloudBankVS,sizeof(cloudBankVS));
        if(SUCCEEDED(d->CreateVertexShader(code.data(),&shader))&&
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
        (void)d;(void)caps;(void)behavior;
#endif
        return false;
    }
    Result upload(const banks::GpuMesh& mesh) {
        void* data=nullptr;
        const UINT vbytes=UINT(mesh.vertices.size()*sizeof(banks::GpuVertex));
        HRESULT hr=vb->Lock(0,vbytes,&data,D3DLOCK_DISCARD);
        if(FAILED(hr))return failed(hr,"cloud VB lock");
        std::memcpy(data,mesh.vertices.data(),vbytes);
        return failed(vb->Unlock(),"cloud VB unlock");
    }
};
struct GpuPasses {
    GpuStream* stream{};
    std::array<std::array<float,4>,10> constants{};
    D3DMATRIX scene{},distant{};
    D3DVIEWPORT9 viewport{};
    // Hardware clips both sides of the old far plane with complementary
    // projections. The distant pass flattens only viewport depth; XY and W
    // remain identical, so clouds never move and near scene geometry wins.
    bool prepare(const Settings& s,const View& v,const Range& r,const canopy::Drift& drift,
                 bool farReady,bool vertexFog,float visibility,RGB color,const D3DMATRIX& transform,
                 const D3DMATRIX& projection,const D3DVIEWPORT9& vp) {
        if(std::abs(projection._34-1.f)>.0001f||std::abs(projection._44)>.0001f||
           projection._33<=1.f||projection._43>=0.f||std::abs(projection._13)>.0001f||std::abs(projection._23)>.0001f)return false;
        const float farClip=projection._43/(1.f-projection._33);
        if(!std::isfinite(farClip)||farClip<=0.f)return false;
        scene=transform;distant=transform;viewport=vp;
        const float extended=std::max(farClip+100.f,2.f*r.end+farClip);
        const float a=extended/(extended-farClip),b=-a*farClip;
        distant._13=a*scene._14;distant._23=a*scene._24;distant._33=a*scene._34;distant._43=a*scene._44+b;
        const auto tex=banks::textureOrigin(s,v,drift);
        const auto lod=banks::lod(s,r,farReady,v.pos.z-(s.minSpawnZ+s.maxSpawnZ)*.5f);
        constants[4]={v.pos.x,v.pos.y,v.pos.z,visibility};
        constants[5]={1.f/tex.tile,tex.u,tex.v,1.f/tex.band};
        constants[6]={v.nearClip,1.f/std::max(80.f,s.nearFadeDistance),s.interiorHaze*.22f,s.minSpawnZ};
        constants[7]={r.end,1.f/(r.end-r.begin),lod.start,lod.end>lod.start?1.f/(lod.end-lod.start):0.f};
        constants[8]={s.cloudFogStart,s.cloudFogEnd-s.cloudFogStart>1.f?1.f/(s.cloudFogEnd-s.cloudFogStart):0.f,vertexFog?1.f:0.f,0.f};
        constants[9]={byte(color.r)/255.f,byte(color.g)/255.f,byte(color.b)/255.f,1.f};
        return true;
    }
    Result draw(IDirect3DDevice9* d,UINT count,UINT triangles) {
        HRESULT hr;
        if(FAILED(hr=d->SetStreamSource(0,stream->vb,0,sizeof(banks::GpuVertex))))return failed(hr,"cloud stream");
        if(FAILED(hr=d->SetStreamSourceFreq(0,1)))return failed(hr,"cloud stream frequency");
        if(FAILED(hr=d->SetIndices(stream->ib)))return failed(hr,"cloud indices");
        for(int pass=0;pass<2;++pass) {
            const auto& m=pass?scene:distant;
            for(int i=0;i<4;++i)constants[i]={m.m[0][i],m.m[1][i],m.m[2][i],m.m[3][i]};
            if(FAILED(hr=d->SetVertexShaderConstantF(0,constants[0].data(),UINT(constants.size()))))break;
            auto vp=viewport;if(!pass)vp.MinZ=vp.MaxZ;
            if(FAILED(hr=d->SetViewport(&vp)))break;
            if(FAILED(hr=d->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,count,UINT((banks::MaxVertices-count)/4*6),triangles)))break;
        }
        const auto restore=d->SetViewport(&viewport);
        return FAILED(restore)?failed(restore,"cloud viewport restore"):failed(hr,"hardware cloud draw");
    }
};
inline Result drawRaw(IDirect3DDevice9* d,const std::array<IDirect3DBaseTexture9*,2>& textures,
                   const Options& opts,const void* vertices,UINT count,UINT stride,DWORD fvf,
                   const std::uint16_t* indices,UINT triangles,float coverage,bool fog,
                   bool projected,bool mipmaps,DWORD fogColor,
                   const D3DMATRIX* view=nullptr,const D3DMATRIX* projection=nullptr,bool ownState=true,GpuPasses* gpu=nullptr) {
    if(!count||!triangles)return {};
    StateScope scoped;
    if(ownState){const auto capture=scoped.capture(d);if(!capture)return capture;}
    // CreateStateBlock already captures state, including on pure devices.
    // Do not use GetTransform, GetRenderState, etc., which pure devices prohibit.
    Result result;
    const auto check=[&](HRESULT hr,const char* op){if(!SUCCEEDED(hr)&&result)result=failed(hr,op);};
    const auto rs=[&](D3DRENDERSTATETYPE st,DWORD value){check(d->SetRenderState(st,value),"SetRenderState");};
    const auto ts=[&](DWORD stage,D3DTEXTURESTAGESTATETYPE st,DWORD value){check(d->SetTextureStageState(stage,st,value),"SetTextureStageState");};
    const auto ss=[&](DWORD stage,D3DSAMPLERSTATETYPE st,DWORD value){check(d->SetSamplerState(stage,st,value),"SetSamplerState");};
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
    rs(D3DRS_ZENABLE,D3DZB_TRUE);rs(D3DRS_ZWRITEENABLE,FALSE);
    rs(D3DRS_ZFUNC,D3DCMP_LESSEQUAL);rs(D3DRS_ALPHABLENDENABLE,TRUE);
    rs(D3DRS_ALPHATESTENABLE,TRUE);rs(D3DRS_ALPHAFUNC,D3DCMP_GREATER);rs(D3DRS_ALPHAREF,0);
    rs(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);rs(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
    rs(D3DRS_BLENDOP,D3DBLENDOP_ADD);rs(D3DRS_SEPARATEALPHABLENDENABLE,FALSE);
    rs(D3DRS_CULLMODE,D3DCULL_NONE);rs(D3DRS_SHADEMODE,D3DSHADE_GOURAUD);
    rs(D3DRS_COLORWRITEENABLE,0xF);rs(D3DRS_SRGBWRITEENABLE,FALSE);rs(D3DRS_STENCILENABLE,FALSE);
    rs(D3DRS_SCISSORTESTENABLE,FALSE);rs(D3DRS_CLIPPING,TRUE);
    rs(D3DRS_CLIPPLANEENABLE,0);rs(D3DRS_WRAP0,0);rs(D3DRS_WRAP1,0);
    const auto factor=byte(coverage*255.f);
    rs(D3DRS_TEXTUREFACTOR,D3DCOLOR_ARGB(factor,factor,factor,factor));
    const bool blend=opts.dual&&coverage>.001f&&coverage<.999f;
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
    if(result&&gpu)result=gpu->draw(d,count,triangles);
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
                            RGB fogColor,bool fog=true,bool ownState=true) {
    return drawRaw(d,textures,opts,mesh.vertices.data(),UINT(mesh.vertices.size()),sizeof(ScreenVertex),
        D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_SPECULAR|D3DFVF_TEX1|D3DFVF_TEXCOORDSIZE3(0),
        mesh.indices.data(),UINT(mesh.indices.size()/3),coverage,fog&&opts.vertexFog,true,!volume&&opts.mip,
        D3DCOLOR_XRGB(byte(fogColor.r),byte(fogColor.g),byte(fogColor.b)),nullptr,nullptr,ownState);
}
} // namespace vc::d3d9
