from pathlib import Path
import sys, time, json
time.clock=time.perf_counter
import numpy as np
from pyffi.formats.nif import NifFormat

def mat(obj):
    return np.array(obj.as_list(),dtype=np.float64)

def extract(path, out):
    d=NifFormat.Data()
    with path.open('rb') as f:d.read(f)
    nodes=[]; parents=[]; names=[]; local=[]; lookup={}
    # Explicit actor placement above the NIF skeleton.
    names.append('Placement');parents.append(-1);local.append(np.eye(4))
    def visit(n,parent):
        if not isinstance(n,NifFormat.NiNode):return
        idx=len(names);lookup[id(n)]=idx;nodes.append(n)
        names.append(n.name.decode());parents.append(parent);local.append(mat(n.get_transform()))
        for c in n.children:
            if c:visit(c,idx)
    for root in d.roots:visit(root,0)
    prefix='SiffHair' if 'hair' in path.name else 'SiffSkirt'
    count=11 if 'hair' in path.name else 9
    chain=np.array([[names.index(f'{prefix}{chr(65+c)}_{i+1:02d}') for i in range(count)] for c in range(12)],dtype=np.int32)
    local=np.array(local);parents=np.array(parents,dtype=np.int32)
    world=local.copy()
    for i,p in enumerate(parents):
        if p>=0:world[i]=local[i]@world[p]
    arrays={'local':local,'world':world,'parents':parents,'chain':chain}
    shapes=[]
    for x in d.get_global_iterator():
        if not isinstance(x,NifFormat.NiTriShape):continue
        idx=len(shapes)
        # Retain full skin matrices so mesh checks use true linear blend skinning.
        vertices=np.array([v.as_tuple() for v in x.data.vertices],np.float64)
        faces=np.array(x.data.get_triangles(),np.int32)
        si=x.skin_instance
        bone_indices=np.array([lookup[id(b)] for b in si.bones],np.int32)
        skin=mat(si.data.get_transform())
        binds=np.array([mat(b.get_transform()) for b in si.data.bone_list])
        weights=np.zeros((len(vertices),len(bone_indices)),np.float64)
        for bi,b in enumerate(si.data.bone_list):
            for w in b.vertex_weights:weights[w.index,bi]=w.weight
        arrays.update({f'v{idx}':vertices,f'f{idx}':faces,f'bi{idx}':bone_indices,f'bind{idx}':binds,f'skin{idx}':skin,f'w{idx}':weights})
        # Reference deformation provided by PyFFI for validation of our matrix evaluator.
        ref=np.array([v.as_tuple() for v in x.get_skin_deformation()[0]],np.float64)
        arrays[f'ref{idx}']=ref
        shapes.append({'name':x.name.decode(),'nvertices':len(vertices),'nfaces':len(faces),'root':lookup[id(si.skeleton_root)]})
    np.savez_compressed(out,**arrays)
    out.with_suffix('.json').write_text(json.dumps({'names':names,'shapes':shapes,'source':str(path)},indent=2))
    print(path.name,'saved',len(names),'nodes',sum(x['nvertices'] for x in shapes),'vertices')

if __name__=='__main__':
    import argparse
    p=argparse.ArgumentParser(description='Extract actual Siff NIF skeleton and skinning fixtures.')
    p.add_argument('--meshes',type=Path,required=True,help='Directory containing siff_hair.nif and siff_regalia.nif')
    args=p.parse_args()
    for name in ['siff_hair','siff_regalia']:
        extract(args.meshes/(name+'.nif'),Path(__file__).resolve().parent/(name+'.npz'))
