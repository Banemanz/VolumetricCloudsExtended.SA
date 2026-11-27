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

def draw_mesh(canvas,tex,v,indices,wrap,colour):
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
            canvas[y,x]=old[y,x]*(1-alpha)+rgba[:,:3]*colour*alpha

def render(scene,exe):
    with tempfile.TemporaryDirectory() as tmp:
        path=pathlib.Path(tmp)/"cloud"
        raw=subprocess.check_output([exe,scene,str(path)],text=True)
        tex=[np.fromfile(str(path)+suffix,dtype=np.uint8).reshape(h,w,4).astype(np.float32)/255
             for suffix,h,w in [(".canopy",512,512),(".extra",512,512),(".volume",1024,1024)]]
    rows=iter(raw.splitlines())
    p=np.array(next(rows).split(),float);t=np.linspace(0,1,H)[:,None,None]
    canvas=np.broadcast_to((p[:3]*(1-t)+p[3:6]*t)/255,(H,W,3)).copy()
    # An original synthetic ridge gives approaching/inside clouds a fixed
    # visibility reference. It is not a map or asset from either GTA game.
    if scene in ("approach","entry","inside"):
        xs=np.arange(W)/W
        profile=((.76+.025*np.sin(xs*12)+.018*np.sin(xs*37)) * H).astype(int)
        for x,y in enumerate(profile):
            canvas[y:,x]=np.array([.23,.37,.37])*.8+canvas[y:,x]*.25
    max_haze=0.
    for line in rows:
        cols=line.split()
        if cols[0]=="H":
            colour=np.array(cols[1:4],float)/255
            a=np.array(cols[4:],float).reshape(3,3)
            max_haze=float(a.max())
            # Exact planar interpolation over the same eight screen triangles.
            for j in range(2):
                for i in range(2):
                    y0,y1=int(H*j/2),int(H*(j+1)/2)
                    x0,x1=int(W*i/2),int(W*(i+1)/2)
                    fy=(np.arange(y0,y1)+.5-y0)/(y1-y0)
                    fx=(np.arange(x0,x1)+.5-x0)/(x1-x0)
                    xx,yy=np.meshgrid(fx,fy)
                    alpha=np.where(xx+yy<=1,a[j,i]*(1-xx-yy)+a[j,i+1]*xx+a[j+1,i]*yy,
                        a[j,i+1]*(1-yy)+a[j+1,i]*(1-xx)+a[j+1,i+1]*(xx+yy-1))
                    alpha=alpha[:,:,None]
                    canvas[y0:y1,x0:x1]=canvas[y0:y1,x0:x1]*(1-alpha)+colour*alpha
            continue
        layer,nv,nt=map(int,cols[:3])
        colour=np.array(cols[3:6],float)/255
        v=np.array([next(rows).split() for _ in range(nv)],float)
        indices=np.array([next(rows).split() for _ in range(nt)],int)
        draw_mesh(canvas,tex[layer],v,indices,layer<2,colour)
    im=Image.fromarray(np.uint8(np.clip(canvas,0,1)*255))
    d=ImageDraw.Draw(im);d.rectangle((0,0,W,24),fill=(15,23,35))
    label=f"LIVE WEATHER / {scene.upper()} - actual C++ textures + mesh / synthetic sky, not GTA"
    if scene in ("approach","entry","inside"):
        label+=f"  |  near haze {max_haze:.2f}"
    d.text((8,6),label,fill=(242,246,255))
    return im

if __name__=="__main__":
    out=pathlib.Path(sys.argv[2]);out.mkdir(parents=True,exist_ok=True)
    scenes=("clear","cloudy","rain","cleared","fog","sandstorm","extra","night","lightning","flight","approach","entry","inside")
    images={}
    for scene in scenes:
        images[scene]=render(scene,sys.argv[1])
        print("Rendered",scene,flush=True)
    for scene in ("clear","rain","flight","inside"):
        images[scene].save(out/f"weather_{scene}.jpg",quality=88,optimize=True)
    def montage(names,cols,filename):
        rows=(len(names)+cols-1)//cols
        sheet=Image.new("RGB",(480*cols,270*rows))
        for i,n in enumerate(names):
            sheet.paste(images[n].resize((480,270)),(i%cols*480,i//cols*270))
        sheet.save(out/filename,quality=90,optimize=True)
    montage(("clear","cloudy","rain","cleared","fog","sandstorm"),2,"weather_transition.jpg")
    montage(("extra","night","lightning"),1,"weather_light.jpg")
    montage(("approach","entry","inside"),1,"weather_interior.jpg")
