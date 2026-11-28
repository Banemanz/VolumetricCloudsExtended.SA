// Standalone old/new comparison: compile against either revision's headers
// with -I<checkout>. Compare hashes only under identical compiler/FP flags.
// Economy density is deliberately held constant to isolate the geometry
// path from baking while changing sampling detail and draw range.
#include "CloudBillboards.hpp"
#include <chrono>
#include <iostream>
using namespace vc;
int main() {
 Settings s;canopy::Map map;map.build(canopy::textureSize(s),s.seed);
 banks::Volume v;v.build(s,map);banks::GpuMesh gpu;banks::Mesh cpu;canopy::Drift drift;
 uint64_t h=14695981039346656037ull;
 auto add=[&](const void* data,size_t n){auto p=(const unsigned char*)data;while(n--){h^=*p++;h*=1099511628211ull;}};
 double ms=0,mscpu=0;size_t n=0;
 for(int i=0;i<1440;++i){
  s.cumulusDetail=i>=960;s.cloudDrawDistance=i<480?8000:i<960?4200:12000;
  float yaw=i*.043f; Vec f=unit(Vec{std::cos(yaw),std::sin(yaw),-.25f+.12f*std::sin(yaw*3)});
  if(i%17==0)f=unit(Vec{.1f,.05f,i%2?1.f:-1.f});
  Vec rt=unit(cross(f,{0,0,1}));View view{{2164.f+i*4,-1265.f-i*1.2f,350.f+float(i%90)*5},rt,cross(rt,f),f,450,1};
  const float w=float(i%100)/100;const auto r=cloudRange(s);
  drift.update(s,{.2f,.1f,0},.016f);
  auto t=std::chrono::steady_clock::now();banks::buildMesh(gpu,s,view,r,drift,.94f,.53f,&v,w);
  ms+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t).count();
  t=std::chrono::steady_clock::now();banks::buildMesh(cpu,s,view,r,drift,.94f,.53f,&v,w);
  mscpu+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t).count();
  add(gpu.vertices.data(),gpu.vertices.size()*sizeof(banks::GpuVertex));add(gpu.spans.data(),gpu.spans.size()*sizeof(banks::VertexSpan));
  add(cpu.vertices.data(),cpu.vertices.size()*sizeof(banks::Vertex));add(cpu.indices.data(),cpu.indices.size()*sizeof(uint16_t));n+=gpu.vertices.size();
 }
 std::cout<<h<<" GPU "<<ms/1440<<"ms CPU "<<mscpu/1440<<"ms "<<n<<" verts\n";
}
