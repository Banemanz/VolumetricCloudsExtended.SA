#pragma once
// Standalone D3D9 volume backend, also compiled by the Win32 device test.
// No RenderWare dependency, shader, or direct pipeline-state getters.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d9.h>
#include <cstring>
#include "CloudBanks.hpp"

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
struct Options {
    D3DTEXTUREFILTERTYPE filter{D3DTEXF_LINEAR};
    D3DTEXTUREOP blendOp{D3DTOP_LERP};
    bool dual{true};
};
inline Options options(const D3DCAPS9& caps) {
    Options o;
    const DWORD linear=D3DPTFILTERCAPS_MINFLINEAR|D3DPTFILTERCAPS_MAGFLINEAR;
    o.filter=(caps.VolumeTextureFilterCaps&linear)==linear?D3DTEXF_LINEAR:D3DTEXF_POINT;
    o.blendOp=(caps.TextureOpCaps&D3DTEXOPCAPS_LERP)?D3DTOP_LERP:D3DTOP_BLENDFACTORALPHA;
    o.dual=caps.MaxSimultaneousTextures>=2&&caps.MaxTextureBlendStages>=3&&
        (caps.TextureOpCaps&(D3DTEXOPCAPS_LERP|D3DTEXOPCAPS_BLENDFACTORALPHA));
    return o;
}
inline Result draw(IDirect3DDevice9* d,const std::array<IDirect3DVolumeTexture9*,2>& textures,
                   const Options& opts,const Vertex* vertices,UINT count,
                   const std::uint16_t* indices,UINT triangles,float coverage,bool fog,
                   const D3DMATRIX* view=nullptr,const D3DMATRIX* projection=nullptr) {
    if(!count||!triangles)return {};
    IDirect3DStateBlock9* saved=nullptr;
    const auto capture=d->CreateStateBlock(D3DSBT_ALL,&saved);
    if(FAILED(capture))return failed(capture,"CreateStateBlock(ALL)");
    // CreateStateBlock already captures state, including on pure devices.
    // Do not use GetTransform, GetRenderState, etc., which pure devices prohibit.
    Result result;
    const auto check=[&](HRESULT hr,const char* op){if(!SUCCEEDED(hr)&&result)result=failed(hr,op);};
    const auto rs=[&](D3DRENDERSTATETYPE st,DWORD value){check(d->SetRenderState(st,value),"SetRenderState");};
    const auto ts=[&](DWORD stage,D3DTEXTURESTAGESTATETYPE st,DWORD value){check(d->SetTextureStageState(stage,st,value),"SetTextureStageState");};
    const auto ss=[&](DWORD stage,D3DSAMPLERSTATETYPE st,DWORD value){check(d->SetSamplerState(stage,st,value),"SetSamplerState");};
    const auto world=identity();
    check(d->SetTransform(D3DTS_WORLD,&world),"SetTransform(WORLD)");
    if(view)check(d->SetTransform(D3DTS_VIEW,view),"SetTransform(VIEW)");
    if(projection)check(d->SetTransform(D3DTS_PROJECTION,projection),"SetTransform(PROJECTION)");
    check(d->SetVertexShader(nullptr),"SetVertexShader");
    check(d->SetPixelShader(nullptr),"SetPixelShader");
    check(d->SetFVF(D3DFVF_XYZ|D3DFVF_DIFFUSE|D3DFVF_TEX1|D3DFVF_TEXCOORDSIZE3(0)),"SetFVF(XYZ,DIFFUSE,TEX3)");
    rs(D3DRS_LIGHTING,FALSE);rs(D3DRS_SPECULARENABLE,FALSE);
    rs(D3DRS_VERTEXBLEND,D3DVBF_DISABLE);rs(D3DRS_INDEXEDVERTEXBLENDENABLE,FALSE);
    rs(D3DRS_FOGENABLE,fog?TRUE:FALSE);rs(D3DRS_ZENABLE,D3DZB_TRUE);rs(D3DRS_ZWRITEENABLE,FALSE);
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
        ss(i,D3DSAMP_MIPFILTER,D3DTEXF_NONE);ss(i,D3DSAMP_SRGBTEXTURE,FALSE);
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
    if(result)check(d->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST,0,count,triangles,
        indices,D3DFMT_INDEX16,vertices,sizeof(Vertex)),"DrawIndexedPrimitiveUP");
    const auto restore=saved->Apply();
    saved->Release();
    // Restore failures are higher priority than draw failures.
    if(FAILED(restore))return failed(restore,"StateBlock::Apply");
    return result;
}
} // namespace vc::d3d9
