#!/usr/bin/env python3
"""Bake the neck-cut cap faces and their radial RGB texture.

Reads the user's Dark Messiah assets; run via import_dark_messiah_caps.py.
World +X points right and +Y up in the output. Colours are unlit samples of the
source atlas.
"""
import json,hashlib,os,sys
from pathlib import Path
import numpy as np
from PIL import Image,ImageDraw
from dark_messiah_cap_geometry import load,BASE,OUT,BASELINE
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from vtf_decode import load_vtf_texture

SIZE=512
atlas_path=Path(os.environ.get('BRUTAL_DM_ATLAS',str(BASE.parents[2]/'materials/models/npc_necroguard/necroguard_full.vtf')))
raw,w,h,isfloat=load_vtf_texture(atlas_path.read_bytes())
atlas=np.frombuffer(raw,dtype=np.float32 if isfloat else np.uint8).reshape(h,w,4)
if isfloat:atlas=np.rint(np.clip(atlas,0,1)*255).astype(np.uint8)
Image.fromarray(atlas).save(OUT/'necroguard_full.png')

def keys(p):
 return [tuple(sorted(tuple(x) for x in np.rint(t*1000).astype(int))) for t in p]

def sample(uv):
 # Direct Source UV v-down; decoder returns top-first. Half-texel center convention.
 xy=uv*np.array([w,h])-.5;lo=np.floor(xy).astype(int);f=xy-lo
 x0=np.clip(lo[...,0],0,w-1);x1=np.clip(lo[...,0]+1,0,w-1)
 y0=np.clip(lo[...,1],0,h-1);y1=np.clip(lo[...,1]+1,0,h-1)
 fx=f[...,0,None];fy=f[...,1,None]
 return ((1-fy)*((1-fx)*atlas[y0,x0,:3]+fx*atlas[y0,x1,:3])+fy*((1-fx)*atlas[y1,x0,:3]+fx*atlas[y1,x1,:3]))

def bake(d,ids,name,side):
 p=d['p'][ids];uv=d['uv'][ids];mn=p[:,:,:2].min(axis=(0,1));mx=p[:,:,:2].max(axis=(0,1))
 center=(mn+mx)*.5;extent=(mx-mn)*1.04
 image=np.zeros((SIZE,SIZE,4),np.uint8);zbuffer=np.full((SIZE,SIZE),-np.inf if side=='body' else np.inf)
 coverage=np.zeros((SIZE,SIZE),np.uint8)
 for tri,tuv in zip(p,uv):
  pixel=(tri[:,:2]-center)/extent*SIZE+SIZE*.5
  pixel[:,1]=SIZE-pixel[:,1]
  low=np.maximum(np.floor(pixel.min(axis=0)-1).astype(int),0);high=np.minimum(np.ceil(pixel.max(axis=0)+1).astype(int),SIZE-1)
  if np.any(high<low):continue
  xx,yy=np.meshgrid(np.arange(low[0],high[0]+1)+.5,np.arange(low[1],high[1]+1)+.5)
  a,b,c=pixel;den=(b[1]-c[1])*(a[0]-c[0])+(c[0]-b[0])*(a[1]-c[1])
  if abs(den)<1e-10:continue
  wa=((b[1]-c[1])*(xx-c[0])+(c[0]-b[0])*(yy-c[1]))/den
  wb=((c[1]-a[1])*(xx-c[0])+(a[0]-c[0])*(yy-c[1]))/den;wc=1-wa-wb
  inside=(wa>=-1e-8)&(wb>=-1e-8)&(wc>=-1e-8)
  z=wa*tri[0,2]+wb*tri[1,2]+wc*tri[2,2]
  sl=np.s_[low[1]:high[1]+1,low[0]:high[0]+1]
  front=(z>=zbuffer[sl]) if side=='body' else (z<=zbuffer[sl]);use=inside&front
  texuv=wa[...,None]*tuv[0]+wb[...,None]*tuv[1]+wc[...,None]*tuv[2]
  colors=np.rint(sample(texuv)).astype(np.uint8)
  image[sl][use,:3]=colors[use];image[sl][use,3]=255;zbuffer[sl][use]=z[use];coverage[sl]+=inside
 # RGB padding around silhouette only; alpha stays original coverage.
 # This avoids dark bilinear borders without painting or inventing interior colors.
 known=image[:,:,3]>0
 for _ in range(8):
  sums=np.zeros((SIZE,SIZE,3),np.float32);counts=np.zeros((SIZE,SIZE),np.float32)
  for dy,dx in [(1,0),(-1,0),(0,1),(0,-1)]:
   shifted=np.roll(known,(dy,dx),(0,1));colors=np.roll(image[:,:,:3],(dy,dx),(0,1))
   if dy==1:shifted[0]=False
   if dy==-1:shifted[-1]=False
   if dx==1:shifted[:,0]=False
   if dx==-1:shifted[:,-1]=False
   sums+=colors*shifted[:,:,None];counts+=shifted
  new=(~known)&(counts>0);image[new,:3]=np.rint(sums[new]/counts[new,None]).astype(np.uint8);known|=new
 Image.fromarray(image).save(OUT/(name+'.png'))
 return dict(file=name+'.png',triangle_count=len(ids),triangle_ids=[int(i) for i in ids],source_bounds=[p.min(axis=(0,1)).tolist(),p.max(axis=(0,1)).tolist()],uv_bounds=[uv.min(axis=(0,1)).tolist(),uv.max(axis=(0,1)).tolist()],source_texel_bounds=[(uv.min(axis=(0,1))*[w,h]).tolist(),(uv.max(axis=(0,1))*[w,h]).tolist()],projection_center_xy=center.tolist(),projection_extent_xy=extent.tolist(),projection='Source +X right,+Y up; orthographic XY; body takes greatest Z, head takes smallest Z',covered_pixels=int((image[:,:,3]>0).sum()),overlap_pixels=int((coverage>1).sum()))

def bake_radial_opaque(d, ids, name, side):
 """Reparameterize the source flesh disk without flattening its texture to a crop.

 1. Build exact positional vertex IDs from the chosen source triangles. Interior
    edges occur twice; edges occurring once form the authored outer flesh ring.
 2. Walk that ring, orient it counterclockwise in Source XY, and use its polygon
    area centroid as center. Assert the center lies in the ring's visibility
    kernel, so each ray stays inside the source disk.
 3. At output pixel center (u,v), let q=(2*u-1,1-2*v), r=min(length(q),1),
    direction=q/length(q). Intersect that ray with the actual source boundary,
    giving distance R(direction). Sample source position center+r*R*direction.
    Thus the disk fills the complete authored flesh polygon. Outside-disk square
    corners clamp to its boundary along their ray; no padding color is invented.
 4. Locate the original projected source triangle; barycentrically interpolate
    its original UV, then bilinearly sample the original atlas. Ignore VTF alpha
    (it may be a material mask); save PNG RGB, inherently fully opaque.

 No intermediate PNG or embedded game geometry is an input to this operation.
 Pixel rows descend while Source +Y points up. Both cap sides use the same XY
 orientation; the engine's cap-frame UV basis decides the displayed handedness.
 """
 from collections import Counter, defaultdict
 p=d['p'][ids];uv=d['uv'][ids]
 vertex_ids={};vertices=[];edges=Counter()
 for tri in p:
  triangle_ids=[]
  for pos in tri:
   key=tuple(pos)
   if key not in vertex_ids:
    vertex_ids[key]=len(vertices);vertices.append(pos.copy())
   triangle_ids.append(vertex_ids[key])
  for i in range(3):
   edges[tuple(sorted((triangle_ids[i],triangle_ids[(i+1)%3])))]+=1
 assert max(edges.values())==2, 'Nonmanifold source flesh patch'
 adjacency=defaultdict(list)
 for (a,b),count in edges.items():
  if count==1:adjacency[a].append(b);adjacency[b].append(a)
 assert adjacency and all(len(n)==2 for n in adjacency.values()), 'Boundary is not a closed loop'
 first=min(adjacency,key=lambda i:tuple(vertices[i]))
 ring=[];previous=None;current=first
 while not ring or current!=first:
  assert current not in ring, 'Multiple/intersecting boundary loops'
  ring.append(current)
  candidates=[n for n in adjacency[current] if n!=previous]
  nxt=min(candidates,key=lambda i:tuple(vertices[i]))
  previous,current=current,nxt
 assert len(ring)==len(adjacency), 'Disconnected flesh boundary'
 polygon=np.asarray(vertices)[ring,:2]
 cross=lambda a,b:a[...,0]*b[...,1]-a[...,1]*b[...,0]
 area2=cross(polygon,np.roll(polygon,-1,axis=0)).sum()
 if area2<0:
  polygon=polygon[::-1];area2=-area2
 next_vertex=np.roll(polygon,-1,axis=0)
 weights=cross(polygon,next_vertex)
 center=((polygon+next_vertex)*weights[:,None]).sum(axis=0)/(3*area2)
 kernel_margins=cross(next_vertex-polygon,center-polygon)
 assert np.all(kernel_margins>1e-8), 'Source boundary is not radial about chosen center'
 xx,yy=np.meshgrid((np.arange(SIZE)+.5)/SIZE*2-1,1-(np.arange(SIZE)+.5)/SIZE*2)
 directions=np.stack((xx,yy),axis=-1)
 original_radius=np.linalg.norm(directions,axis=-1)
 directions/=np.maximum(original_radius[...,None],1e-30)
 directions[original_radius==0]=[1,0]
 boundary_radius=np.full((SIZE,SIZE),np.inf)
 for start,end in zip(polygon,next_vertex):
  edge=end-start;offset=start-center;den=cross(directions,edge)
  valid=np.abs(den)>1e-12
  distance=np.divide(cross(offset,edge),den,out=np.full_like(den,np.inf),where=valid)
  fraction=np.divide(cross(offset,directions),den,out=np.full_like(den,np.inf),where=valid)
  valid&=(distance>0)&(fraction>=-1e-10)&(fraction<=1+1e-10)
  boundary_radius=np.minimum(boundary_radius,np.where(valid,distance,np.inf))
 assert np.all(np.isfinite(boundary_radius)), 'A radial sample missed the source boundary'
 points=center+directions*(np.minimum(original_radius,1)*boundary_radius)[...,None]
 source_uv=np.empty((SIZE,SIZE,2));assigned=np.zeros((SIZE,SIZE),bool)
 mapped_ids=np.full((SIZE,SIZE),-1,dtype=np.int32)
 for tid,tri,tuv in zip(ids,p,uv):
  a,b,c=tri[:,:2];den=cross(b-a,c-a)
  assert abs(den)>1e-10, 'Degenerate projected source triangle'
  wb=cross(points-a,c-a)/den;wc=cross(b-a,points-a)/den;wa=1-wb-wc
  inside=(wa>=-1e-9)&(wb>=-1e-9)&(wc>=-1e-9)
  choose=inside&~assigned
  bary=np.stack((wa,wb,wc),axis=-1)[choose]
  bary=np.maximum(bary,0);bary/=bary.sum(axis=-1,keepdims=True)
  source_uv[choose]=bary@tuv
  mapped_ids[choose]=tid;assigned|=inside
 assert assigned.all(), f'{int((~assigned).sum())} pixels have no authored source triangle'
 rgb=np.clip(np.rint(sample(source_uv)),0,255).astype(np.uint8)
 Image.fromarray(rgb,'RGB').save(OUT/(name+'.png'))
 return {'file':name+'.png','method':'radial_opaque','size':[SIZE,SIZE],
         'triangle_ids':[int(i) for i in ids],'source_triangle_count':len(ids),
         'boundary_xy_ccw':polygon.tolist(),'center_xy':center.tolist(),
         'boundary_area_xy':float(area2*.5),'minimum_kernel_margin':float(kernel_margins.min()),
         'pixel_rule':'q=(2*(x+.5)/width-1,1-2*(y+.5)/height); radius=min(length(q),1); source=center+radius*boundaryDistance(q/length(q))*q/length(q)',
         'atlas_filter':'bilinear, sourceUV*atlasSize-0.5, no color/lighting modifications',
         'channels':'RGB, implicit alpha=1, original VTF alpha ignored',
         'outside_disk':'same ray; radius clamped to1 at original flesh boundary',
         'missing_source_pixels':int((~assigned).sum()),
         'sampled_triangle_count':len(np.unique(mapped_ids)),
         'source_uv_bounds':[source_uv.min(axis=(0,1)).tolist(),source_uv.max(axis=(0,1)).tolist()],
         'corner_rgb':[rgb[0,0].tolist(),rgb[0,-1].tolist(),rgb[-1,0].tolist(),rgb[-1,-1].tolist()],
         'sha256':hashlib.sha256((OUT/(name+'.png')).read_bytes()).hexdigest()}

base=load('npc_necroguard');basekeys=set(keys(base['p']));manifest={'atlas':str(atlas_path),'atlas_sha256':hashlib.sha256(atlas_path.read_bytes()).hexdigest(),'atlas_size':[w,h],'sourceio_commit':'6deac3176bf0e8dd2a50038cfa1066abfa58c1f9','method':'LOD0 triangles absent from intact model by sorted bind-position triplets quantized to0.001 Source unit; UV groups validated against original texture. Barycentric color sampling; no lighting, tint, generative tools, or painted pixels. Alpha is projected triangle coverage, unrelated to VTF material/specular alpha.','baseline_triangles':len(base['p']),'outputs':[],'models':{}}
matched=[];preview=[]
for suffix,side in [('nohead','body'),('nohead_link','head')]:
 name='npc_necroguard_'+suffix;d=load(name);kk=keys(d['p']);new=np.array([i for i,k in enumerate(kk) if k not in basekeys]);matched.extend(k for k in kk if k in basekeys)
 uv=d['uv'][new];centeruv=uv.mean(axis=1)
 central=new[(centeruv[:,0]<.805)&(centeruv[:,1]<.8)]
 flesh=new[centeruv[:,1]<.8]
 assert len(new)==60 and len(central)==10 and len(flesh)==30
 manifest['models'][name]={'triangles':len(kk),'matched_intact_triangles':len(kk)-len(new),'new_cap_triangles':len(new),'new_ids':new.tolist(),'mean_vertex_normal':d['norm'][new].mean(axis=(0,1)).tolist()}
 for label,ids in [('full_cap',new),('flesh_cap',flesh),('central_gore',central)]:
  info=bake(d,ids,side+'_'+label,side);manifest['outputs'].append(info)
  preview.append((side+' '+label,Image.open(OUT/info['file']).copy()))
 manifest.setdefault('opaque_radial_outputs',[]).append(bake_radial_opaque(d,flesh,side+'_flesh_opaque',side))
 # Source cap triangle vertices, UVs and normals, for reference.
 np.savez(OUT/(side+'_authored_cap.npz'),positions=d['p'][new],uvs=d['uv'][new],normals=d['norm'][new],source_triangle_ids=new)
 ov=Image.fromarray(atlas.copy());draw=ImageDraw.Draw(ov)
 for group,col in [(new,(0,255,255,255)),(flesh,(255,255,0,255)),(central,(255,0,255,255))]:
  for tri in d['uv'][group]:
   xy=[tuple(p) for p in tri*[w,h]];draw.line(xy+[xy[0]],fill=col,width=1)
 ov.crop((1500,530,1790,1024)).resize((580,988)).save(OUT/(side+'_uv_proof.png'))
manifest['intact_partition_proof']={'cut_original_triangles_total':len(matched),'unique':len(set(matched)),'missing':len(basekeys-set(matched)),'overlap':len(matched)-len(set(matched))}
assert len(matched)==len(base['p']) and set(matched)==basekeys and len(set(matched))==len(matched)
board=Image.new('RGB',(3*SIZE,2*(SIZE+30)),(50,50,50));draw=ImageDraw.Draw(board)
for i,(label,img) in enumerate(preview):
 x=i%3*SIZE;y=i//3*(SIZE+30);board.paste(img,(x,y+30),img);draw.text((x+8,y+8),label,fill='white')
board.save(OUT/'cap_contact_sheet.png')
manifest['derivation_inputs']=[]
for model in ['npc_necroguard','npc_necroguard_nohead','npc_necroguard_nohead_link']:
 for suffix in ['.mdl','.vvd','.dx90.vtx']:
  local=BASE/(model+suffix)
  if not local.exists():local=BASELINE/(model+suffix)
  manifest['derivation_inputs'].append({'vpk_path':'mm/models/npc/necroguard/'+model+suffix,'local':str(local),'sha256':hashlib.sha256(local.read_bytes()).hexdigest()})
manifest['derivation_inputs'].append({'vpk_path':'mm/materials/models/npc_necroguard/necroguard_full.vtf','local':str(atlas_path),'sha256':hashlib.sha256(atlas_path.read_bytes()).hexdigest()})
(OUT/'cap-proof.json').write_text(json.dumps(manifest,indent=2)+'\n');print(json.dumps(manifest['intact_partition_proof']));print('Wrote6 planar bakes,2 opaque radial bakes and proof.')
