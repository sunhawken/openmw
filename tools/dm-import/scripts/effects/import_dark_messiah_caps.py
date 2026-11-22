#!/usr/bin/env python3
"""Bake the neck-cut flesh texture from the user's Dark Messiah.

Compares intact/cut LOD0 topology, then samples the authored flesh triangles.
"""
import zlib
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'scripts/animation'))
from dm_sources import archives, entries, extract
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from pins import pinned_commit  # noqa: E402
PIN='6deac3176bf0e8dd2a50038cfa1066abfa58c1f9'


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--vpk-root',type=Path,required=True)
    ap.add_argument('--output',type=Path,required=True)
    ap.add_argument('--sourceio',type=Path,default=ROOT/'third_party/SourceIO')
    args=ap.parse_args()
    output=args.output.resolve()
    if output.is_relative_to(ROOT):
        ap.error('Game-derived textures must go outside the importer folder')
    actual=pinned_commit(args.sourceio)
    if actual!=PIN:raise ValueError('SourceIO pin mismatch')
    wanted={f'mm/models/npc/necroguard/{name}{suffix}'
        for name in ('npc_necroguard','npc_necroguard_nohead','npc_necroguard_nohead_link')
        for suffix in ('.mdl','.vvd','.dx90.vtx')}
    wanted.add('mm/materials/models/npc_necroguard/necroguard_full.vtf')
    index={}
    for archive in archives(args.vpk_root):
        for name,entry in entries(archive):
            if name.lower() in wanted:index[name.lower()]=(archive,entry)
    if set(index)!=wanted:raise ValueError('Missing original cut-model inputs')
    output.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='dm-cut-bake-',dir=output.parent) as temporary:
        work=Path(temporary);sources={}
        for name,(archive,entry) in index.items():
            data=extract(archive,entry);p=work/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(data)
            sources[name]=dict(archive=archive.name,crc32=f'{zlib.crc32(data):08x}',sha256=hashlib.sha256(data).hexdigest())
        env=dict(os.environ,BRUTAL_SOURCEIO=str(args.sourceio.resolve()),
            BRUTAL_DM_MODEL_DIR=str(work/'mm/models/npc/necroguard'),
            BRUTAL_DM_ATLAS=str(work/'mm/materials/models/npc_necroguard/necroguard_full.vtf'),
            BRUTAL_DM_OUTPUT=str(work/'bake'))
        subprocess.run([sys.executable,str(Path(__file__).with_name('dark_messiah_cap_bake.py'))],env=env,check=True)
        proof=json.loads((work/'bake/cap-proof.json').read_text())
        outputs={}
        from PIL import Image
        for side in ('body','head'):
            source=work/'bake'/f'{side}_flesh_opaque.png'
            image=Image.open(source)
            if image.mode!='RGB' or image.size!=(512,512):raise ValueError('Expected opaque original flesh')
            relative=f'textures/brutal_dm/cut_{side}.png';dest=output/relative;dest.parent.mkdir(parents=True,exist_ok=True)
            shutil.copy2(source,dest)
            if Image.open(dest).tobytes()!=image.tobytes():raise ValueError('Cap image round trip failed')
            outputs[side]=dict(path=relative,sha256=hashlib.sha256(dest.read_bytes()).hexdigest(),
                channels='RGB',width=512,height=512)
        manifest=dict(schema=1,derived_from_game_files=True,sourceio=PIN,sources=sources,outputs=outputs,
            intact_partition_proof=proof['intact_partition_proof'],
            algorithm='Authored 30-triangle flesh disk; radial mapping to true boundary; original barycentric UV and bilinear atlas samples; radius clamped at disk boundary; opaque RGB, collar excluded',
            image_orientation='Source +X right, +Y up; image rows retained; native ImageManager applies normal PNG orientation')
        (output/'cut-gore-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
        print(json.dumps(outputs,indent=2))


if __name__=='__main__':main()
