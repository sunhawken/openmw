#!/usr/bin/env python3
"""Generate a synthetic skinned Collada (.dae) test character.

Everything is generated: box geometry, a skeleton whose names are NOT the
Morrowind Bip01 set, two skinned meshes on that one skeleton, a texture
reference, named animation clips and the OpenMW text-key sidecar (.txt).

The skeleton deliberately contains names that the old loader mangled or
ignored:
  - the root bone is "rig_root" (no "Bip01"),
  - "arm_l" and "arm l" both exist (an underscore/space collision),
  - "spine.upper" contains a dot.

Usage:
  make_collada_rig.py OUT_DIR [--name rig] [--bip01]

--bip01 writes the same rig with stock Bip01 names (Bip01, Bip01 Pelvis, ...),
exporter style: exact names in `name`, underscored ids. --legacy writes ids
only (no `name` attributes), like old exports that relied on the
underscore-to-space rule.
"""
import argparse
import math
import os

# Bone: (name, parent, local translation)
CUSTOM_BONES = [
    ("rig_root", None, (0, 0, 0)),
    ("pelvis", "rig_root", (0, 0, 90)),
    ("spine_lower", "pelvis", (0, 0, 15)),
    ("spine.upper", "spine_lower", (0, 0, 20)),
    ("neck", "spine.upper", (0, 0, 25)),
    ("arm_l", "spine.upper", (20, 0, 20)),
    ("arm l", "arm_l", (25, 0, 0)),  # collides with arm_l if '_' becomes ' '
    ("arm_r", "spine.upper", (-20, 0, 20)),
    ("hand_r", "arm_r", (-25, 0, 0)),
]

BIP01_BONES = [
    ("Bip01", None, (0, 0, 0)),
    ("Bip01 Pelvis", "Bip01", (0, 0, 90)),
    ("Bip01 Spine", "Bip01 Pelvis", (0, 0, 15)),
    ("Bip01 Spine1", "Bip01 Spine", (0, 0, 20)),
    ("Bip01 Neck", "Bip01 Spine1", (0, 0, 25)),
    ("Bip01 L Clavicle", "Bip01 Spine1", (20, 0, 20)),
    ("Bip01 L UpperArm", "Bip01 L Clavicle", (25, 0, 0)),
    ("Bip01 R Clavicle", "Bip01 Spine1", (-20, 0, 20)),
    ("Bip01 R Hand", "Bip01 R Clavicle", (-25, 0, 0)),
]


LEGACY = False
BIP01_STYLE = False


def sid(name):
    """Collada ids/sids cannot contain spaces. Stock-style rigs use exporter ids
    (spaces to underscores); the custom rig uses an encoding that keeps the
    underscore/space pair distinct, so its names survive only via `name`."""
    if BIP01_STYLE:
        return name.replace(" ", "_")
    return "j_" + "".join(c if c.isalnum() else "_%02x" % ord(c) for c in name)


def xml_escape(text):
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', "&quot;")


def translate(t):
    return [1, 0, 0, t[0], 0, 1, 0, t[1], 0, 0, 1, t[2], 0, 0, 0, 1]


def rot_z(t, angle):
    c, s = math.cos(angle), math.sin(angle)
    return [c, -s, 0, t[0], s, c, 0, t[1], 0, 0, 1, t[2], 0, 0, 0, 1]


def rot_x(t, angle):
    c, s = math.cos(angle), math.sin(angle)
    return [1, 0, 0, t[0], 0, c, -s, t[1], 0, s, c, t[2], 0, 0, 0, 1]


def world_translations(bones):
    out = {}
    for name, parent, t in bones:
        base = out[parent] if parent else (0, 0, 0)
        out[name] = (base[0] + t[0], base[1] + t[1], base[2] + t[2])
    return out


def box(center, half):
    cx, cy, cz = center
    hx, hy, hz = half
    verts = []
    for dz in (-1, 1):
        for dy in (-1, 1):
            for dx in (-1, 1):
                verts.append((cx + dx * hx, cy + dy * hy, cz + dz * hz))
    quads = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
    tris = []
    for a, b, c, d in quads:
        tris += [a, b, c, a, c, d]
    return verts, tris


def fmt(values):
    return " ".join("%g" % v for v in values)


def mesh_xml(mid, parts):
    """parts: list of (center, half, bone). Returns geometry xml and per-vertex bones."""
    verts, tris, vbones = [], [], []
    for center, half, bone in parts:
        v, t = box(center, half)
        base = len(verts)
        verts += v
        tris += [base + i for i in t]
        vbones += [bone] * len(v)
    uv = []
    for i in range(len(verts)):
        uv += [(i % 2), ((i // 2) % 2)]
    xml = f"""    <geometry id="{mid}" name="{mid}">
      <mesh>
        <source id="{mid}-pos"><float_array id="{mid}-pos-a" count="{len(verts) * 3}">{fmt([c for v in verts for c in v])}</float_array>
          <technique_common><accessor source="#{mid}-pos-a" count="{len(verts)}" stride="3"><param name="X" type="float"/><param name="Y" type="float"/><param name="Z" type="float"/></accessor></technique_common></source>
        <source id="{mid}-uv"><float_array id="{mid}-uv-a" count="{len(uv)}">{fmt(uv)}</float_array>
          <technique_common><accessor source="#{mid}-uv-a" count="{len(verts)}" stride="2"><param name="S" type="float"/><param name="T" type="float"/></accessor></technique_common></source>
        <vertices id="{mid}-vtx"><input semantic="POSITION" source="#{mid}-pos"/></vertices>
        <triangles material="mat" count="{len(tris) // 3}">
          <input semantic="VERTEX" source="#{mid}-vtx" offset="0"/>
          <input semantic="TEXCOORD" source="#{mid}-uv" offset="1" set="0"/>
          <p>{" ".join("%d %d" % (i, i) for i in tris)}</p>
        </triangles>
      </mesh>
    </geometry>
"""
    return xml, vbones


def skin_xml(cid, mid, joints, vbones, world):
    inv = []
    for j in joints:
        w = world[j]
        inv += translate((-w[0], -w[1], -w[2]))
    index = {j: i for i, j in enumerate(joints)}
    vcount = " ".join("1" for _ in vbones)
    v = " ".join("%d 0" % index[b] for b in vbones)
    return f"""    <controller id="{cid}" name="{cid}">
      <skin source="#{mid}">
        <bind_shape_matrix>1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1</bind_shape_matrix>
        <source id="{cid}-joints"><Name_array id="{cid}-joints-a" count="{len(joints)}">{" ".join(sid(j) for j in joints)}</Name_array>
          <technique_common><accessor source="#{cid}-joints-a" count="{len(joints)}" stride="1"><param name="JOINT" type="name"/></accessor></technique_common></source>
        <source id="{cid}-bind"><float_array id="{cid}-bind-a" count="{len(joints) * 16}">{fmt(inv)}</float_array>
          <technique_common><accessor source="#{cid}-bind-a" count="{len(joints)}" stride="16"><param name="TRANSFORM" type="float4x4"/></accessor></technique_common></source>
        <source id="{cid}-w"><float_array id="{cid}-w-a" count="1">1</float_array>
          <technique_common><accessor source="#{cid}-w-a" count="1" stride="1"><param name="WEIGHT" type="float"/></accessor></technique_common></source>
        <joints><input semantic="JOINT" source="#{cid}-joints"/><input semantic="INV_BIND_MATRIX" source="#{cid}-bind"/></joints>
        <vertex_weights count="{len(vbones)}">
          <input semantic="JOINT" source="#{cid}-joints" offset="0"/>
          <input semantic="WEIGHT" source="#{cid}-w" offset="1"/>
          <vcount>{vcount}</vcount>
          <v>{v}</v>
        </vertex_weights>
      </skin>
    </controller>
"""


def node_xml(bones, name, depth=3):
    t = dict((b[0], b[2]) for b in bones)[name]
    children = [b[0] for b in bones if b[1] == name]
    pad = "  " * depth
    inner = "".join(node_xml(bones, c, depth + 1) for c in children)
    name_attr = "" if LEGACY else f' name="{xml_escape(name)}"'
    return (f'{pad}<node id="{sid(name)}" sid="{sid(name)}"{name_attr} type="JOINT">\n'
            f'{pad}  <matrix sid="transform">{fmt(translate(t))}</matrix>\n{inner}{pad}</node>\n')


def animation_xml(aid, bone, local_t, times, matrices):
    out = []
    for m in matrices:
        out += m
    return f"""    <animation id="{aid}">
      <source id="{aid}-in"><float_array id="{aid}-in-a" count="{len(times)}">{fmt(times)}</float_array>
        <technique_common><accessor source="#{aid}-in-a" count="{len(times)}" stride="1"><param name="TIME" type="float"/></accessor></technique_common></source>
      <source id="{aid}-out"><float_array id="{aid}-out-a" count="{len(out)}">{fmt(out)}</float_array>
        <technique_common><accessor source="#{aid}-out-a" count="{len(times)}" stride="16"><param name="TRANSFORM" type="float4x4"/></accessor></technique_common></source>
      <source id="{aid}-interp"><Name_array id="{aid}-interp-a" count="{len(times)}">{" ".join("LINEAR" for _ in times)}</Name_array>
        <technique_common><accessor source="#{aid}-interp-a" count="{len(times)}" stride="1"><param name="INTERPOLATION" type="name"/></accessor></technique_common></source>
      <sampler id="{aid}-s"><input semantic="INPUT" source="#{aid}-in"/><input semantic="OUTPUT" source="#{aid}-out"/><input semantic="INTERPOLATION" source="#{aid}-interp"/></sampler>
      <channel source="#{aid}-s" target="{sid(bone)}/transform"/>
    </animation>
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out_dir")
    ap.add_argument("--name", default="rig")
    ap.add_argument("--bip01", action="store_true")
    ap.add_argument("--legacy", action="store_true", help="with --bip01: ids only, no name attributes")
    args = ap.parse_args()
    global LEGACY, BIP01_STYLE
    LEGACY = args.legacy
    BIP01_STYLE = args.bip01
    bones = BIP01_BONES if args.bip01 else CUSTOM_BONES
    names = [b[0] for b in bones]
    root, pelvis, spine_low, spine_up, neck, arm_l, arm_l2, arm_r, hand_r = names
    local = dict((b[0], b[2]) for b in bones)
    world = world_translations(bones)

    body_parts = [(world[pelvis], (8, 6, 8), pelvis), (world[spine_up], (10, 6, 12), spine_up),
                  (world[neck], (5, 5, 6), neck)]
    arm_parts = [(world[arm_l], (6, 3, 3), arm_l), (world[arm_l2], (6, 3, 3), arm_l2),
                 (world[arm_r], (6, 3, 3), arm_r), (world[hand_r], (4, 4, 4), hand_r)]
    geo_body, vb_body = mesh_xml("body_mesh", body_parts)
    geo_arms, vb_arms = mesh_xml("arms_mesh", arm_parts)

    # Clips: (name, bone, keyframe function, duration). Each keeps the bone's
    # rest translation and rotates it, so the motion is measurable per bone.
    clips = [
        # Starts at 0. Not named "idle": the engine plays "idle" on every actor
        # itself, which would interfere with a scripted check.
        ("sway", spine_up, lambda a: rot_z(local[spine_up], 0.9 * math.sin(a)), 2.0),
        ("wave_hello", arm_l, lambda a: rot_x(local[arm_l], 1.2 * math.sin(a)), 1.0),
        ("Slash Combo.2", hand_r, lambda a: rot_z(local[hand_r], 1.5 * math.sin(a)), 0.8),
    ]
    anim_xml, clip_xml, keys = "", "", []
    offset = 0.0
    for i, (clip, bone, fn, dur) in enumerate(clips):
        times = [offset + dur * k / 8 for k in range(9)]
        mats = [fn(2 * math.pi * k / 8) for k in range(9)]
        aid = "anim%d" % i
        anim_xml += animation_xml(aid, bone, local[bone], times, mats)
        clip_xml += (f'    <animation_clip id="clip{i}" name="{clip}" start="{offset:g}" end="{offset + dur:g}">'
                     f'<instance_animation url="#{aid}"/></animation_clip>\n')
        group = clip.lower()
        keys.append(f"{group}: start {offset:g}")
        if group == "slash combo.2":
            keys.append(f"{group}: hit {offset + dur / 2:g}")
        keys.append(f"{group}: stop {offset + dur:g}")
        offset += dur

    body_joints = [pelvis, spine_low, spine_up, neck]
    arm_joints = [arm_l, arm_l2, arm_r, hand_r]
    nodes = node_xml(bones, root)
    dae = f"""<?xml version="1.0" encoding="utf-8"?>
<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1">
  <asset><unit name="meter" meter="1"/><up_axis>Z_UP</up_axis></asset>
  <library_images><image id="tex" name="tex"><init_from>{args.name}_tex.png</init_from></image></library_images>
  <library_effects><effect id="fx"><profile_COMMON>
    <newparam sid="surf"><surface type="2D"><init_from>tex</init_from></surface></newparam>
    <newparam sid="samp"><sampler2D><source>surf</source></sampler2D></newparam>
    <technique sid="common"><lambert><diffuse><texture texture="samp" texcoord="UVMap"/></diffuse></lambert></technique>
  </profile_COMMON></effect></library_effects>
  <library_materials><material id="mat" name="mat"><instance_effect url="#fx"/></material></library_materials>
  <library_geometries>
{geo_body}{geo_arms}  </library_geometries>
  <library_controllers>
{skin_xml("body_skin", "body_mesh", body_joints, vb_body, world)}{skin_xml("arms_skin", "arms_mesh", arm_joints, vb_arms, world)}  </library_controllers>
  <library_animations>
{anim_xml}  </library_animations>
  <library_animation_clips>
{clip_xml}  </library_animation_clips>
  <library_visual_scenes>
    <visual_scene id="scene" name="scene">
{nodes}      <node id="body" name="body"><instance_controller url="#body_skin"><skeleton>#{sid(root)}</skeleton>
        <bind_material><technique_common><instance_material symbol="mat" target="#mat"><bind_vertex_input semantic="UVMap" input_semantic="TEXCOORD" input_set="0"/></instance_material></technique_common></bind_material></instance_controller></node>
      <node id="arms" name="arms"><instance_controller url="#arms_skin"><skeleton>#{sid(root)}</skeleton>
        <bind_material><technique_common><instance_material symbol="mat" target="#mat"><bind_vertex_input semantic="UVMap" input_semantic="TEXCOORD" input_set="0"/></instance_material></technique_common></bind_material></instance_controller></node>
    </visual_scene>
  </library_visual_scenes>
  <scene><instance_visual_scene url="#scene"/></scene>
</COLLADA>
"""
    os.makedirs(args.out_dir, exist_ok=True)
    with open(os.path.join(args.out_dir, args.name + ".dae"), "w") as f:
        f.write(dae)
    with open(os.path.join(args.out_dir, args.name + ".txt"), "w") as f:
        f.write("\n".join(keys) + "\n")
    # A 2x2 generated texture (plain PNG, written by hand to avoid dependencies).
    import struct
    import zlib

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    raw = b"\x00" + bytes([200, 60, 60, 60, 200, 60]) + b"\x00" + bytes([60, 60, 200, 200, 200, 60])
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 2, 2, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
    with open(os.path.join(args.out_dir, args.name + "_tex.png"), "wb") as f:
        f.write(png)
    print("wrote", os.path.join(args.out_dir, args.name + ".dae"), "bones:", ", ".join(names))


if __name__ == "__main__":
    main()
