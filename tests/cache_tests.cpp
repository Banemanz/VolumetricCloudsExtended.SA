#include "../CloudCache.hpp"
#include <sstream>
#include <cstdlib>
#include <iostream>
#define CHECK(x) do {if(!(x)){std::cerr<<"Failure: "<<#x<<" at "<<__LINE__<<"\n";return 1;}}while(0)
int main(){
    vc::Settings s; s.cumulusDetail=false;
    const int size=vc::canopy::textureSize(s);
    vc::canopy::Map map;map.build(size,s.seed);map.shadeWeatherLayers(s);
    vc::banks::Volume v;v.build(s,map);
    vc::billboards::Atlas a;
    if(vc::billboards::eligible(s,vc::cloudRange(s)))a.build(s,v);
    vc::cache::Masks m{map.pixels,map.extraPixels},loaded;
    std::stringstream f(std::ios::in|std::ios::out|std::ios::binary);
    CHECK(vc::cache::save(f,s,size,m,v,a));
    const auto bytes=f.str();vc::banks::Volume lv;vc::billboards::Atlas la;
    CHECK(vc::cache::load(f,s,size,loaded,lv,la));
    const auto equal=[](const auto& x,const auto& y){return x.size()==y.size()&&(x.empty()||!std::memcmp(x.data(),y.data(),x.size()*sizeof(vc::canopy::Pixel)));};
    for(int i=0;i<2;++i){CHECK(equal(m[i],loaded[i]));CHECK(equal(v.pixels[i],lv.pixels[i]));CHECK(equal(a.pixels[i],la.pixels[i]));}
    CHECK(v.occupied==lv.occupied&&v.occupiedPrefix==lv.occupiedPrefix&&a.occupied==la.occupied);
    // Backward compatibility: existing v1 files remain usable without
    // re-baking, and v2 halves pixel bytes without changing any bake.
    std::stringstream legacy(std::ios::in|std::ios::out|std::ios::binary);
    CHECK(vc::cache::save(legacy,s,size,m,v,a,vc::cache::LegacyVersion));
    CHECK(legacy.str().size()>bytes.size()&&bytes.size()<legacy.str().size()*0.51);
    loaded={};lv={};la={};
    CHECK(vc::cache::load(legacy,s,size,loaded,lv,la));
    for(int i=0;i<2;++i){CHECK(equal(m[i],loaded[i]));CHECK(equal(v.pixels[i],lv.pixels[i]));CHECK(equal(a.pixels[i],la.pixels[i]));}
    CHECK(v.occupied==lv.occupied&&v.occupiedPrefix==lv.occupiedPrefix&&a.occupied==la.occupied);
    auto invalid=m;invalid[0][0]={1,2,3,4};std::ostringstream colour;
    CHECK(!vc::cache::save(colour,s,size,invalid,v,a));
    std::cout<<"Legacy cache "<<legacy.str().size()<<" bytes; compact "<<bytes.size()<<" bytes.\n";
    auto wrong=s;wrong.seed++;std::istringstream badKey(bytes);
    CHECK(!vc::cache::load(badKey,wrong,size,loaded,lv,la));
    auto corrupt=bytes;corrupt[corrupt.size()/2]^=1;std::istringstream badData(corrupt);
    CHECK(!vc::cache::load(badData,s,size,loaded,lv,la));
    std::istringstream truncated(bytes.substr(0,bytes.size()-1));
    CHECK(!vc::cache::load(truncated,s,size,loaded,lv,la));
    wrong=s;wrong.cloudFogStart+=100;CHECK(vc::cache::key(s,size)==vc::cache::key(wrong,size));
    std::stringstream ini;vc::write(ini,s);vc::Settings parsed;vc::load(ini,parsed);
    CHECK(vc::cache::key(s,size)==vc::cache::key(parsed,size));
    std::cout<<"Schematic byte-exact round trip: "<<bytes.size()<<" bytes; bad key, checksum and truncation rejected.\n";
}
