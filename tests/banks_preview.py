"""Software diagnostic of actual fixed-function 3D texture slices, no GTA assets."""
import pathlib, subprocess, sys, tempfile, os
import numpy as np
from PIL import Image,ImageDraw
from canopy_preview import clip

W,H=map(int,os.environ.get("VC_PREVIEW_SIZE","960x540").split("x"))

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

def depth_clip(poly,limit,far):
    result=[]
    for a,b in zip(poly,np.roll(poly,-1,axis=0)):
        keep_a=(a[2]>=limit) if far else (a[2]<=limit)
        keep_b=(b[2]>=limit) if far else (b[2]<=limit)
        if keep_a:result.append(a)
        if keep_a!=keep_b:result.append(a+(b-a)*(limit-a[2])/(b[2]-a[2]))
    return result

def draw(canvas,tex,v,indices,layer,weather,fog,depth_split=None,far=False):
    height,width=canvas.shape[:2]
    fragments=passing=0
    for tri in indices:
        poly=clip(v[tri])
        if depth_split is not None and len(poly):poly=depth_clip(poly,depth_split,far)
        for j in range(1,len(poly)-1):
            a=np.array([poly[0],poly[j],poly[j+1]])
            xy=a[:,:2]/a[:,2,None]*(510*width/960)*[1,-1]+[width/2,height/2]
            lo=np.maximum(0,np.floor(xy.min(0))).astype(int)
            hi=np.minimum([width,height],np.ceil(xy.max(0))).astype(int)
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
            elif layer==4:
                rgba=sample2(tex[0],attr[:,:2])
                if weather>0:rgba=rgba*(1-weather)+sample2(tex[1],attr[:,:2])*weather
            else:rgba=sample2(tex[layer],attr[:,:2])
            alpha=(rgba[:,3]*attr[:,3])[:,None]
            fragments+=len(alpha)
            passing+=np.count_nonzero(alpha>0.5/255)
            color=np.ones_like(rgba[:,:3]) if os.environ.get("VC_ALPHA") else rgba[:,:3]*attr[:,4:7]*attr[:,7:8]+fog*(1-attr[:,7:8])
            canvas[y,x,:3]=canvas[y,x,:3]*(1-alpha)+color*alpha
            if canvas.shape[2]==4:canvas[y,x,3:4]=canvas[y,x,3:4]*(1-alpha)+alpha
    return fragments,passing

def proxy_prefix(v,indices):
    # Approximate native clipped far-depth prefix and area gate using the
    # same projection. Near tail is never downsampled.
    focal=510*W/960;wx=W/(2*focal);wy=H/(2*focal)
    area=0;lo=np.array([W,H],float);hi=np.zeros(2)
    def cut(poly,coeff):
        result=[]
        if not len(poly):return result
        for a,b in zip(poly,np.roll(poly,-1,axis=0)):
            d,e=np.dot(a[:3],coeff),np.dot(b[:3],coeff)
            if d>=0:result.append(a)
            if (d>=0)!=(e>=0):result.append(a+(b-a)*d/(d-e))
        return result
    for i,tri in enumerate(indices):
        poly=clip(v[tri])
        for coeff in ([1,0,wx],[-1,0,wx],[0,1,wy],[0,-1,wy]):
            poly=cut(poly,coeff)
        if len(poly)<3:continue
        p=np.array(poly)
        if np.any(p[:,2]<450):break
        xy=p[:,:2]/p[:,2,None]*focal*[1,-1]+[W/2,H/2]
        area+=abs(np.sum(xy[:,0]*np.roll(xy[:,1],-1)-xy[:,1]*np.roll(xy[:,0],-1)))*.5
        lo=np.minimum(lo,xy.min(0));hi=np.maximum(hi,xy.max(0))
    else:i=len(indices)
    lo=np.maximum(0,np.floor(lo)-3).astype(int);hi=np.minimum([W,H],np.ceil(hi)+3).astype(int)
    pixels=np.prod(hi-lo)
    if i<32 or np.any(hi<=lo) or area<1.8*pixels:return 0,lo,hi
    return i,lo,hi

def render(scene,exe):
    with tempfile.TemporaryDirectory() as tmp:
        path=pathlib.Path(tmp)/"cloud"
        raw=subprocess.check_output([exe,scene,str(path)]+(["full-volume"] if os.environ.get("VC_FULL_VOLUME") else ["legacy"] if os.environ.get("VC_LEGACY") else ["no-trim"] if os.environ.get("VC_NO_TRIM") else []),text=True)
        tex=[np.fromfile(str(path)+suffix,dtype=np.uint8).reshape(shape).astype(np.float32)/255
             for suffix,shape in [(".canopy",(512,512,4)),(".extra",(512,512,4)),(".clear3d",(32,128,128,4)),(".wet3d",(32,128,128,4))]]
        if pathlib.Path(str(path)+".atlas0").exists():
            for suffix in [".atlas0",".atlas1"]:
                data=np.fromfile(str(path)+suffix,dtype=np.uint8)
                side=int(np.sqrt(data.size/4))
                tex.append(data.reshape(side,side,4).astype(np.float32)/255)
    rows=iter(raw.splitlines())
    p=np.array(next(rows).split(),float);t=np.linspace(0,1,H)[:,None,None]
    canvas=np.broadcast_to((p[:3]*(1-t)+p[3:6]*t)/255,(H,W,3)).copy()
    if os.environ.get("VC_ALPHA"):canvas.fill(0)
    for line in rows:
        cols=line.split(); layer,nv,nt=map(int,cols[:3])
        weather=float(cols[3]) if layer in (2,3,4) else 0
        v=np.array([next(rows).split() for _ in range(nv)],float)
        indices=np.array([next(rows).split() for _ in range(nt)],int)
        if nt:
            fog=(.8*p[3:6]+.2*p[:3])/255
            prefix,lo,hi=proxy_prefix(v,indices) if layer==3 and os.environ.get("VC_HALF_RES") else (0,None,None)
            if prefix:
                reduced=np.zeros(((H+1)//2,(W+1)//2,4),dtype=np.float32)
                far_frag,far_pass=draw(reduced,tex,v,indices[:prefix],layer,weather,fog)
                up=np.stack([np.array(Image.fromarray(reduced[:,:,i]).resize((W,H),Image.Resampling.BILINEAR)) for i in range(4)],axis=2)
                box=np.s_[lo[1]:hi[1],lo[0]:hi[0]]
                canvas[box]=canvas[box]*(1-up[box][:,:,3:])+up[box][:,:,:3]
                near_frag,near_pass=draw(canvas,tex,v,indices[prefix:],layer,weather,fog)
                composite=int(np.prod(hi-lo))
                fragments=far_frag+near_frag+composite;passing=far_pass+near_pass
                print(f"{scene} proxy half {far_frag}, near tail {near_frag}, bounded composite {composite}",flush=True)
            elif layer==2 and os.environ.get("VC_HALF_RES"):
                reduced=np.zeros(((H+1)//2,(W+1)//2,4),dtype=np.float32)
                far_frag,far_pass=draw(reduced,tex,v,indices,layer,weather,fog,450,True)
                up=np.stack([np.array(Image.fromarray(reduced[:,:,i]).resize((W,H),Image.Resampling.BILINEAR)) for i in range(4)],axis=2)
                canvas=canvas*(1-up[:,:,3:])+up[:,:,:3]
                near_frag,near_pass=draw(canvas,tex,v,indices,layer,weather,fog,450,False)
                fragments=far_frag+near_frag+W*H
                passing=far_pass+near_pass
                print(f"{scene} far half {far_frag}, near full {near_frag}, composite {W*H}",flush=True)
            else:fragments,passing=draw(canvas,tex,v,indices,layer,weather,fog)
            print(f"{scene} layer {layer}: {nt} tris; {fragments} rasterized fragments; {passing} nonzero-alpha fragments",flush=True)
    im=Image.fromarray(np.uint8(np.clip(canvas,0,1)*255))
    d=ImageDraw.Draw(im);d.rectangle((0,0,W,24),fill=(15,23,35))
    d.text((8,6),f"3D DENSITY / {scene.upper()} - actual density and slice geometry / synthetic sky, not GTA",fill=(242,246,255))
    return im

if __name__=="__main__":
    out=pathlib.Path(sys.argv[2]);out.mkdir(parents=True,exist_ok=True)
    for scene in sys.argv[3:] or ["canopy","ground","flight","above","rainflight","night","entry","zenith"]:
        render(scene,sys.argv[1]).save(out/f"banks_{scene}.png")
        print("Rendered",scene,flush=True)
