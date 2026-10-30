import numpy as np
from PIL import Image, ImageFilter
W,H=320,480
rng=np.random.default_rng(7)
y=np.linspace(0,1,H)[:,None]; x=np.linspace(0,1,W)[None,:]
top=np.array([10,22,48]); mid=np.array([6,14,32]); bot=np.array([3,6,14])
t=np.clip(y,0,1)
sky=np.where(t<0.5,(top*(1-t*2)+mid*(t*2)),(mid*(1-(t-0.5)*2)+bot*((t-0.5)*2)))
img=np.broadcast_to(sky[:,None,:] if sky.ndim==2 else sky,(H,W,3)).astype(float).copy()
def band(cy,amp,freq,phase,width,color,strength,curtain):
    centre=cy+amp*np.sin(2*np.pi*(freq*x+phase))+0.03*np.sin(2*np.pi*(3.1*x+phase*2))
    d=(y-centre)
    prof=np.where(d<0,np.exp(-(d/(width*0.35))**2),np.exp(-(d/width)**1.2*1.0))
    rays=0.55+0.45*np.sin(2*np.pi*(curtain*x+phase*3))**2
    rays*=0.7+0.3*np.sin(2*np.pi*(curtain*2.3*x+phase))**2
    a=np.clip(prof*rays*strength,0,1)[...,None]
    return a*np.array(color)
# lower and softer than the first version so the clock/date stay readable
img+=band(0.52,0.05,0.9,0.15,0.10,[30,200,150],0.55,9)
img+=band(0.62,0.04,1.3,0.55,0.08,[20,170,190],0.42,13)
img+=band(0.44,0.03,0.7,0.80,0.07,[90,80,190],0.25,7)
img+=band(0.70,0.03,1.7,0.35,0.06,[30,150,120],0.25,17)
im=Image.fromarray(np.clip(img,0,255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(3))
a=np.array(im).astype(float)
for _ in range(90):
    sx,sy=rng.integers(0,W),int(rng.random()**1.6*H*0.65)
    b=rng.uniform(90,220); a[sy,sx]=np.maximum(a[sy,sx],[b,b,b*1.05])
fade=np.clip((y-0.55)/0.45,0,1)[...,None]
a*=1-0.55*fade
vx=(x-0.5)**2; vy=(y-0.45)**2
a*=(1-0.35*np.clip((vx+vy)*2.2,0,1))[...,None]
Image.fromarray(np.clip(a,0,255).astype(np.uint8)).save('../images/aurora.png')
print('ok')
