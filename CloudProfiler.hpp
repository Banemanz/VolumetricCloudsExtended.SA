#pragma once
#include <d3d9.h>
#include <chrono>
#include <cstdio>

// Opt-in cloud pass timings. No query or timer work without the marker.
// Timestamp queries are sampled every 60 frames; unavailable or late
// results are never flushed, waited for, or replaced with invented times.
namespace vc::d3d9 {
struct CloudProfiler {
    using Clock=std::chrono::steady_clock;
    bool enabled=false,attempted=false,pending=false,active=false;
    IDirect3DQuery9 *start=nullptr,*finish=nullptr,*freq=nullptr,*disjoint=nullptr;
    unsigned frames=0,sequence=0,gpuSequence=0;
    Clock::time_point cpuStart{};
    void release() {
        for(auto** p:{&start,&finish,&freq,&disjoint})if(*p){(*p)->Release();*p=nullptr;}
        attempted=pending=active=false;
    }
    bool init(IDirect3DDevice9* d) {
        if(attempted)return start!=nullptr;
        if(FAILED(d->CreateQuery(D3DQUERYTYPE_TIMESTAMP,&start))||
           FAILED(d->CreateQuery(D3DQUERYTYPE_TIMESTAMP,&finish))||
           FAILED(d->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ,&freq))||
           FAILED(d->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT,&disjoint))) {
            release();attempted=true;return false;
        }
        attempted=true;return true;
    }
    template<class Log>
    void poll(Log log) {
        if(!pending)return;
        UINT64 a=0,b=0,hz=0;BOOL broken=FALSE;
        const auto ra=start->GetData(&a,sizeof(a),0),rb=finish->GetData(&b,sizeof(b),0),
            rf=freq->GetData(&hz,sizeof(hz),0),rd=disjoint->GetData(&broken,sizeof(broken),0);
        if(FAILED(ra)||FAILED(rb)||FAILED(rf)||FAILED(rd)) {
            release();attempted=true;log("Cloud profile: GPU timestamp query failed; CPU samples continue.");return;
        }
        if(ra!=S_OK||rb!=S_OK||rf!=S_OK||rd!=S_OK)return;
        pending=false;
        char line[128];
        if(!broken&&hz&&b>=a) {
            std::snprintf(line,sizeof(line),"Cloud profile #%u: GPU %.3f ms.",gpuSequence,1000.*double(b-a)/double(hz));
            log(line);
        } else log("Cloud profile: discarded a discontinuous GPU timestamp sample.");
    }
    template<class Log>
    bool begin(IDirect3DDevice9* device,Log log) {
        if(!enabled)return false;
        poll(log);
        if((++frames%60)!=1)return false;
        ++sequence;
        if(!attempted&&!init(device))log("Cloud profile: D3D9 timestamp queries unavailable; CPU samples only.");
        if(start&&!pending) {
            if(SUCCEEDED(disjoint->Issue(D3DISSUE_BEGIN))&&SUCCEEDED(start->Issue(D3DISSUE_END))) {
                active=true;gpuSequence=sequence;
            } else {release();attempted=true;}
        }
        cpuStart=Clock::now();return true;
    }
    template<class Log>
    void end(bool sampled,bool success,float playerZ,float coverage,float range,Log log) {
        if(!sampled)return;
        const double cpu=std::chrono::duration<double,std::milli>(Clock::now()-cpuStart).count();
        if(active) {
            active=false;
            if(SUCCEEDED(finish->Issue(D3DISSUE_END))&&SUCCEEDED(freq->Issue(D3DISSUE_END))&&
               SUCCEEDED(disjoint->Issue(D3DISSUE_END)))pending=true;
            else {release();attempted=true;}
        }
        char line[192];
        std::snprintf(line,sizeof(line),"Cloud profile #%u: CPU %.3f ms, player Z %.1f, weather %.2f, 3D range %.0f, %s.",
            sequence,cpu,playerZ,coverage,range,success?"drawn":"fallback");
        log(line);
    }
};
} // namespace vc::d3d9
