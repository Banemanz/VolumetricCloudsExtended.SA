#pragma once
// Standalone diagnostic: the actual D3D9 cloud backend, texture formats,
// matrices, occupancy, fog, weather, depth and draw order. No GTA assets.
// Completion readings include driver/GPU work, not just CPU submission.
// Do not extrapolate this machine's timing to a Celeron/Intel HD device.
#include <chrono>
#include <cstdio>
#include <vector>

namespace synthetic {
using Clock=std::chrono::steady_clock;
inline double ms(Clock::time_point a,Clock::time_point b) {
    return std::chrono::duration<double,std::milli>(b-a).count();
}
template<class T> struct Owner {
    T* p{};
    ~Owner(){if(p)p->Release();}
};
inline bool wait(IDirect3DQuery9* query,void* data,DWORD size) {
    const auto deadline=Clock::now()+std::chrono::seconds(5);
    HRESULT hr;
    do {
        hr=query->GetData(data,size,D3DGETDATA_FLUSH);
        if(hr!=S_FALSE)return hr==S_OK;
        Sleep(0);
    } while(Clock::now()<deadline);
    return false;
}
inline double percentile(std::vector<double> values,unsigned p) {
    if(values.empty())return -1.;
    std::sort(values.begin(),values.end());
    return values[(values.size()-1)*p/100];
}
inline int benchmark(IDirect3DDevice9* device,const D3DCAPS9& caps,bool half=false) {
    using namespace vc;
    using namespace vc::d3d9;
    D3DDEVICE_CREATION_PARAMETERS creation{};
    D3DVIEWPORT9 vp{};
    if(FAILED(device->GetCreationParameters(&creation))||FAILED(device->GetViewport(&vp)))return 1;
    Owner<IDirect3D9> api;
    if(FAILED(device->GetDirect3D(&api.p)))return 1;
    D3DADAPTER_IDENTIFIER9 ident{};
    if(SUCCEEDED(api.p->GetAdapterIdentifier(creation.AdapterOrdinal,0,&ident)))
        std::printf("Benchmark adapter: %s | %s | vendor=%04lx device=%04lx type=%d flags=%lx\n",
            ident.Description,ident.Driver,static_cast<unsigned long>(ident.VendorId),
            static_cast<unsigned long>(ident.DeviceId),int(creation.DeviceType),static_cast<unsigned long>(creation.BehaviorFlags));
    std::printf("Synthetic D3D9 %ux%u; no GTA simulation, not a target GPU emulator. Milliseconds on this adapter only.\n",vp.Width,vp.Height);
    if(!(caps.TextureCaps&D3DPTEXTURECAPS_VOLUMEMAP)||caps.MaxVolumeExtent<banks::Width)return 77;
    Owner<IDirect3DQuery9> event,occlusion,ts0,ts1,freq,disjoint;
    if(FAILED(device->CreateQuery(D3DQUERYTYPE_EVENT,&event.p)))return 77;
    const bool samples=SUCCEEDED(device->CreateQuery(D3DQUERYTYPE_OCCLUSION,&occlusion.p));
    bool timestamps=SUCCEEDED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP,&ts0.p))&&
        SUCCEEDED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP,&ts1.p))&&
        SUCCEEDED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ,&freq.p))&&
        SUCCEEDED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT,&disjoint.p));
    std::printf("Query capabilities: timestamps %s, alpha/depth-passing samples %s\n",timestamps?"yes":"no",samples?"yes":"no");
    Settings s;canopy::Map map;map.build(canopy::textureSize(s),s.seed);
    banks::Volume volume;volume.build(s,map);
    canopyVolume::Relief relief;relief.build(s,volume);
    canopyVolume::MaskPair masks;canopyVolume::project(s,volume,canopy::textureSize(s),masks);
    billboards::Atlas atlas;atlas.build(s,volume);
    auto bankOpts=options(caps),sheetOpts=bankOpts,atlasOpts=bankOpts;
    const DWORD linear=D3DPTFILTERCAPS_MINFLINEAR|D3DPTFILTERCAPS_MAGFLINEAR;
    sheetOpts.filter=atlasOpts.filter=(caps.TextureFilterCaps&linear)==linear?D3DTEXF_LINEAR:D3DTEXF_POINT;
    sheetOpts.mip=(caps.TextureFilterCaps&D3DPTFILTERCAPS_MIPFLINEAR)!=0;atlasOpts.mip=false;
    std::array<IDirect3DVolumeTexture9*,2> bank{};
    std::array<IDirect3DTexture9*,2> sheet{},farAtlasTex{};
    // Put the textures under RAII even if a later pair fails.
    std::array<Owner<IDirect3DVolumeTexture9>,2> bankOwner;
    std::array<Owner<IDirect3DTexture9>,2> sheetOwner,farOwner;
    D3DFORMAT bf{},sf{},ff{};
    const auto b=uploadPair(bank,compactFormat(device,D3DRTYPE_VOLUMETEXTURE,bankOpts.filter==D3DTEXF_LINEAR),bf,
        [&](int i,IDirect3DVolumeTexture9** out,bool managed,D3DFORMAT f){
            return upload(device,volume,i,volume.width,volume.depth,out,managed,f);
        });
    for(int i=0;i<2;++i)bankOwner[i].p=bank[i];
    if(!b)return 1;
    const auto c=uploadPair(sheet,compactFormat(device,D3DRTYPE_TEXTURE,sheetOpts.filter==D3DTEXF_LINEAR),sf,
        [&](int i,IDirect3DTexture9** out,bool managed,D3DFORMAT f){
            return uploadCanopy(device,masks[i],canopy::textureSize(s),out,managed,true,f);
        });
    for(int i=0;i<2;++i)sheetOwner[i].p=sheet[i];
    if(!c)return 1;
    const auto a=uploadPair(farAtlasTex,compactFormat(device,D3DRTYPE_TEXTURE,atlasOpts.filter==D3DTEXF_LINEAR),ff,
        [&](int i,IDirect3DTexture9** out,bool managed,D3DFORMAT f){
            return uploadCanopy(device,atlas.pixels[i],billboards::AtlasSize,out,managed,false,f);
        });
    for(int i=0;i<2;++i)farOwner[i].p=farAtlasTex[i];
    if(!a)return 1;
    GpuStream hw;FarLayer farLayer;
    const bool hardware=hw.create(device,caps,creation.BehaviorFlags);
    std::printf("Volume %s, canopy %s, atlas %s; hardware VS2: %s\n",
        bf==D3DFMT_A8L8?"A8L8":"RGBA",sf==D3DFMT_A8L8?"A8L8":"RGBA",ff==D3DFMT_A8L8?"A8L8":"RGBA",hardware?"yes":"no");
    canopyVolume::Mesh proxy;
    banks::Mesh card,cpu;
    banks::GpuMesh gpu;
    ProjectedMesh ps,pa,pb;
    StateScope scoped;
    constexpr int warm=4,frames=24;
    const float wx=.94f,wy=wx*vp.Height/vp.Width;
    const RGB light{208,208,208},fog{90,120,160};
    const auto full=cloudRange(s);
    // Fixed and moving camera samples; two clear/mixed-weather scenarios.
    // Use a short scene clip and a depth occluder, simulating sky visible
    // between buildings without loading a proprietary world.
    for(float height:{30.f,443.f,535.f,635.f,1000.f})for(float coverage:{0.f,.5f}) {
        std::vector<double> prep,submit,complete,gpuTime,passed;
        std::size_t input=0,submitted=0,bytes=0,skipped=0,tri=0,halfFrames=0;
        canopy::Drift drift;
        for(int frame=0;frame<warm+frames;++frame) {
            const int moving=std::max(0,frame-warm-7);
            const float yaw=.7f+moving*.19f,pitch=height<440.f?.65f:-.24f+moving*.012f;
            const Vec f=unit(Vec{std::cos(yaw)*std::cos(pitch),std::sin(yaw)*std::cos(pitch),std::sin(pitch)});
            const Vec right=unit(cross(f,{0,0,1})),up=cross(right,f);
            const Vec pos{2239.f+moving*3.f,-1262.f-moving*.8f,height};
            const View cam{pos,right,up,f,450,1};
            drift.update(s,{.2f,.1f,0},.016f);
            const auto begin=Clock::now();
            const auto blend=canopy::handover(s,height);
            const bool directional=billboards::eligible(s,full);
            const auto range=directional?full:canopyVolume::volumeRange(s,full,height,blend);
            const auto lod=banks::hybridLod(s,full,height,cam.pos.z);
            const auto* sharedLod=directional?&lod:nullptr;
            const bool farReady=directional||billboards::eligible(s,range);
            auto view=identity();
            view._11=right.x;view._21=right.y;view._31=right.z;view._41=-dot(pos,right);
            view._12=up.x;view._22=up.y;view._32=up.z;view._42=-dot(pos,up);
            view._13=f.x;view._23=f.y;view._33=f.z;view._43=-dot(pos,f);
            D3DMATRIX projection{};projection._11=1.f/wx;projection._22=1.f/wy;
            projection._33=450.f/449.f;projection._34=1;projection._43=-projection._33;
            const auto transform=multiply(view,projection);
            if(directional){proxy.vertices.clear();proxy.indices.clear();}else canopyVolume::mesh(proxy,relief,s,cam,full,drift,wx,wy,blend,&range);
            ps.clip.clear();
            for(const auto& v:proxy.vertices) {
                const float fogWeight=cloudFogFactor(s,v.distance);
                ps.clip.push_back(clipVertex(v.pos,v.u,v.v,0,v.alpha*(sheetOpts.vertexFog?1.f:fogWeight),fogWeight,transform));
            }
            projectMesh(ps,vp,light,1.f,proxy.indices.data(),proxy.indices.size());
            pa.clip.clear();pa.indices.clear();
            if(farReady) {
                billboards::buildMesh(card,s,cam,range,drift,atlas,wx,wy,atlasOpts.dual?coverage:0.f,sharedLod);
                for(const auto& v:card.vertices)pa.clip.push_back(clipVertex(v.pos,v.u,v.v,v.w,v.alpha*(atlasOpts.vertexFog?1.f:v.fog),v.fog,transform));
                projectMesh(pa,vp,light,1.f,card.indices.data(),card.indices.size());
            }
            GpuPasses passes;passes.stream=&hw;if(half)passes.farLayer=&farLayer;
            const bool useGpu=hardware&&passes.prepare(s,cam,range,drift,farReady,bankOpts.vertexFog,1.f,light,transform,projection,vp,sharedLod);
            UINT n=0,nt=0;
            if(useGpu) {
                banks::buildMesh(gpu,s,cam,range,drift,wx,wy,&volume,bankOpts.dual?coverage:0.f,farReady,true,true,sharedLod);
                n=UINT(gpu.vertices.size());nt=UINT(gpu.triangleCount());
                passes.restrictTo(gpu);
            } else {
                banks::buildMesh(cpu,s,cam,range,drift,wx,wy,&volume,bankOpts.dual?coverage:0.f,farReady,true,true,sharedLod);
                pb.clip.clear();
                for(const auto& v:cpu.vertices)pb.clip.push_back(clipVertex(v.pos,v.u,v.v,v.w,v.alpha*(bankOpts.vertexFog?1.f:v.fog),v.fog,transform));
                projectMesh(pb,vp,light,1.f,cpu.indices.data(),cpu.indices.size());
                n=UINT(pb.vertices.size());nt=UINT(pb.indices.size()/3);
            }
            const auto ready=Clock::now();
            if(FAILED(device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xFF50779F,1.f,0)))return 1;
            const D3DRECT occluder{0,LONG(vp.Height*3/4),LONG(vp.Width),LONG(vp.Height)};
            if(FAILED(device->Clear(1,&occluder,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xFF445566,.5f,0)))return 1;
            if(FAILED(device->BeginScene()))return 1;
            if(timestamps)timestamps=SUCCEEDED(disjoint.p->Issue(D3DISSUE_BEGIN))&&SUCCEEDED(ts0.p->Issue(D3DISSUE_END));
            if(samples&&FAILED(occlusion.p->Issue(D3DISSUE_BEGIN)))return 1;
            const auto submittedStart=Clock::now();
            if(!scoped.capture(device))return 1;
            vc::d3d9::DrawState states;
            if(!drawProjected(device,{sheet[0],sheet[1]},sheetOpts,ps,coverage,false,fog,true,false,&states))return 1;
            bool usedProxy=false;
            if(!drawFarProjected(device,{farAtlasTex[0],farAtlasTex[1]},atlasOpts,pa,coverage,fog,vp,half?&farLayer:nullptr,&states,&usedProxy))return 1;
            bool reused=false;
            if(useGpu&&n) {
                if(!hw.uploadReusable(gpu,&reused))return 1;
                if(!drawRaw(device,{bank[0],bank[1]},bankOpts,nullptr,n,sizeof(banks::GpuVertex),0,nullptr,nt,
                    coverage,bankOpts.vertexFog,true,false,D3DCOLOR_XRGB(vc::d3d9::byte(fog.r),vc::d3d9::byte(fog.g),vc::d3d9::byte(fog.b)),
                    nullptr,nullptr,false,&passes,&states))return 1;
            } else if(!useGpu&&nt) {
                if(!drawProjected(device,{bank[0],bank[1]},bankOpts,pb,coverage,true,fog,true,false,&states))return 1;
            }
            if(!scoped.restore())return 1;
            const auto submittedEnd=Clock::now();
            if(samples&&FAILED(occlusion.p->Issue(D3DISSUE_END)))return 1;
            if(timestamps)timestamps=SUCCEEDED(ts1.p->Issue(D3DISSUE_END))&&SUCCEEDED(freq.p->Issue(D3DISSUE_END))&&SUCCEEDED(disjoint.p->Issue(D3DISSUE_END));
            if(FAILED(device->EndScene())||FAILED(event.p->Issue(D3DISSUE_END)))return 1;
            BOOL done=FALSE;
            if(!wait(event.p,&done,sizeof(done))||!done)return 1;
            const auto completed=Clock::now();
            DWORD pixels=0;UINT64 start=0,end=0,hz=0;BOOL broken=FALSE;
            bool gpuValid=false;
            if(samples&&!wait(occlusion.p,&pixels,sizeof(pixels)))return 1;
            if(timestamps) {
                gpuValid=wait(ts0.p,&start,sizeof(start))&&wait(ts1.p,&end,sizeof(end))&&
                    wait(freq.p,&hz,sizeof(hz))&&wait(disjoint.p,&broken,sizeof(broken))&&!broken&&hz&&end>=start;
            }
            if(frame>=warm) {
                prep.push_back(ms(begin,ready));submit.push_back(ms(submittedStart,submittedEnd));
                complete.push_back(ms(submittedStart,completed));
                if(gpuValid)gpuTime.push_back(1000.*double(end-start)/double(hz));
                if(samples)passed.push_back(double(pixels));
                input+=n;submitted+=useGpu?passes.ranges[0].count()+passes.ranges[1].count():n;
                bytes+=useGpu&&!reused?n*sizeof(banks::GpuVertex):0;skipped+=reused;halfFrames+=passes.usedFarLayer||usedProxy;
                tri+=ps.indices.size()/3+pa.indices.size()/3+(useGpu?(passes.ranges[0].count()+passes.ranges[1].count())/2:nt);
            }
        }
        std::printf("Z=%4.0f weather=%.1f N=%d | CPU prep med/p95 %.3f/%.3f | submit %.3f/%.3f | completion %.3f/%.3f | GPU interval %.3f/%.3f (N=%zu) | avg verts %zu/%zu tris %zu, VB upload %zu B, reused %zu/%d, median passing samples %.0f, half-res frames %zu\n",
            height,coverage,frames,percentile(prep,50),percentile(prep,95),percentile(submit,50),percentile(submit,95),
            percentile(complete,50),percentile(complete,95),percentile(gpuTime,50),percentile(gpuTime,95),gpuTime.size(),
            input/frames,submitted/frames,tri/frames,bytes/frames,skipped,frames,percentile(passed,50),halfFrames);
    }
    std::puts("Benchmark complete. GPU -1 means unsupported/disjoint; completion includes synchronization, not in-game FPS.");
    return 0;
}
} // namespace synthetic
