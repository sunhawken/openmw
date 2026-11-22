"""Explicit first-person bone correspondence, with structural validation.

Names address measured landmarks; they do not establish anatomical equivalence.
This module validates explicit rig hierarchy and rest-basis consistency. It does
not check a generated pose or physical contact with an item.
"""
import hashlib
import json
from pathlib import Path

import numpy as np

from retarget_kf import cross, norm
PATH = Path(__file__).with_suffix('.json')
CONTRACT = json.loads(PATH.read_text())
if CONTRACT['schema'] != 1:
    raise ValueError('Unsupported first-person retarget correspondence schema')
SHA256 = hashlib.sha256(PATH.read_bytes()).hexdigest()


def sides():
    keys = ('clavicle', 'shoulder', 'elbow', 'palm')
    return {side: dict(dm=tuple(row['source'][k] for k in keys),
                       mw=tuple(row['target'][k] for k in keys))
            for side, row in CONTRACT['sides'].items()}


def digit_chains(side):
    return CONTRACT['sides'][side]['digits']


def validate(source_bones, native_nodes, native_parents, native_rest):
    """Reject missing/ambiguous landmarks or a different rig topology.

    Source spelling is explicitly case-insensitive because the owned rig mixes
    SK/Sk. There is no approximate-name or neighbouring-finger fallback.
    """
    source = {b.name.casefold(): b for b in source_bones}
    if len(source) != len(source_bones):
        raise ValueError('Ambiguous case-insensitive Source bone names')
    for bone in source_bones:
        q=np.asarray(bone.quat,float)
        if not np.isfinite(q).all() or not np.isfinite(bone.position).all() or norm(q)<1e-8:
            raise ValueError(f'Source: invalid bind transform on {bone.name}')
    source_parents = {b.name.casefold(): source_bones[b.parent_id].name.casefold()
                      for b in source_bones if b.parent_id >= 0}
    counts = {'arm_landmarks': 0, 'source_digit_points': 0, 'native_phalanges': 0}
    rest_errors = {}

    def require_chain(names, lookup, parents, kind):
        for name in names:
            if name not in lookup:
                raise ValueError(f'{kind}: missing explicit landmark {name}')
            if kind.startswith('Native'):
                node=lookup[name]
                matrix=np.asarray(native_rest[name],float)
                if not np.isfinite(matrix).all() or not np.isfinite(node.translation).all():
                    raise ValueError(f'{kind}: nonfinite rest transform on {name}')
                if not np.isfinite(node.scale) or abs(float(node.scale)-1.) > 1e-5:
                    raise ValueError(f'{kind}: non-unit scale on {name}')
                r=matrix[:3,:3]
                if abs(np.linalg.det(r)-1.)>1e-4 or np.max(np.abs(r.T@r-np.eye(3)))>1e-4:
                    raise ValueError(f'{kind}: non-orthonormal rest frame on {name}')
        for parent, child in zip(names, names[1:]):
            if parents.get(child) != parent:
                raise ValueError(f'{kind}: expected {child} parent {parent}, got {parents.get(child)}')

    def frame(a, b):
        a = a / norm(a)
        b = b - a * (a @ b)
        if norm(b) < 1e-6:
            raise ValueError('Degenerate native rest bend-plane basis')
        b /= norm(b)
        return np.column_stack((a, b, cross(a, b)))

    for side, row in CONTRACT['sides'].items():
        src, dst = row['source'], row['target']
        roles = ('clavicle', 'shoulder', 'elbow', 'forearm_helper', 'distal_twist_helper', 'palm')
        require_chain([src[k].casefold() for k in roles], source, source_parents, f'Source {side} arm')
        names = [dst[k] for k in ('clavicle', 'shoulder', 'elbow', 'palm')]
        require_chain(names, native_nodes, native_parents, f'Native {side} arm')
        counts['arm_landmarks'] += len(roles) + len(names)
        for digit in row['digits']:
            require_chain([src['palm'].casefold()] + [n.casefold() for n in digit['source']],
                          source, source_parents, f'Source {side} {digit["role"]}')
            require_chain([dst['palm']] + digit['target'], native_nodes, native_parents,
                          f'Native {side} {digit["role"]}')
            counts['source_digit_points'] += len(digit['source'])
            counts['native_phalanges'] += len(digit['target'])
        u, f, h = [native_rest[dst[k]] for k in ('shoulder', 'elbow', 'palm')]
        a, b = f[:3,3]-u[:3,3], h[:3,3]-f[:3,3]
        normal = cross(a,b)
        if norm(normal) < 1e-6:
            raise ValueError(f'Native {side} rest arm has no stable bend plane')
        normal /= norm(normal)
        errors = []
        for node, child, direction in ((u, dst['elbow'], a), (f, dst['palm'], b)):
            rotation = node[:3,:3]
            local_axis = np.asarray(native_nodes[child].translation, float)
            reconstructed = frame(direction, normal) @ frame(local_axis, rotation.T @ normal).T
            errors.append(float(np.max(np.abs(reconstructed-rotation))))
        if max(errors) > 1e-4:
            raise ValueError(f'Native {side} oriented rest does not round-trip: {errors}')
        rest_errors[side] = errors
    for attachment in CONTRACT['attachments']:
        name=attachment['target']
        if attachment.get('generated_target') and name not in native_nodes:
            continue
        if name not in native_nodes or native_parents.get(name)!=attachment['target_parent']:
            raise ValueError(f'Native attachment {name}: expected parent {attachment["target_parent"]}')
    return dict(contract=CONTRACT['id'], schema=CONTRACT['schema'], sha256=SHA256,
                scope='structural hierarchy and rest-basis self-consistency; solver, pose and contact not checked',
                counts=counts, rest_basis_self_consistency=rest_errors)


def validate_attachment(source_name, actual_parent):
    rows=[row for row in CONTRACT['attachments'] if row['source'].casefold()==source_name.casefold()]
    if not rows:
        return None
    row=rows[0]
    if actual_parent.casefold()!=row['source_parent'].casefold():
        raise ValueError(f'Attachment {source_name}: expected parent {row["source_parent"]}, got {actual_parent}')
    return dict(source=source_name, source_parent=actual_parent, target=row['target'],
                target_parent=row['target_parent'], scope='attachment parent only; physical offset/axes require separate fitting')


def measure_grip_solver(native_nodes, native_rest, hinges):
    """Measure the eig-hinge/half-hand-roll basis at native rest.

    Uses the solver's hinge and roll formula rather than rebuilding a basis from
    the tested pose, and reports its orientation error.
    """
    def frame(a, b):
        a = a / norm(a); b = b - a * (a @ b); b /= norm(b)
        return np.column_stack((a,b,cross(a,b)))

    def error(a,b):
        # Orthonormalize float32 NIF matrices before measuring the angle.
        u,_,v=np.linalg.svd(a.T @ b); r=u@v
        return float(np.degrees(np.arccos(np.clip((np.trace(r)-1)/2,-1,1))))

    errors={}
    for side,row in CONTRACT['sides'].items():
        target=row['target']; u,f,h=[native_rest[target[k]] for k in ('shoulder','elbow','palm')]
        normal=cross(f[:3,3]-u[:3,3],h[:3,3]-f[:3,3]); normal/=norm(normal)
        hinge=hinges[side].copy(); hinge[0]=0.; hinge/=norm(hinge)
        basis=frame(np.array([1.,0.,0.]),hinge)
        ru=frame(f[:3,3]-u[:3,3],normal) @ basis.T
        rf=frame(h[:3,3]-f[:3,3],normal) @ basis.T
        local=rf.T @ h[:3,:3]
        twist=np.arctan2(local[2,1]-local[1,2],local[1,1]+local[2,2])/2
        c,s=np.cos(twist),np.sin(twist)
        rf=rf @ np.array([[1.,0.,0.],[0.,c,-s],[0.,s,c]])
        errors[side]=dict(upper_degrees=error(ru,u[:3,:3]),forearm_degrees=error(rf,f[:3,:3]))
    return dict(profile='grip-ik', conforms=False, rest_orientation_errors=errors,
                palm_mapping='item-axis R / middle-thumb L; fixed reference skin frame not applied',
                fingers='native static sword grip or direction-only family mapping; axial and terminal4 transfer unaudited',
                reason='Historical reproduction only. Structural correspondence does not certify this solver.')


def main():
    import argparse
    p=argparse.ArgumentParser(description='Audit explicit bone correspondence and the actual importer arm solver.')
    p.add_argument('--vpk-root',type=Path,required=True)
    p.add_argument('--work-dir',type=Path,required=True)
    p.add_argument('--skeleton',type=Path,required=True)
    p.add_argument('--grip-animation',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--structure-only',action='store_true',help='Inspect only the hierarchy, not the pose solver')
    a=p.parse_args()
    from import_viewmodel import Viewmodel
    vm=Viewmodel(a.vpk_root,a.work_dir,a.skeleton,a.grip_animation)
    report=vm.bone_correspondence
    report['requested_scope']='structure-only' if a.structure_only else 'structure-and-pose-solver'
    report['passed']=a.structure_only or report['implementation']['conforms']
    a.output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))
    raise SystemExit(not report['passed'])


if __name__=='__main__':
    main()
