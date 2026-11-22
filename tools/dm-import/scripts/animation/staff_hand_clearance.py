"""Native hand-size adaptation for authored staff hand-over-hand passes.

Only coherent left-arm translations along the shaft are emitted. Source palm,
forearm and shoulder rotations, articulation, fingers, weapon and clocks remain
untouched. Both native hand meshes constrain the smallest clearing shift.
"""
from pathlib import Path
import numpy as np
from audit_staff_mesh_grip import Hand, Rig, sample_channels
from es3 import nif

def intersections(A,B):
    p=np.concatenate((A[:,0],A[:,1],A[:,2]));q=np.concatenate((A[:,1],A[:,2],A[:,0]))
    # An edge can only cross a triangle where their bounding boxes overlap, so
    # pairs outside the other mesh's box are skipped; the count is unchanged.
    margin=1e-3
    low,high=B.reshape(-1,3).min(0)-margin,B.reshape(-1,3).max(0)+margin
    near=(np.maximum(p,q)>=low).all(1)&(np.minimum(p,q)<=high).all(1)
    p,q=p[near],q[near]
    if not len(p):return 0
    low,high=np.minimum(p,q).min(0)-margin,np.maximum(p,q).max(0)+margin
    B=B[(B.max(1)>=low).all(1)&(B.min(1)<=high).all(1)]
    if not len(B):return 0
    d=q-p
    e1,e2=B[:,1]-B[:,0],B[:,2]-B[:,0]
    h=np.cross(d[:,None,:],e2[None,:,:]);det=np.einsum('ti,eti->et',e1,h)
    inv=np.divide(1,det,out=np.zeros_like(det),where=abs(det)>1e-9)
    offset=p[:,None,:]-B[None,:,0,:];u=np.einsum('eti,eti->et',offset,h)*inv
    c=np.cross(offset,e1[None,:,:]);v=np.einsum('ei,eti->et',d,c)*inv;t=np.einsum('ti,eti->et',e2,c)*inv
    return int(((abs(det)>1e-9)&(u>1e-6)&(v>1e-6)&(u+v<1-1e-6)&(t>1e-6)&(t<1-1e-6)).sum())

def native_hand(path,side):
    scene=nif.NiStream();scene.load(path);hand=object.__new__(Hand);hand.parts=[]
    for shape in scene.objects_of_type(nif.NiTriShape):
        if not shape.name.startswith('Tri '+side+' Hand') or not shape.skin:continue
        hand.parts.append((np.asarray(shape.data.vertices,float),np.asarray(shape.data.triangles),
            [(bone.name,data.vertex_weights['f0'],data.vertex_weights['f1'],data.matrix)
             for bone,data in shape.bone_influences]))
    if not hand.parts:raise ValueError('Missing native hand skin: '+str(path))
    return hand

def adapt(controllers,rig_path,native,sections):
    rig=Rig(rig_path)
    models={sex:{side:native_hand(native/'b'/f'b_n_dark elf_{sex}_hands.1st.nif',side)
                 for side in ('Left','Right')}for sex in ('m','f')}
    channels={n:(c.data.rotations.keys.copy(),c.data.translations.keys.copy())for n,c in controllers.items()}
    grid=channels['Bip01 L Hand'][0][:,0];rows=[]
    for section in sections:
        indices=np.flatnonzero((grid>=section['start']-1e-5)&(grid<=section['stop']+1e-5))
        requirements=[]
        # Subframes catch intersecting interpolation between collision-free keys.
        for t in np.linspace(float(grid[indices[0]]),float(grid[indices[-1]]),4*(len(indices)-1)+1):
            W=rig.world({n:(np.array([[0,*q]]),np.array([[0,*p]]))for n,(q,p)in sample_channels(channels,t).items()},0)
            meshes=[(M['Left'].triangles(W),M['Right'].triangles(W))for M in models.values()]
            def collides(shift):
                delta=np.array([0.,shift,0.])
                return any(intersections(A+delta,B)+intersections(B,A+delta)for A,B in meshes)
            if not collides(0.):continue
            # Preserve which side of the right hand the source performance uses.
            centres=[triangles.reshape(-1,3).mean(0)for triangles in meshes[-1]]
            sign=1. if centres[0][1]>=centres[1][1] else -1.
            for distance in np.arange(.25,8.01,.25):
                if not collides(sign*distance):break
            else:raise ValueError(f'Hand-size correction exceeds 8 native units: {section["section"]}/{t}')
            requirements.append((float(t),sign*(float(distance)+1.0)))
        if not requirements:continue
        signs={np.sign(v)for _,v in requirements}
        if len(signs)!=1:raise ValueError('Clearance interval changes source hand ordering')
        sign=signs.pop();shifts=[]
        for i in indices:
            # Compact C2 envelopes anticipate native-mesh contact and release
            # smoothly. They never hold, retime or rotate an authored limb.
            shift=sign*max(abs(v)*max(0.,1-((float(grid[i])-t)/.16)**2)**3 for t,v in requirements)
            shifts.append(shift)
            if shift==0:continue
            W=rig.world(channels,i);axis=W['Weapon Bone'][:3,1];axis/=np.linalg.norm(axis)
            parent=W[rig.parents['Bip01 L Clavicle']][:3,:3]
            controllers['Bip01 L Clavicle'].data.translations.keys[i,1:]+=parent.T@(axis*shift)
        rows.append(dict(section=section['section'],source_contact_samples=len(requirements),
            max_shift_native_units=max(abs(v)for v in shifts),direction_along_shaft=sign,
            measured_requirements=requirements,only_channel='Bip01 L Clavicle translation',
            envelope_radius_seconds=.16,interpolation_margin_native_units=1.0))
    return rows
