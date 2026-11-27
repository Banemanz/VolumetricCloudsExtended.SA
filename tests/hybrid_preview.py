"""Software diagnostic of actual mode-2 textures and depth meshes, no GTA assets.
Usage: python tests/hybrid_preview.py ./hybrid_preview docs/previews
"""
import pathlib
import subprocess
import sys
import tempfile
import numpy as np
from PIL import Image,ImageDraw
from canopy_preview import clip

W,H=960,540

def draw_mesh(canvas,tex,v,indices,wrap):
    h,w=tex.shape[:2]
    # One draw per proxy. Guard shared edges and folded mesh triangles.
    depth=np.full(canvas.shape[:2],np.inf)
    old=canvas.copy()
    for tri in indices:
        poly=clip(v[tri])
        for j in range(1,len(poly)-1):
            a=np.array([poly[0],poly[j],poly[j+1]])
            xy=a[:,:2]/a[:,2,None]*510*[1,-1]+[W/2,H/2]
            lo=np.maximum(0,np.floor(xy.min(0))).astype(int)
            hi=np.minimum([W,H],np.ceil(xy.max(0))).astype(int)
            if np.any(lo>=hi):continue
            yy,xx=np.mgrid[lo[1]:hi[1],lo[0]:hi[0]]
            px=xx+.5-xy[0,0];py=yy+.5-xy[0,1]
            dx,dy=xy[1]-xy[0];ex,ey=xy[2]-xy[0];den=dx*ey-dy*ex
            if abs(den)<1e-9:continue
            b1=(px*ey-py*ex)/den;b2=(dx*py-dy*px)/den;b0=1-b1-b2
            m=(b0>=-1e-8)&(b1>=-1e-8)&(b2>=-1e-8)
            if not np.any(m):continue
            weights=np.stack([b0[m],b1[m],b2[m]],axis=1)/a[:,2]
            z=1/weights.sum(1);y,x=yy[m],xx[m]
            keep=z<depth[y,x]
            if not np.any(keep):continue
            y,x=y[keep],x[keep];weights=weights[keep]*z[keep,None]
            depth[y,x]=z[keep];attr=weights@a[:,3:]
            uv=attr[:,:2]*[w,h]-.5
            ix=np.floor(uv[:,0]).astype(int);iy=np.floor(uv[:,1]).astype(int)
            fx=(uv[:,0]-ix)[:,None];fy=(uv[:,1]-iy)[:,None]
            if wrap:
                jx=(ix+1)%w;jy=(iy+1)%h;ix%=w;iy%=h
            else:
                jx=np.clip(ix+1,0,w-1);jy=np.clip(iy+1,0,h-1);ix=np.clip(ix,0,w-1);iy=np.clip(iy,0,h-1)
            rgba=(tex[iy,ix]*(1-fx)+tex[iy,jx]*fx)*(1-fy)+(tex[jy,ix]*(1-fx)+tex[jy,jx]*fx)*fy
            alpha=(rgba[:,3]*attr[:,2])[:,None]
            canvas[y,x]=old[y,x]*(1-alpha)+rgba[:,:3]*alpha

def render(scene,exe):
    with tempfile.TemporaryDirectory() as tmp:
        path=pathlib.Path(tmp)/"cloud"
        raw=subprocess.check_output([exe,scene,str(path)],text=True)
        tex=[np.fromfile(str(path)+suffix,dtype=np.uint8).reshape(h,w,4).astype(np.float32)/255
             for suffix,h,w in [(".canopy",512,512),(".volume",1024,1024)]]
    rows=iter(raw.splitlines())
    p=np.array(next(rows).split(),float);t=np.linspace(0,1,H)[:,None,None]
    canvas=np.broadcast_to((p[:3]*(1-t)+p[3:6]*t)/255,(H,W,3)).copy()
    for line in rows:
        layer,nv,nt=map(int,line.split())
        v=np.array([next(rows).split() for _ in range(nv)],float)
        indices=np.array([next(rows).split() for _ in range(nt)],int)
        draw_mesh(canvas,tex[layer],v,indices,layer==0)
    im=Image.fromarray(np.uint8(np.clip(canvas,0,1)*255))
    d=ImageDraw.Draw(im);d.rectangle((0,0,W,24),fill=(15,23,35))
    d.text((8,6),f"HYBRID / {scene.upper()} - actual C++ density atlas + mesh / synthetic sky, not GTA",fill=(242,246,255))
    return im

if __name__=="__main__":
    out=pathlib.Path(sys.argv[2]);out.mkdir(parents=True,exist_ok=True)
    for scene in ("day","zenith","sunrise","night","rain","flight"):
        render(scene,sys.argv[1]).save(out/f"hybrid_{scene}.jpg",quality=88,optimize=True)
    sheet=Image.new("RGB",(960,360))
    for i,a in enumerate((0,15,30,45,60,75)):
        im=render(f"orbit{a}",sys.argv[1]).resize((320,180))
        sheet.paste(im,(i%3*320,i//3*180))
    sheet.save(out/"hybrid_orbit.jpg",quality=91,optimize=True)
