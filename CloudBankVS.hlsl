// VS 2.0 only: no pixel shader, depth texture, render target or ray marcher.
// Identical optical weights and UVs to CloudBanks.hpp; a second projection
// clips the geometry beyond the scene far plane at the cleared depth.
float4 cx : register(c0);
float4 cy : register(c1);
float4 cz : register(c2);
float4 cw : register(c3);
float4 eye : register(c4); // xyz eye, w opacity
float4 uv : register(c5); // tile reciprocal, u offset, v offset, band reciprocal
float4 nearData : register(c6); // near clip, reciprocal near fade, interior haze, min z
float4 distances : register(c7); // max range, reciprocal range taper, LOD start, reciprocal LOD transition
float4 fogData : register(c8); // start, reciprocal fog range, hardware fog flag, unused
float4 rgb : register(c9);
float ease(float x) { x=saturate(x);return x*x*(3-2*x); }
struct Output {
    float4 pos : POSITION;
    float4 color : COLOR0;
    float3 tex : TEXCOORD0;
    float3 tex2 : TEXCOORD1;
    float fog : FOG;
};
Output main(float3 position : POSITION, float4 lattice : TEXCOORD0) {
    Output o;
    float4 p=float4(position,1);
    o.pos=float4(dot(p,cx),dot(p,cy),dot(p,cz),dot(p,cw));
    float3 delta=position-eye.xyz;
    float d=length(delta);
    float3 ad=abs(delta);
    float3 weights=saturate((ad/max(max(max(ad.x,ad.y),ad.z),.001)-.8)*5);
    weights=weights*weights*(3-2*weights);
    float3 axis=saturate(1-abs(lattice.x-float3(0,1,2)));
    float w=dot(weights,axis)/max(dot(weights,float3(1,1,1)),.0001);
    float cosine=dot(ad,axis)/max(d,.001);
    float fine=1-ease((d-140)/300);
    float coarse=ease((d-620)/680);
    float path=lattice.y*(fine+lattice.z*2*(1-fine)*(1-coarse)+lattice.w*4*(1-fine)*coarse)/max(cosine,.4);
    float nearWeight=ease((d-8)/24)*lerp(ease((d-nearData.x)*nearData.y),1,nearData.z);
    float fade=ease((distances.x-d)*distances.y);
    float lod=1-ease((d-distances.z)*distances.w);
    float fog=1-ease((d-fogData.x)*fogData.y);
    float alpha=saturate(path*w/90)*fade*fade*lod*nearWeight*lerp(fog,1,fogData.z)*eye.w;
    o.color=float4(rgb.xyz,floor(saturate(alpha)*255+.5)/255);
    o.fog=floor(saturate(fog)*255+.5)/255;
    o.tex=float3(position.x*uv.x-uv.y,position.y*uv.x-uv.z,(position.z-nearData.w)*uv.w);
    o.tex2=o.tex;
    return o;
}
