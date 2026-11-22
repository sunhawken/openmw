"""Read MDL/VVD/VTX cap geometry; used by import_dark_messiah_caps.py."""
import sys,json,os
from pathlib import Path
import numpy as np
sys.path.insert(0,str(Path(os.environ['BRUTAL_SOURCEIO']).resolve().parent))
from SourceIO.library.models.mdl.structs.header import MdlHeaderV44
from SourceIO.library.models.mdl.structs.bodygroup import BodyPart
from SourceIO.library.models.vvd import Vvd
from SourceIO.library.models.vtx.v7.vtx import Vtx
from SourceIO.library.utils import MemoryBuffer
BASE=Path(os.environ['BRUTAL_DM_MODEL_DIR'])
OUT=Path(os.environ['BRUTAL_DM_OUTPUT'])
OUT.mkdir(parents=True,exist_ok=True)
BASELINE=Path(os.environ['BRUTAL_DM_MODEL_DIR'])

def load(name):
 b=MemoryBuffer((BASE/(name+'.mdl')).read_bytes());h=MdlHeaderV44.from_buffer(b);b.seek(h.body_part_offset);parts=[BodyPart.from_buffer(b,h.version) for _ in range(h.body_part_count)]
 assetdir=BASE if (BASE/(name+'.vvd')).exists() else BASELINE
 vv=Vvd.from_buffer(MemoryBuffer((assetdir/(name+'.vvd')).read_bytes())).lod_data[0]
 vt=Vtx.from_buffer(MemoryBuffer((assetdir/(name+'.dx90.vtx')).read_bytes()))
 tris=[]
 for bp,vbp in zip(parts,vt.body_parts):
  for m,vm in zip(bp.models,vbp.models):
   for mesh,vmesh in zip(m.meshes,vm.model_lods[0].meshes):
    for sg in vmesh.strip_groups:
     assert all(int(s.flags)==1 for s in sg.strips)
     idx=sg.vertexes['original_mesh_vertex_index'].reshape(-1)[sg.indices]+mesh.vertex_index_start+m.vertex_offset
     tris.extend(idx.reshape(-1,3))
 tris=np.asarray(tris);p=vv['vertex'][tris].astype(float);uv=vv['uv'][tris].astype(float);norm=vv['normal'][tris].astype(float)
 cross=np.cross(p[:,1]-p[:,0],p[:,2]-p[:,0]);area=np.linalg.norm(cross,axis=1);normface=cross/np.maximum(area[:,None],1e-10)
 return dict(indices=tris,p=p,uv=uv,norm=norm,face=normface,area=area*.5)
