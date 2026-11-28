#pragma once
#include "CloudCore.hpp"

// Portable representation of the live SA 1.0 US weather inputs. The numeric
// weather IDs are from the pinned Plugin-SDK eWeatherType; no engine globals here.
namespace vc::weather {
struct Input {
    int oldType{},newType{};
    float interpolation{},cloudCoverage{},rain{},fog{},sandstorm{},extraSunny{};
    bool scriptRain{},lightning{};
};
struct State {
    float coverage{},storm{},fog{},dust{},extraSunny{};
    bool flash{};
};
inline bool rainy(int id) {return id==8 || id==16;}
inline bool dust(int id) {return id==19;}
inline bool mist(int id) {return id==9;}
inline State sample(const Input& input) {
    const float t=sat(input.interpolation);
    const auto blend=[&](auto pred) {return mix(pred(input.oldType)?1.f:0.f,pred(input.newType)?1.f:0.f,t);};
    // CloudCoverage, Foggyness and ExtraSunnyness are already interpolated
    // by CWeather::Update. Rain and Sandstorm are separately step-limited.
    // Do not let leftover drops keep a whole storm bank after ForceWeatherNow.
    const float stormIntent=blend(rainy),dustIntent=blend(dust);
    const float cloud=sat(input.cloudCoverage);
    const float dustAmount=dustIntent; // residual particle intensity is not cloud coverage
    const float fogAmount=std::max(blend(mist),sat(input.fog)*(1.f-dustAmount));
    const float precip=input.scriptRain?std::max(stormIntent,sat(input.rain)):stormIntent;
    const float cover=std::max(cloud,input.scriptRain?precip:0.f)*(1.f-.75f*dustAmount)*(1.f-.28f*fogAmount);
    return {sat(cover),sat(precip),sat(fogAmount),sat(dustAmount),sat(input.extraSunny),input.lightning};
}
inline float visibility(const State& w) {return sat((1.f-.78f*w.dust)*(1.f-.33f*w.fog));}
inline float clearOpacity(const State& w) {return 1.f-.68f*w.extraSunny;}
} // namespace vc::weather
