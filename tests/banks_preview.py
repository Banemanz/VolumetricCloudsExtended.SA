"""Software diagnostic of actual fixed-function 3D texture slices, no GTA assets."""
import pathlib, subprocess, sys, tempfile
import numpy as np
from PIL import Image,ImageDraw
from canopy_preview import clip

W,H=960,540

def sample2(tex,uv):
    h,w=tex.shape[:2];uv=uv*np.array([w,h])-.5
    i=np.floor(uv).astype(int);fx,fy=(uv-i).T
    x,y=i.T;fx=fx[:,None];fy=fy[:,None]
    return (tex[y%h,x%w]*(1-fx)+tex[y%h,(x+1)%w]*fx)*(1-fy)+(tex[(y+1)%h,x%w]*(1-fx)+tex[(y+1)%h,(x+1)%w]*fx)*fy

def sample3(tex,uv):
    d,h,w=tex.shape[:3];u=uv*np.array([w,h,d])-.5
    i=np.floor(u).astype(int);fx,fy,fz=(u-i).T; x,y,z=i.T
    fx=fx[:,None];fy=fy[:,None];fz=fz[:,None]
    def plane(k):
        k=np.clip(k,0,d-1)
        return (tex[k,y%h,x%w]*(1-fx)+tex[k,y%h,(x+1)%w]*fx)*(1-fy)+(tex[k,(y+1)%h,x%w]*(1-fx)+tex[k,(y+1)%h,(x+1)%w]*fx)*fy
    return plane(z)*(1-fz)+plane(z+1)*fz

def draw(canvas,tex,v,indices,layer,weather,fog):
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
            m=(b0>=0)&(b1>=0)&(b2>=0)
            if not np.any(m):continue
            weights=np.stack([b0[m],b1[m],b2[m]],axis=1)/a[:,2]
            weights/=weights.sum(1)[:,None]; attr=weights@a[:,3:]
            y,x=yy[m],xx[m]
            if layer==2:
                rgba=sample3(tex[2],attr[:,:3])
                if weather>0:rgba=rgba*(1-weather)+sample3(tex[3],attr[:,:3])*weather
            elif layer==3:
                rgba=sample2(tex[4],attr[:,:2])
                if weather>0:rgba=rgba*(1-weather)+sample2(tex[5],attr[:,:2])*weather
            else:rgba=sample2(tex[layer],attr[:,:2])
            alpha=(rgba[:,3]*attr[:,3])[:,None]
            color=rgba[:,:3]*attr[:,4:7]*attr[:,7:8]+fog*(1-attr[:,7:8])
            canvas[y,x]=canvas[y,x]*(1-alpha)+color*alpha

def render(scene,exe):
    with tempfile.TemporaryDirectory() as tmp:
        path=pathlib.Path(tmp)/"cloud"
        raw=subprocess.check_output([exe,scene,str(path)],text=True)
        tex=[np.fromfile(str(path)+suffix,dtype=np.uint8).reshape(shape).astype(np.float32)/255
             for suffix,shape in [(".canopy",(512,512,4)),(".extra",(512,512,4)),(".clear3d",(32,128,128,4)),(".wet3d",(32,128,128,4))]]
        if pathlib.Path(str(path)+".atlas0").exists():
            tex+= [np.fromfile(str(path)+suffix,dtype=np.uint8).reshape(512,512,4).astype(np.float32)/255 for suffix in [".atlas0",".atlas1"]]
    rows=iter(raw.splitlines())
    p=np.array(next(rows).split(),float);t=np.linspace(0,1,H)[:,None,None]
    canvas=np.broadcast_to((p[:3]*(1-t)+p[3:6]*t)/255,(H,W,3)).copy()
    for line in rows:
        cols=line.split(); layer,nv,nt=map(int,cols[:3])
        weather=float(cols[3]) if layer in (2,3) else 0
        v=np.array([next(rows).split() for _ in range(nv)],float)
        indices=np.array([next(rows).split() for _ in range(nt)],int)
        if nt:draw(canvas,tex,v,indices,layer,weather,(.8*p[3:6]+.2*p[:3])/255)
    im=Image.fromarray(np.uint8(np.clip(canvas,0,1)*255))
    d=ImageDraw.Draw(im);d.rectangle((0,0,W,24),fill=(15,23,35))
    d.text((8,6),f"3D DENSITY / {scene.upper()} - actual density and slice geometry / synthetic sky, not GTA",fill=(242,246,255))
    return im

if __name__=="__main__":
    out=pathlib.Path(sys.argv[2]);out.mkdir(parents=True,exist_ok=True)
    for scene in sys.argv[3:] or ["canopy","ground","flight","above","rainflight","night","entry","zenith"]:
        render(scene,sys.argv[1]).save(out/f"banks_{scene}.jpg",quality=87,optimize=True)
        print("Rendered",scene,flush=True)
