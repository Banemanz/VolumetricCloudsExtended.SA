"""Software rasterizer for the actual C++ cumulus mesh and generated RGBA atlas.
Usage: python tests/cumulus_preview.py ./cumulus_preview docs/previews
Synthetic sky only. No GTA executable/assets; not an in-game screenshot.
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
def render(scene,executable,destination):
    with tempfile.TemporaryDirectory() as work:
        atlas_path=pathlib.Path(work)/"atlas.rgba"
        raw=subprocess.check_output([executable,scene,str(atlas_path)],text=True)
        atlas=np.fromfile(atlas_path,dtype=np.uint8).reshape(int(np.sqrt(atlas_path.stat().st_size/4)),int(np.sqrt(atlas_path.stat().st_size/4)),4).astype(np.float32)/255
    size=atlas.shape[0]
    rows=csv.reader(io.StringIO(raw))
    p=np.array(list(map(float,next(rows))))
    t=np.linspace(0,1,H)[:,None,None]
    canvas=np.broadcast_to((p[:3]*(1-t)+p[3:6]*t)/255,(H,W,3)).copy()
    # Illustrative sky glare (not part of the plugin; SA retains the game's sun).
    if not scene.startswith("night") and p[7]>0:
        sx=480+510*p[6]/max(.01,p[7])
        sy=270-510*p[8]/max(.01,p[7])
        yy,xx=np.mgrid[:H,:W]
        glow=np.exp(-((xx-sx)**2+(yy-sy)**2)/(2*80**2))*.8
        canvas=canvas*(1-glow[:,:,None])+glow[:,:,None]
    for row in rows:
        data=np.array(list(map(float,row)))
        n,alpha=int(data[0]),data[1]
        verts=data[2:].reshape(n*n,8)
        # Perspective-correct interpolation by actual mesh triangles. A strict
        # per-card coverage mask prevents double application on shared edges.
        lo=np.maximum(0,np.floor(verts[:,:2].min(0))).astype(int)
        hi=np.minimum([W,H],np.ceil(verts[:,:2].max(0))).astype(int)
        if np.any(lo>=hi):continue
        covered=np.zeros((hi[1]-lo[1],hi[0]-lo[0]),dtype=bool)
        for y in range(n-1):
            for x in range(n-1):
                a=y*n+x;b=a+1;c=a+n;d=c+1
                for tri in ((a,b,c),(b,d,c)):
                    v=verts[list(tri)]
                    low=np.maximum(lo,np.floor(v[:,:2].min(0))).astype(int)
                    high=np.minimum(hi,np.ceil(v[:,:2].max(0))).astype(int)
                    if np.any(low>=high):continue
                    yy,xx=np.mgrid[low[1]:high[1],low[0]:high[0]]
                    px=xx+.5-v[0,0];py=yy+.5-v[0,1]
                    dx=v[1,0]-v[0,0];dy=v[1,1]-v[0,1]
                    ex=v[2,0]-v[0,0];ey=v[2,1]-v[0,1]
                    den=dx*ey-dy*ex
                    if abs(den)<1e-8:continue
                    b1=(px*ey-py*ex)/den;b2=(dx*py-dy*px)/den;b0=1-b1-b2
                    block=covered[low[1]-lo[1]:high[1]-lo[1],low[0]-lo[0]:high[0]-lo[0]]
                    m=(b0>=-1e-8)&(b1>=-1e-8)&(b2>=-1e-8)&~block
                    if not np.any(m):continue
                    block[m]=True
                    weights=np.stack([b0[m],b1[m],b2[m]],axis=1)/v[:,2]
                    weights/=weights.sum(1)[:,None]
                    attr=weights@v[:,3:]
                    uv=attr[:,:2]*size-.5
                    u=np.clip(uv[:,0],0,size-1);w=np.clip(uv[:,1],0,size-1)
                    ix=np.floor(u).astype(int);iy=np.floor(w).astype(int)
                    fx=(u-ix)[:,None];fy=(w-iy)[:,None]
                    jx=np.minimum(ix+1,size-1);jy=np.minimum(iy+1,size-1)
                    tex=(atlas[iy,ix]*(1-fx)+atlas[iy,jx]*fx)*(1-fy)+(atlas[jy,ix]*(1-fx)+atlas[jy,jx]*fx)*fy
                    colour=tex[:,:3]*attr[:,2:]/255
                    a=tex[:,3:]*alpha
                    canvas[yy[m],xx[m]]=canvas[yy[m],xx[m]]*(1-a)+colour*a
    im=Image.fromarray(np.uint8(np.clip(canvas,0,1)*255))
    d=ImageDraw.Draw(im);d.rectangle((0,0,W,27),fill=(15,23,35))
    d.text((8,7),f"CUMULUS / {scene.upper()} - actual atlas + mesh / synthetic sky, not GTA",fill=(242,246,255))
    im.save(destination,quality=89,optimize=True)
    return im
if __name__=="__main__":
    out=pathlib.Path(sys.argv[2]);out.mkdir(parents=True,exist_ok=True)
    for scene in ("back","front","flight","ground","low","night","zenith","inside","height_low","height_mid","height_high","sunrise","close"):
        render(scene,sys.argv[1],out/f"cumulus_{scene}.jpg")
