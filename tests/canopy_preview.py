"""Rasterize actual canopy C++ texture/mesh, perspective-correct UV wrap.
Usage: python tests/canopy_preview.py ./canopy_preview docs/previews
Synthetic sky, no GTA assets; not a game screenshot.
"""
import csv
import io
import pathlib
import subprocess
import sys
import tempfile
import numpy as np
from PIL import Image, ImageDraw

W,H=960,540
def clip(v):
    result=[]
    for a,b in zip(v,np.roll(v,-1,axis=0)):
        if a[2]>=1: result.append(a)
        if (a[2]>=1)!=(b[2]>=1):
            result.append(a+(b-a)*(1-a[2])/(b[2]-a[2]))
    return result

def render(scene,exe,destination):
    with tempfile.TemporaryDirectory() as work:
        path=pathlib.Path(work)/"cloud.rgba"
        raw=subprocess.check_output([exe,scene,str(path)],text=True)
        size=int(np.sqrt(path.stat().st_size/4))
        tex=np.fromfile(path,dtype=np.uint8).reshape(size,size,4).astype(np.float32)/255
    rows=list(csv.reader(io.StringIO(raw)))
    p=np.array(list(map(float,rows[0])))
    v=np.array(rows[1:258],dtype=float)
    indices=np.array(rows[258:],dtype=int)
    t=np.linspace(0,1,H)[:,None,None]
    canvas=np.broadcast_to((p[:3]*(1-t)+p[3:6]*t)/255,(H,W,3)).copy()
    # Single visible surface; depth guard also handles shared triangle edges.
    depth=np.full((H,W),np.inf)
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
            dx,dy=xy[1]-xy[0];ex,ey=xy[2]-xy[0]
            den=dx*ey-dy*ex
            if abs(den)<1e-9:continue
            b1=(px*ey-py*ex)/den;b2=(dx*py-dy*px)/den;b0=1-b1-b2
            m=(b0>=-1e-8)&(b1>=-1e-8)&(b2>=-1e-8)
            if not np.any(m):continue
            weights=np.stack([b0[m],b1[m],b2[m]],axis=1)/a[:,2]
            z=1/weights.sum(1)
            y,x=yy[m],xx[m]
            keep=z<depth[y,x]
            if not np.any(keep):continue
            y,x=y[keep],x[keep]
            weights=weights[keep]*z[keep,None]
            depth[y,x]=z[keep]
            attr=weights@a[:,3:]
            uv=attr[:,:2]*size-.5
            ix=np.floor(uv[:,0]).astype(int);iy=np.floor(uv[:,1]).astype(int)
            fx=(uv[:,0]-ix)[:,None];fy=(uv[:,1]-iy)[:,None]
            jx=(ix+1)%size;jy=(iy+1)%size;ix%=size;iy%=size
            rgba=(tex[iy,ix]*(1-fx)+tex[iy,jx]*fx)*(1-fy)+(tex[jy,ix]*(1-fx)+tex[jy,jx]*fx)*fy
            alpha=(rgba[:,3]*attr[:,2])[:,None]
            # All triangles share one material. No alpha overdraw at shared edges.
            bg=(p[:3]*(1-t[y,0])+p[3:6]*t[y,0])/255
            canvas[y,x]=bg*(1-alpha)+rgba[:,:3]*alpha
    im=Image.fromarray(np.uint8(np.clip(canvas,0,1)*255))
    d=ImageDraw.Draw(im);d.rectangle((0,0,W,24),fill=(15,23,35))
    d.text((8,6),f"CANOPY / {scene.upper()} - actual procedural texture + mesh / synthetic sky, not GTA",fill=(242,246,255))
    im.save(destination,quality=87,optimize=True)

if __name__=="__main__":
    out=pathlib.Path(sys.argv[2]);out.mkdir(parents=True,exist_ok=True)
    for scene in ("day","zenith","sunrise","night","rain","flight","inside"):
        render(scene,sys.argv[1],out/f"canopy_{scene}.jpg")
