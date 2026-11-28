// Build-time tool: generate the exact default schematic for distribution.
// The ASI will regenerate if any relevant INI setting is customized.
#include "../CloudCache.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
int main(int argc,char** argv) {
    if(argc!=2){std::cerr<<"Usage: cloud_bake_cache OUTPUT.cache\n";return 1;}
    vc::Settings settings;
    const int size=vc::canopy::textureSize(settings);
    vc::canopy::Map map;map.build(size,settings.seed);
    vc::banks::Volume volume;vc::billboards::Atlas atlas;
    if(settings.hybridClouds) {
        volume.build(settings,map);
        if(vc::billboards::eligible(settings,vc::cloudRange(settings)))atlas.build(settings,volume);
    }
    vc::cache::Masks masks;
    if(settings.hybridClouds)vc::canopyVolume::project(settings,volume,size,masks);
    else {
        map.shadeWeatherLayers(settings);
        masks={std::move(map.pixels),std::move(map.extraPixels)};
    }
    const auto path=std::filesystem::path(argv[1]);
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    if(!out||!vc::cache::save(out,settings,size,masks,volume,atlas))return 1;
    out.close();if(!out)return 1;
    std::cout<<"Default cloud schematic: "<<std::filesystem::file_size(path)<<" bytes\n";
}
