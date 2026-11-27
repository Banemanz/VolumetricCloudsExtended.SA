// This test uses the actual plugin's D3D9 upload, FVF, sampler and draw path.
// It does NOT mock a device or emulate the rasterizer. If the machine has
// no usable D3D9 device, CTest marks it skipped (not passed).
#include "../CloudD3D9.hpp"
#include <windows.h>
#include <cstdio>
#include <cmath>

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
    const float wx=.94f,wy=.53f;
    const Vec forward=unit(Vec{.96f,-.27f,-.17f});
    const Vec right=unit(cross(forward,{0,0,1})),up=cross(right,forward);
    for(const Vec pos:{Vec{2164,-1265,673},Vec{2255,-1341,672.7f},Vec{2266,-1271,490}}) {
        View camera{pos,right,up,forward,2200,1};
        auto view=d3d9::identity();
        view._11=right.x;view._21=right.y;view._31=right.z;view._41=-dot(pos,right);
        view._12=up.x;view._22=up.y;view._32=up.z;view._42=-dot(pos,up);
        view._13=forward.x;view._23=forward.y;view._33=forward.z;view._43=-dot(pos,forward);
        D3DMATRIX projection{};
        projection._11=1.f/wx;projection._22=1.f/wy;
        projection._33=camera.farClip/(camera.farClip-camera.nearClip);
        projection._34=1;projection._43=-camera.nearClip*projection._33;
        banks::Mesh mesh;banks::buildMesh(mesh,settings,camera,range(settings,camera.farClip,1600),{},wx,wy,
            uploadWidth==volume.width&&uploadDepth==volume.depth?&volume:nullptr,0);
        if(mesh.indices.empty())return fail("No occupied 3D geometry in flight view");
        std::vector<d3d9::Vertex> batch;
        for(const auto& v:mesh.vertices)batch.push_back({v.pos.x,v.pos.y,v.pos.z,
            D3DCOLOR_ARGB(d3d9::byte(v.alpha*255),208,208,208),v.u,v.v,v.w});
        if(FAILED(hr=device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xFF000000,1,0)))return fail("Scene Clear",hr);
        if(FAILED(hr=device->BeginScene()))return fail("Scene BeginScene",hr);
        const auto result=d3d9::draw(device,textures,opts,batch.data(),UINT(batch.size()),mesh.indices.data(),
            UINT(mesh.indices.size()/3),0,false,&view,&projection);
        const auto end=device->EndScene();
        if(!result)return fail(result.operation,result.hr);
        if(FAILED(end))return fail("Scene EndScene",end);
        if(FAILED(hr=device->GetRenderTargetData(back.p,readback.p)))return fail("Scene readback",hr);
        D3DLOCKED_RECT lock{};
        if(FAILED(hr=readback->LockRect(&lock,nullptr,D3DLOCK_READONLY)))return fail("Scene LockRect",hr);
        unsigned visible=0,empty=0,peak=0;
        for(UINT y=0;y<desc.Height;++y)for(UINT x=0;x<desc.Width;++x) {
            DWORD colour=0;
            std::memcpy(&colour,static_cast<const char*>(lock.pBits)+std::size_t(y)*lock.Pitch+x*sizeof(DWORD),sizeof(DWORD));
            const unsigned value=colour&255;
            visible+=value>8;empty+=value<=2;peak=std::max(peak,value);
        }
        readback->UnlockRect();
        std::printf("Flight scene x=%.1f y=%.1f z=%.1f: %d planes, %u visible pixels, %u clear pixels, peak %u\n",
            pos.x,pos.y,pos.z,mesh.slices,visible,empty,peak);
        if(visible<desc.Width*desc.Height/64 || (pos.z>settings.maxSpawnZ&&empty<desc.Width*desc.Height/64) || peak<25 || peak>209)
            return fail("Flight scene readback has no cloud/sky contrast");
    }
    return 0;
}
}
int main() {
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
        D3DCAPS9 caps{};
        if(FAILED(api->GetDeviceCaps(D3DADAPTER_DEFAULT,type,&caps)))continue;
        for(DWORD flags:{DWORD(D3DCREATE_HARDWARE_VERTEXPROCESSING|D3DCREATE_PUREDEVICE),
                         DWORD(D3DCREATE_HARDWARE_VERTEXPROCESSING),
                         DWORD(D3DCREATE_SOFTWARE_VERTEXPROCESSING)}) {
            D3DPRESENT_PARAMETERS pp{};
            pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=window;
            pp.BackBufferWidth=pp.BackBufferHeight=128;pp.BackBufferFormat=D3DFMT_X8R8G8B8;
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
            const int result=run(device.p,actual);
            if(result==77)continue;
            if(result!=0)return result;
            any=true;pure|=(flags&D3DCREATE_PUREDEVICE)!=0;
            if(pure)break;
        }
        if(any)break;
    }
    if(!any){std::puts("SKIP: no available D3D9 volume-capable device.");return 77;}
    std::puts(pure?"PASS: actual pure-device 3D texture rendering/readback.":"PASS: actual non-pure 3D rendering/readback; PURE was unavailable.");
    return 0;
}
