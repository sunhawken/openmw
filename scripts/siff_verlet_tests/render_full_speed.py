"""Continuous, real-time previews of the actual full-resolution skinned meshes."""
from pathlib import Path
import sys, math, json
import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent
from run_tests import Asset, run, Rotation
from verify_final import SETTINGS
def geometry(a,world):
    parts=[]
    for i,s in enumerate(a.meta['shapes']):
        v=a.skin(world,i)
        v=(np.column_stack([v,np.ones(len(v))])@np.linalg.inv(a.d[f'skin{i}'])@world[s['root']]@np.linalg.inv(world[0]))[:,:3]
        parts.append((v,a.d[f'f{i}'],i))
    return parts

OUT = ROOT / 'previews'
LIBRARY = 'final.so'
def preview_font(size):
    for path in ['/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',
                 'C:/Windows/Fonts/arial.ttf',
                 '/System/Library/Fonts/Supplemental/Arial.ttf']:
        if Path(path).exists():return ImageFont.truetype(path,size)
    return ImageFont.load_default()
FONT = preview_font(18)
SMALL = preview_font(13)
TITLE = preview_font(24)
PALETTE = [(77,87,112),(60,75,103),(186,124,126),(188,167,95),
           (199,172,96),(162,150,119),(199,170,85),(99,126,159)]

def body(a, world):
    inv = np.linalg.inv(world[0])
    names = ['Bip01 Pelvis','Bip01 Spine2','Bip01 Neck','Bip01 Head','Bip01 HeadNub',
             'Bip01 L Thigh','Bip01 L Calf','Bip01 L Foot',
             'Bip01 R Thigh','Bip01 R Calf','Bip01 R Foot']
    names += [f'Bip01 {side} {joint}' for side in ['L','R'] for joint in ['UpperArm','Forearm','Hand']]
    return {n: (world[a.names.index(n)] @ inv)[3,:3] for n in names}

def parts_for(assets, worlds):
    return [(v, f, i+4*j) for j,(a,w) in enumerate(zip(assets,worlds))
            for v,f,i in geometry(a,w)]

def root_points(a, world):
    return (world[a.chain] @ np.linalg.inv(world[0]))[:,:,3,:3]

def angles(assets, worlds, deg):
    u = np.array([math.sin(math.radians(deg)),math.cos(math.radians(deg)),0])
    result = []
    for a,w in zip(assets,worlds):
        p = root_points(a,w);d = p[:,3]-p[:,2]
        result.append(float(np.median(np.degrees(np.arctan2(-(d@u),-d[:,2])))))
    return result

def camera(bounds, yaw):
    r = Rotation.from_euler('zx',[math.radians(yaw),math.radians(7)]).as_matrix().T
    b = bounds @ r
    lo,hi = b.min(0),b.max(0)
    scale = min(400/max(10,hi[2]-lo[2]),365/max(10,hi[0]-lo[0]))
    cx = (lo[0]+hi[0])/2
    def project(v):
        q = np.asarray(v) @ r
        return np.column_stack([(q[:,0]-cx)*scale+225,(hi[2]-q[:,2])*scale+65])
    return r,scale,project

def panel(assets, worlds, parts, bounds, yaw, t=0, deg=0):
    im = Image.new('RGB',(450,500),(225,232,239));d = ImageDraw.Draw(im)
    r,scale,project = camera(bounds,yaw)
    b = body(assets[0],worlds[0])
    edges = [('Bip01 Pelvis','Bip01 Spine2',17),('Bip01 Spine2','Bip01 Neck',17),
             ('Bip01 L Thigh','Bip01 L Calf',8),('Bip01 L Calf','Bip01 L Foot',6),
             ('Bip01 R Thigh','Bip01 R Calf',8),('Bip01 R Calf','Bip01 R Foot',6),
             ('Bip01 Pelvis','Bip01 L Thigh',7),('Bip01 Pelvis','Bip01 R Thigh',7)]
    edges += [(f'Bip01 {side} UpperArm',f'Bip01 {side} Forearm',5) for side in ['L','R']]
    edges += [(f'Bip01 {side} Forearm',f'Bip01 {side} Hand',4) for side in ['L','R']]
    for n1,n2,width in edges:
        q=project([b[n1],b[n2]]);d.line([tuple(x) for x in q],fill=(175,188,201),width=max(1,round(width*scale)))
    q=project([b['Bip01 Head'],b['Bip01 HeadNub']]);mid=q.mean(0)
    d.ellipse((mid[0]-9*scale,mid[1]-12*scale,mid[0]+9*scale,mid[1]+12*scale),fill=(175,188,201))
    polys=[];colors=[];depths=[]
    light=np.array([.4,-.6,.7]);light/=np.linalg.norm(light)
    for v,f,i in parts:
        tri=(v@r)[f];norm=np.cross(tri[:,1]-tri[:,0],tri[:,2]-tri[:,0])
        shade=.45+.55*np.abs(norm@light/np.maximum(np.linalg.norm(norm,axis=1),1e-10))
        color=np.clip(np.array(PALETTE[i])[None,:]*shade[:,None]+18,0,255).astype(np.uint8)
        xy=project(v)[f]
        keep=(xy[:,:,0].max(1)>=0)&(xy[:,:,0].min(1)<450)&(xy[:,:,1].max(1)>=0)&(xy[:,:,1].min(1)<500)
        polys.append(xy[keep].astype(np.int32));colors.append(color[keep]);depths.append(tri[keep,:,1].mean(1))
    polys=np.concatenate(polys);colors=np.concatenate(colors);depths=np.concatenate(depths)
    for i in np.argsort(depths)[::-1]:d.polygon([tuple(p) for p in polys[i]],fill=tuple(colors[i]))
    # Mark real attachment/first-free bone segments, not a desired target angle.
    for a,w in zip(assets,worlds):
        p=root_points(a,w)[0,2:4];q=project(p)
        d.line([tuple(x) for x in q],fill=(208,102,38),width=3)
        d.ellipse((q[0,0]-4,q[0,1]-4,q[0,0]+4,q[0,1]+4),fill=(30,126,101))
    d.text((13,10),'Side view' if yaw==90 else 'Three-quarter view',font=SMALL,fill=(49,66,86))
    d.text((13,30),'Gray: skeleton/body reference',font=SMALL,fill=(78,91,105))
    # Moving ground ticks convey speed with a camera that follows the actor.
    u=np.array([math.sin(math.radians(deg)),math.cos(math.radians(deg)),0])
    for x in np.arange(-150,151,20):
        p=u*(x-(320*t)%20);q=project([p+np.array([-2,0,0]),p+np.array([2,0,0])])
        d.line([tuple(v) for v in q],fill=(148,167,182),width=2)
    return im

def frame(assets,worlds,bounds,deg,t):
    im=Image.new('RGB',(940,635),(240,244,248));d=ImageDraw.Draw(im)
    direction='Forward' if deg==0 else 'Backward'
    d.text((22,10),f'Siff · {direction.lower()} running at full test speed',font=TITLE,fill=(31,49,73))
    d.text((22,43),'320 units/s · continuous 1× playback · 60 Hz solver / 20 fps preview',font=FONT,fill=(51,72,92))
    parts=parts_for(assets,worlds)
    for yaw,x in [(35,15),(90,475)]:im.paste(panel(assets,worlds,parts,bounds,yaw,t,deg),(x,77))
    ang=angles(assets,worlds,deg)
    d=ImageDraw.Draw(im)
    d.text((22,579),f'Free-root tilt from downward: hair {ang[0]:.0f}° · skirt {ang[1]:.0f}°  |  green: attachment · orange: first free segment',font=SMALL,fill=(43,61,78))
    d.text((22,606),'Actual full-resolution skinned meshes · procedural gait · untextured simulation, not gameplay',font=SMALL,fill=(78,91,105))
    return im

def main(still=False):
    OUT.mkdir(parents=True,exist_ok=True)
    assets=[Asset(n) for n in SETTINGS];recordings={};cameras={}
    for deg in [0,180]:
        bounds=[]
        recordings[deg]=[]
        for a in assets:
            c=run(a,LIBRARY,'full_run',math.radians(deg),settings=SETTINGS[a.name],record=True)
            recordings[deg].append(c['worlds'])
        # Include rest and the whole cruise in shared camera bounds for both directions.
        for index in list(range(54,175,10)):
            worlds=[c[index] for c in recordings[deg]]
            bounds.extend(v for v,f,i in parts_for(assets,worlds))
            bounds.append(np.array(list(body(assets[0],worlds[0]).values())))
        bounds=np.concatenate(bounds);cameras[deg]=np.array([[x,y,z] for x in [bounds[:,0].min()-5,bounds[:,0].max()+5]
                  for y in [bounds[:,1].min()-5,bounds[:,1].max()+5] for z in [min(0,bounds[:,2].min())-3,bounds[:,2].max()+5]])
    metrics={}
    for deg in [0,180]:
        name='Forward' if deg==0 else 'Backward'
        corners=cameras[deg]
        worlds=[c[90] for c in recordings[deg]]
        frame(assets,worlds,corners,deg,3).save(OUT/f'Siff-Full-Speed-{name}.png')
        metrics[name]={'speed_units_per_second':320,'playback_rate':1,'solver_fps':60,'render_fps':20,
                       'clip_start_seconds':1.8,'clip_duration_seconds':4,
                       'median_root_angles_degrees':dict(zip(['hair','skirt'],angles(assets,worlds,deg)))}
        print(name,'still saved',metrics[name],flush=True)
        if still:continue
        frames=[]
        # Capture every third solver frame: 80 consecutive frames, exactly 4 s.
        for f in range(108,348,3):
            index=f//2;t=f/60
            frames.append(frame(assets,[c[index] for c in recordings[deg]],corners,deg,t))
        frames[0].save(OUT/f'Siff-Full-Speed-{name}.gif',save_all=True,append_images=frames[1:],duration=50,loop=0,optimize=False,disposal=2)
        print(name,'continuous clip saved',len(frames),'frames',flush=True)
    (ROOT/'Full-Speed-Preview-Metrics.json').write_text(json.dumps(metrics,indent=2))

if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser();parser.add_argument('--library',default='final.so');parser.add_argument('--still',action='store_true')
    args=parser.parse_args();LIBRARY=args.library;main(args.still)
