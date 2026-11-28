#pragma once
#include "CloudBillboards.hpp"
#include "CloudCanopyVolume.hpp"
#include <istream>
#include <ostream>
#include <cstring>

// The periodic world schematic is immutable. Weather changes blend its two
// baked endpoints and wind shifts coordinates; neither requires disk writes.
namespace vc::cache {
constexpr std::uint32_t Version=4; // elevation-aware cell atlas
static_assert(sizeof(canopy::Pixel)==4);
struct Hash {
    std::uint64_t value=14695981039346656037ull;
    void add(const void* p,std::size_t n) {
        const auto* b=static_cast<const unsigned char*>(p);
        while(n--){value^=*b++;value*=1099511628211ull;}
    }
    template<class T> void add(T v){add(&v,sizeof(v));}
};
inline std::uint64_t key(const Settings& s,int size,std::uint32_t version=Version) {
    Hash h;
    h.add(version);h.add(size);h.add(s.seed);h.add(s.targetProcClouds);
    h.add(s.badWeatherDensityMultiplier);h.add(s.minSpawnZ);h.add(s.maxSpawnZ);
    h.add(s.sizeScale);h.add(s.opacity);h.add(s.cumulusDensity);
    h.add(s.cumulusContrast);h.add(s.cumulusRimStrength);h.add(int(s.cumulusDetail));
    h.add(int(s.hybridClouds));h.add(int(s.hybridClouds&&billboards::eligible(s,cloudRange(s))));
    return h.value;
}
using Masks=std::array<std::vector<canopy::Pixel>,2>;
struct Header {std::uint64_t magic,signature;std::uint32_t version,size;};
constexpr std::uint64_t Magic=0x3145484341434356ull; // VCCACHE1
static_assert(sizeof(Header)==24);
inline bool transfer(std::istream& in,void* p,std::size_t n,Hash& h) {
    if(!in.read(static_cast<char*>(p),std::streamsize(n)))return false;
    h.add(p,n);return true;
}
inline bool transfer(std::ostream& out,const void* p,std::size_t n,Hash& h) {
    if(!out.write(static_cast<const char*>(p),std::streamsize(n)))return false;
    h.add(p,n);return true;
}
inline bool load(std::istream& in,const Settings& s,int size,Masks& masks,banks::Volume& volume,billboards::Atlas& atlas) {
    Header header{};Hash h;
    if(!transfer(in,&header,sizeof(header),h)||header.magic!=Magic||header.version!=Version||
       header.signature!=key(s,size,header.version)||header.size!=std::uint32_t(size))return false;
    // File lengths are never used as allocation sizes. Even malformed input
    // only allocates the exact dimensions already validated in the INI.
    Masks m; banks::Volume v;billboards::Atlas a;
    const auto readPixels=[&](auto& p,std::size_t n){
        p.resize(n);

        // A bounded sequential read on HDD, then expand losslessly for CPU
        // sampling and devices that lack native luminance texture support.
        std::array<std::uint8_t,32768> packed{};
        for(std::size_t start=0;start<n;) {
            const auto count=std::min(n-start,packed.size()/2);
            if(!transfer(in,packed.data(),count*2,h))return false;
            for(std::size_t i=0;i<count;++i) {
                const auto l=packed[2*i];p[start+i]={l,l,l,packed[2*i+1]};
            }
            start+=count;
        }
        return true;
    };
    for(auto& p:m)if(!readPixels(p,std::size_t(size)*size))return false;
    if(s.hybridClouds) {
        v.width=s.cumulusDetail?banks::DetailWidth:banks::Width;
        v.depth=s.cumulusDetail?banks::DetailDepth:banks::Depth;
        for(auto& p:v.pixels)if(!readPixels(p,std::size_t(v.width)*v.width*v.depth))return false;
        if(!transfer(in,v.occupied.data(),sizeof(v.occupied),h)||
           !transfer(in,v.occupiedPrefix.data(),sizeof(v.occupiedPrefix),h))return false;
        if(billboards::eligible(s,cloudRange(s))) {
            for(auto& p:a.pixels)if(!readPixels(p,billboards::AtlasSize*billboards::AtlasSize))return false;
            for(auto& layer:a.occupied)for(auto& slot:layer) {
                std::uint8_t b=0;if(!transfer(in,&b,1,h)||b>1)return false;slot=b!=0;
            }
        }
    }
    std::uint64_t check=0;
    if(!in.read(reinterpret_cast<char*>(&check),sizeof(check))||check!=h.value||
       in.peek()!=std::char_traits<char>::eof())return false;
    if(s.hybridClouds){v.buildRectangleMasks();v.buildPlaneSupport();if(!a.pixels[0].empty())a.buildBounds();}
    masks=std::move(m);volume=std::move(v);atlas=std::move(a);
    return true;
}
inline bool save(std::ostream& out,const Settings& s,int size,const Masks& masks,const banks::Volume& volume,const billboards::Atlas& atlas,std::uint32_t version=Version) {
    if(version!=Version)return false;
    const Header header{Magic,key(s,size,version),version,std::uint32_t(size)};
    Hash h;if(!transfer(out,&header,sizeof(header),h))return false;
    const auto writePixels=[&](const auto& p,std::size_t n){
        if(p.size()!=n)return false;

        std::array<std::uint8_t,32768> packed{};
        for(std::size_t start=0;start<n;) {
            const auto count=std::min(n-start,packed.size()/2);
            for(std::size_t i=0;i<count;++i) {
                const auto pixel=p[start+i];
                if(pixel.r!=pixel.g||pixel.r!=pixel.b)return false; // never discard a colour channel
                packed[2*i]=pixel.r;packed[2*i+1]=pixel.a;
            }
            if(!transfer(out,packed.data(),count*2,h))return false;
            start+=count;
        }
        return true;
    };
    for(const auto& p:masks)if(!writePixels(p,std::size_t(size)*size))return false;
    if(s.hybridClouds) {
        const auto w=s.cumulusDetail?banks::DetailWidth:banks::Width;
        const auto d=s.cumulusDetail?banks::DetailDepth:banks::Depth;
        if(volume.width!=w||volume.depth!=d)return false;
        for(const auto& p:volume.pixels)if(!writePixels(p,std::size_t(w)*w*d))return false;
        if(!transfer(out,volume.occupied.data(),sizeof(volume.occupied),h)||
           !transfer(out,volume.occupiedPrefix.data(),sizeof(volume.occupiedPrefix),h))return false;
        if(billboards::eligible(s,cloudRange(s))) {
            for(const auto& p:atlas.pixels)if(!writePixels(p,billboards::AtlasSize*billboards::AtlasSize))return false;
            for(const auto& layer:atlas.occupied)for(bool slot:layer) {
                const std::uint8_t b=slot?1:0;if(!transfer(out,&b,1,h))return false;
            }
        }
    }
    return bool(out.write(reinterpret_cast<const char*>(&h.value),sizeof(h.value)));
}
} // namespace vc::cache
