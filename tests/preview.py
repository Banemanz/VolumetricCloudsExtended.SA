"""Render the portable geometry with a synthetic cloud mask, not GTA textures.
Requires Python 3, Pillow, numpy and an executable built from dump_preview.cpp.
Usage: python tests/preview.py ./dump_preview docs/previews
"""
import csv
import io
import pathlib
import subprocess
import sys
import numpy as np
from PIL import Image, ImageDraw

WIDTH, HEIGHT = 960, 540
N = 128
rng = np.random.default_rng(123)
# A soft, irregular procedural mask for diagnostics only.
x, y = np.meshgrid(np.linspace(-1, 1, N), np.linspace(-1, 1, N))
noise = np.zeros((N, N))
for n, strength in [(5,.5), (11,.26), (24,.15), (48,.09)]:
    image = Image.fromarray(np.uint8(rng.random((n, n))*255))
    noise += np.asarray(image.resize((N,N), Image.Resampling.BICUBIC))/255*strength
radial = np.sqrt(x*x+y*y)
mask = np.clip((1-radial*1.05)+(noise-.5)*.45,0,1)
mask = np.uint8(np.clip(mask*2.7,0,1)**1.5*255)

def render(scene, destination, executable):
    raw = subprocess.check_output([executable, scene], text=True)
    storm = scene=="rain"
    top = np.array([61,80,99] if storm else [59,127,197])
    bottom = np.array([145,155,165] if storm else [192,218,236])
    y = np.linspace(0,1,HEIGHT)[:,None,None]
    sky = np.broadcast_to(top*(1-y)+bottom*y, (HEIGHT, WIDTH,3)).copy()
    canvas = Image.fromarray(sky.astype(np.uint8)).convert("RGBA")
    base = np.array([190,199,205] if storm else [255,250,243])
    for row in csv.reader(io.StringIO(raw)):
        *coords, opacity, shade, haze = map(float,row)
        p=np.array(coords).reshape(4,2)
        lo=np.maximum(0,np.floor(p.min(axis=0))).astype(int)
        hi=np.minimum([WIDTH,HEIGHT],np.ceil(p.max(axis=0))).astype(int)
        w,h=hi-lo
        if w<=0 or h<=0: continue
        p=p-lo
        # Perspective mapping from output quad into the mask.
        dst=np.array([[0,0],[N-1,0],[0,N-1],[N-1,N-1]])
        mat=[]; result=[]
        for (x,y),(u,v) in zip(p,dst):
            mat.extend([[x,y,1,0,0,0,-u*x,-u*y],[0,0,0,x,y,1,-v*x,-v*y]])
            result.extend([u,v])
        try: coeff=np.linalg.solve(np.array(mat),np.array(result))
        except np.linalg.LinAlgError: continue
        rgba=np.zeros((N,N,4),dtype=np.uint8)
        lit=base*(.75+.25*shade)
        colour=lit*(1-.35*haze)+bottom*.35*haze
        rgba[:,:,:3]=(colour[None,None,:]*np.linspace(1,.75,N)[:,None,None]).astype(np.uint8)
        rgba[:,:,3]=(mask*opacity).astype(np.uint8)
        card=Image.fromarray(rgba).transform((int(w),int(h)),Image.Transform.PERSPECTIVE,tuple(coeff),Image.Resampling.BILINEAR)
        canvas.alpha_composite(card,tuple(lo))
    d=ImageDraw.Draw(canvas)
    d.rectangle((0,0,WIDTH,31),fill=(22,28,40,255))
    d.text((10,9),f"{scene.upper()} | CORE GEOMETRY + SYNTHETIC TEXTURE | NOT AN IN-GAME SCREENSHOT",fill=(240,240,240))
    canvas.convert("RGB").save(destination,quality=88,optimize=True)

if __name__=="__main__":
    dest=pathlib.Path(sys.argv[2]); dest.mkdir(parents=True,exist_ok=True)
    for scene in ("clear","rain","flight","crossed"):
        render(scene,dest/(scene+".jpg"),sys.argv[1])
