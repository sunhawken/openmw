"""Authored feet-first slide on the Morrowind skeleton.

This is authored skeletal geometry. Actor space is +Y forward, +Z up; translations
are in Morrowind units. The existing first-person Camera and pitch pivot remain
unchanged.
"""
from pathlib import Path
import sys

import numpy as np

from retarget_kf import cross, norm
from retarget_kf import quat_matrix, matrix_quat

sys.path.insert(0, str(Path(__file__).resolve().parents[2]/'third_party/io_scene_mw/lib'))
from es3 import nif


def mat(rotation, position):
    m = np.eye(4); m[:3, :3] = rotation; m[:3, 3] = position
    return m


def rotation(axis, degrees):
    axis = np.asarray(axis, float)
    axis /= norm(axis)
    x, y, z = axis
    skew = np.array([[0, -z, y], [z, 0, -x], [-y, x, 0]])
    angle = np.radians(degrees)
    return np.eye(3) + np.sin(angle)*skew + (1-np.cos(angle))*(skew@skew)


def frame(direction, normal):
    x = direction / norm(direction)
    y = normal - x*np.dot(x, normal)
    y /= norm(y)
    return np.column_stack((x, y, cross(x, y)))


class SlidePose:
    """Grounded, vertically stacked legs and a torso lying partly on its side.

    Solve the legs on every transition sample. Interpolating just the final
    bone rotations while lowering the pelvis drives the feet through the floor.
    """
    def __init__(self, rig, idle):
        self.rig, self.idle = rig, idle
        self.base = rig.world(idle)
        self.pelvis = np.array([0., 24., 13.])
        self.recline, self.roll, self.yaw = 78., -65., 0.
        self.ankles = {'R': np.array([0., 91., 17.]),
                       'L': np.array([0., 93., 4.])}
        self.bend = {side: np.array([-.9063, 0., .4226]) for side in 'LR'}

    def sample(self, weight):
        # Smoothstep gives zero velocity at idle and the stationary slide loop.
        u = np.clip(float(weight), 0., 1.)
        u = u*u*(3-2*u)
        # The physical eye lowers linearly. Let the chest retreat first so that
        # the eye cannot descend into its collar before the body starts moving.
        # This composition is monotonic and still has zero endpoint velocity.
        u = 1-(1-u)**3
        pose = {n: (r.copy(), p.copy()) for n, (r, p) in self.idle.items()}
        if u == 0:
            return pose
        # Roll the already reclined body around the slide's forward axis. The
        # hip line becomes mostly vertical, so the thighs lie one over another.
        tilt = rotation([0, 0, 1], self.yaw*u) @ rotation([0, 1, 0], self.roll*u) @ rotation([1, 0, 0], self.recline*u)
        name = 'Bip01 Pelvis'
        parent = self.base[self.rig.parents[name]]
        target = (1-u)*self.base[name][:3, 3] + u*self.pelvis
        pose[name] = (parent[:3, :3].T @ tilt @ self.base[name][:3, :3],
                      parent[:3, :3].T @ (target-parent[:3, 3]))
        for side in 'LR':
            thigh, calf, foot = [f'Bip01 {side} {n}' for n in ('Thigh', 'Calf', 'Foot')]
            world = self.rig.world(pose)
            hip = world[thigh][:3, 3]
            ankle = (1-u)*self.base[foot][:3, 3] + u*self.ankles[side]
            # The heel sweeps down when the boot pitches up. Give it clearance
            # during that sweep, then settle the lower boot near the floor.
            ankle[2] += 12*u*(1-u)
            # The upper leg passes above the lower shin while they align, then
            # extends onto it. A straight ankle interpolation would cut through
            # the lower knee part halfway through the roll.
            if side == 'R': ankle[2] += 40*u*(1-u)
            lengths = [norm(self.rig.rest[n][1]) for n in (calf, foot)]
            reach = ankle-hip
            distance = norm(reach)
            if not abs(lengths[0]-lengths[1]) < distance < sum(lengths):
                raise ValueError(f'{side} leg target outside reach: {distance}, {lengths}')
            axis = reach/distance
            p0, p1, p2 = [self.base[n][:3, 3] for n in (thigh, calf, foot)]
            base_axis = (p2-p0)/norm(p2-p0)
            old_bend = p1-p0-base_axis*np.dot(p1-p0, base_axis)
            old_bend /= norm(old_bend)
            bend = (1-u)*old_bend + u*self.bend[side]
            bend -= axis*np.dot(bend, axis)
            bend /= norm(bend)
            along = (lengths[0]**2-lengths[1]**2+distance**2)/(2*distance)
            knee = hip + axis*along + bend*np.sqrt(max(0., lengths[0]**2-along**2))
            normal = cross(knee-hip, ankle-knee)
            normal /= norm(normal)
            old_normal = cross(p1-p0, p2-p1)
            old_normal /= norm(old_normal)
            rotations = {n: frame(direction, normal) @ frame(self.base[n][:3, 0], old_normal).T @ self.base[n][:3, :3]
                         for n, direction in ((thigh, knee-hip), (calf, ankle-knee))}
            pose[thigh] = (world[self.rig.parents[thigh]][:3, :3].T @ rotations[thigh], pose[thigh][1])
            pose[calf] = (rotations[thigh].T @ rotations[calf], pose[calf][1])
            # Heel near the floor, toes raised, so the boot does not face straight
            # into the camera.
            toe = self.base[foot][:3, 1]
            parallel_yaw = np.degrees(np.arctan2(toe[0], toe[1]))
            foot_world = rotation([0, 1, 0], self.roll*u) @ rotation([1, 0, 0], 55*u) @ rotation([0, 0, 1], parallel_yaw*u) @ self.base[foot][:3, :3]
            pose[foot] = (rotations[calf].T @ foot_world, pose[foot][1])
            np.testing.assert_allclose(self.rig.world(pose)[foot][:3, 3], ankle, atol=1e-5)
        return pose

    def description(self):
        return {'source': 'authored skeletal slide; native Morrowind proportions',
                'pelvis': self.pelvis.tolist(), 'recline_degrees': self.recline,
                'roll_degrees': self.roll, 'yaw_degrees': self.yaw,
                'roll_order': 'world forward-axis roll after reclining; right leg above left',
                'ankles': {k: v.tolist() for k, v in self.ankles.items()},
                'transitions': 'body-leading cubic smoothstep plus two-bone IK at every sample',
                'note': 'Authored legs.'}


class Rig:
    def __init__(self, path):
        s = nif.NiStream(); s.load(path)
        self.nodes = {n.name:n for n in s.objects_of_type(nif.NiNode) if n.name}
        self.parents = {c.name:n.name for n in self.nodes.values() for c in n.children
                        if isinstance(c, nif.NiNode) and c.name}
        self.rest = {n:(np.asarray(v.rotation,float),np.asarray(v.translation,float)) for n,v in self.nodes.items()}

    def world(self, pose):
        out = {}
        def get(n):
            if n not in out:
                r,p = pose.get(n,self.rest[n]); m=mat(r,p)
                out[n]=(get(self.parents[n]) if n in self.parents else np.eye(4))@m
            return out[n]
        for n in self.nodes:get(n)
        return out

    def sample(self, path, t=0):
        s=nif.NiStream();s.load(path)
        names=[e.string_data for e in s.root.extra_datas if isinstance(e,nif.NiStringExtraData)]
        out={}
        for n,c in zip(names,s.root.controllers):
            if n not in self.nodes or c.data is None:continue
            r,p=self.rest[n]
            if c.data.rotations is not None and len(c.data.rotations.keys):
                keys=c.data.rotations.keys
                q=np.array([np.interp(t,keys[:,0],keys[:,i]) for i in range(1,5)]);q/=norm(q)
                r=quat_matrix(q[[1,2,3,0]])
            if c.data.translations is not None and len(c.data.translations.keys):
                keys=c.data.translations.keys
                p=np.array([np.interp(t,keys[:,0],keys[:,i]) for i in range(1,4)])
            out[n]=r,p
        return out



def write_kf(path, times, poses, marks):
    s=nif.NiStream();s.root=nif.NiSequenceStreamHelper(extra_data=nif.NiTextKeyExtraData(
        keys=np.array([(float(t),'brutal_slide: '+m) for t,m in marks],dtype='<f,O')))
    names=list(poses[0]);duration=float(times[-1])
    for n in names:
        rotations=np.array([[t,*matrix_quat(p[n][0])] for t,p in zip(times,poses)],np.float32)
        translations=np.array([[t,*p[n][1]] for t,p in zip(times,poses)],np.float32)
        for i in range(1,len(rotations)):
            if rotations[i-1,1:]@rotations[i,1:]<0:rotations[i,1:]*=-1
        assert np.all(np.isfinite(rotations)) and np.all(np.isfinite(translations))
        data=nif.NiKeyframeData(rotations=nif.NiRotData(keys=rotations),translations=nif.NiPosData(keys=translations))
        s.root.extra_datas.append(nif.NiStringExtraData(string_data=n))
        s.root.controllers.append(nif.NiKeyframeController(data=data,start_time=0.,stop_time=duration,flags=12))
    path.parent.mkdir(parents=True,exist_ok=True);s.save(path)
    check=nif.NiStream();check.load(path)
    assert len(list(check.root.controllers))==len(names)
    for c,n in zip(check.root.controllers,names):
        np.testing.assert_allclose(c.data.translations.keys,np.array([[t,*p[n][1]] for t,p in zip(times,poses)]),atol=1e-5)
    return {'path':str(path),'bones':len(names),'samples':len(times),'duration':duration,'roundtrip':'passed'}
