#!/usr/bin/env python3
"""Import Dark Messiah's combat audio and RGBA particle textures.

VPK reads verify the archive's CRC32; PNG round trips verify every decoded RGBA
byte, including transparent-edge colours.
"""
import zlib
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import wave

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/animation'))
from dm_sources import archives, entries, extract
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from pins import pinned_commit  # noqa: E402

SOURCEIO_PIN = '6deac3176bf0e8dd2a50038cfa1066abfa58c1f9'
# Selected active sound-script events; commented-out waves are never imported.
SOUNDS = {
    'sword_swing': ('sword', 'Sword_Swish'),
    'flesh_hit': ('sword', 'Sword_Flesh_Std'),
    'flesh_heavy': ('sword', 'Sword_Flesh_Big'),
    'armor_light': ('sword', 'Sword_Leather_Std'),
    'armor_medium': ('sword', 'Sword_Metal_Std'),
    'armor_heavy': ('sword', 'Sword_Metal_Big'),
    'block_metal': ('sword', 'Sword_Shield_Metal'),
    'block_wood': ('sword', 'Sword_Shield_Wood'),
    'parry': ('sword', 'Sword_Versus_Sword'),
    'blade_contact': ('sword', 'Sword_on_Sword'),
    'metal_hit': ('impacts_weapons', 'SolidMetal.BulletImpact'),
    'stone_hit': ('impacts_weapons', 'Stone.BulletImpact'),
    'wood_hit': ('sword', 'Sword_Wood_Std'),
    'kick_swing': ('player', 'Leg_Swoosh'),
    'kick_hit': ('player', 'Player_Kick'),
    'critical_voice': ('player', 'Player_Critical_Strike'),
    'adrenaline_ready': ('inventory', 'Adrenaline_Reached'),
    # Dark Messiah's named slow-motion cues.
    'slow_motion_in': ('player', 'Slow_Motion_In'),
    'slow_motion_out': ('player', 'Slow_Motion_Out'),
}
MATERIALS = {
    'blood': 'effects/blood_part_001',
    'blood_drop': 'effects/blood_drop',
    'blood_spatter': 'effects/blood_part_002',
    'blood_spray': 'effects/blood_part_003',
    'blood_core': 'effects/blood_core',
    'blood_puff': 'effects/blood_puff',
    'blood_legacy_spray': 'sprites/bloodspray',
    'spark': 'effects/spark',
    'adrenaline_ready': 'hud/fullscreen_adrenaline',
}

def sha(data): return hashlib.sha256(data).hexdigest()

def keyvalues(text):
    """Small duplicate-preserving Valve KeyValues reader for sound scripts."""
    text = re.sub(r'//[^\n]*', '', text)
    tokens = re.findall(r'"([^"\n]*)"|([{}])|([^\s{}"]+)', text)
    tokens = [a or b or c for a, b, c in tokens]
    pos = 0
    def block(nested=False):
        nonlocal pos
        result = []
        while pos < len(tokens):
            key = tokens[pos]; pos += 1
            if key == '}':
                if not nested: raise ValueError('Unexpected KeyValues closing brace')
                return result
            if pos >= len(tokens): raise ValueError('Missing KeyValues value')
            value = tokens[pos]; pos += 1
            if value == '{': value = block(True)
            result.append((key, value))
        if nested: raise ValueError('Unclosed KeyValues block')
        return result
    return block()

def leaves(tree, key):
    result = []
    for name, value in tree:
        if isinstance(value, list): result.extend(leaves(value, key))
        elif name.lower() == key.lower(): result.append(value)
    return result

def lua(value):
    if value is None: return 'nil'
    if isinstance(value, bool): return 'true' if value else 'false'
    if isinstance(value, (int, float)): return repr(value)
    if isinstance(value, str): return json.dumps(value, ensure_ascii=True)
    if isinstance(value, list): return '{' + ','.join(lua(v) for v in value) + '}'
    return '{' + ','.join('['+lua(k)+']='+lua(v) for k,v in value.items()) + '}'

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--vpk-root', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--sourceio', type=Path, default=ROOT/'third_party/SourceIO')
    ap.add_argument('--language', default='english')
    args = ap.parse_args()
    if not re.fullmatch('[a-z]+', args.language): raise ValueError('Invalid language')
    args.output.mkdir(parents=True, exist_ok=True)
    actual = pinned_commit(args.sourceio)
    if actual != SOURCEIO_PIN: raise ValueError(f'SourceIO expected {SOURCEIO_PIN}, got {actual}')
    sys.path.insert(0, str(args.sourceio.resolve().parent))
    from vtf_decode import load_vtf_texture_frames
    from PIL import Image
    import PIL
    index = {}
    for archive in archives(args.vpk_root):
        for name, entry in entries(archive): index[name.lower()] = (archive, name, entry)
    if not index: raise ValueError('No Dark Messiah resources found')
    sources = {}
    def read(name):
        archive, exact, entry = index[name.lower()]
        data = extract(archive, entry)
        sources[exact] = {'archive':archive.name, 'crc32':f'{zlib.crc32(data):08x}', 'sha256':sha(data), 'bytes':len(data)}
        return data
    outputs = {}
    def write(path, data):
        dest = args.output/path; dest.parent.mkdir(parents=True, exist_ok=True); dest.write_bytes(data)
        outputs[path] = {'sha256':sha(data), 'bytes':len(data)}
    sound_data = {}; script_cache = {}
    for category, (script, event) in SOUNDS.items():
        source_script = f'mm/scripts/game_sounds_{script}.txt'
        if script not in script_cache: script_cache[script] = dict(keyvalues(read(source_script).decode('cp1252')))
        tree = script_cache[script][event]
        records = []
        for i, reference in enumerate(leaves(tree, 'wave'), 1):
            relative = re.sub(r'^[*#@~!><^)+?]+', '', reference).replace('%language%', args.language)
            original = 'mm/sound/' + relative.replace('\\','/').lower()
            data = read(original)
            with wave.open(io.BytesIO(data)) as wav:
                if wav.getcomptype() != 'NONE': raise ValueError(f'Unsupported compressed WAV {original}')
                meta = {'channels':wav.getnchannels(), 'sample_rate':wav.getframerate(),
                        'sample_width_bytes':wav.getsampwidth(), 'frames':wav.getnframes(),
                        'duration_seconds':wav.getnframes()/wav.getframerate()}
                pcm = wav.readframes(wav.getnframes())
                if len(pcm) != meta['frames'] * meta['channels'] * meta['sample_width_bytes']:
                    raise ValueError(f'Truncated PCM audio: {original}')
            path = f'sound/brutal_dm/{category}_{i:02d}.wav'; write(path,data)
            records.append({'path':path,'source':original,'source_reference':reference,**meta})
        sound_data[category] = {'event':event,'script':source_script,
            'parameters':{k:v for k,v in tree if isinstance(v,str) and k.lower()!='wave'}, 'files':records}
        print(f'{category}: {len(records)} original WAV variants')
    texture_data = {}
    for category, material in MATERIALS.items():
        material_path = 'mm/materials/'+material+'.vmt'
        raw = read(material_path); text = re.sub(r'//[^\n]*','',raw.decode('cp1252'))
        def prop(name, default=None):
            m = re.search(r'"?'+re.escape(name)+r'"?\s+(?:"([^"]*)"|([^\s{}]+))',text,re.I)
            return (m[1] if m[1] is not None else m[2]) if m else default
        base_texture = prop('$basetexture')
        source_texture = 'mm/materials/'+base_texture.lower()+'.vtf'
        vtf = read(source_texture)
        decoded, width, height, is_float = load_vtf_texture_frames(vtf)
        expected = struct.unpack_from('<H',vtf,24)[0]
        if is_float or len(decoded)!=expected: raise ValueError(f'Unexpected VTF decode layout: {source_texture}')
        frames = []; alpha_min, alpha_max = 255,0
        for i, rgba in enumerate(decoded):
            if len(rgba)!=width*height*4: raise ValueError('Decoded RGBA byte count mismatch')
            image = Image.frombytes('RGBA',(width,height),rgba)
            low,high=image.getchannel('A').getextrema(); alpha_min=min(alpha_min,low);alpha_max=max(alpha_max,high)
            path=f'textures/brutal_dm/{category}'+(f'_{i:02d}' if len(decoded)>1 else '')+'.png'
            buffer=io.BytesIO();image.save(buffer,format='PNG');data=buffer.getvalue()
            if Image.open(io.BytesIO(data)).convert('RGBA').tobytes()!=rgba: raise ValueError('PNG round-trip mismatch')
            write(path,data);frames.append(path)
        fps=prop('animatedtextureframerate')
        texture_data[category]={'material':material_path,'source':source_texture,'width':width,'height':height,
            'frames':frames,'frame_count':len(frames),'fps':float(fps) if fps else None,
            'blend':'additive' if prop('$additive')=='1' else 'alpha',
            'origin':prop('$spriteorigin','[ 0.50 0.50 ]'),
            'orientation':prop('$spriteorientation','unspecified by material'),
            'source_material_properties':{key:prop(key) for key in
                ('$translucent','$additive','$vertexalpha','$vertexcolor','$scale','$rotate',
                 'sineperiod','sinemin','sinemax','rate','initialvalue') if prop(key) is not None},
            'alpha_min':alpha_min,'alpha_max':alpha_max,
            'rgb_note':'Original RGB preserved; spark black background requires additive blending.',
            'pixel_orientation':'VTF decoded rows retained without flip; separate full-image frames, no atlas.'}
        if category == 'adrenaline_ready':
            # A single sampler lets the postprocessing pass reproduce the original
            # AnimatedTexture sequence. Preserve the decoded RGBA and verify every
            # tile, including its transparent/black additive background.
            columns = 5
            rows = (len(decoded) + columns - 1) // columns
            atlas = Image.new('RGBA', (width * columns, height * rows))
            for i, rgba in enumerate(decoded):
                atlas.paste(Image.frombytes('RGBA', (width, height), rgba),
                            ((i % columns) * width, (i // columns) * height))
            buffer = io.BytesIO(); atlas.save(buffer, format='PNG')
            data = buffer.getvalue()
            check = Image.open(io.BytesIO(data)).convert('RGBA')
            for i, rgba in enumerate(decoded):
                x, y = (i % columns) * width, (i // columns) * height
                if check.crop((x, y, x + width, y + height)).tobytes() != rgba:
                    raise ValueError('Adrenaline atlas tile round-trip mismatch')
            atlas_path = 'textures/brutal_dm/adrenaline_ready_atlas.png'
            write(atlas_path, data)
            texture_data[category]['atlas'] = {
                'path': atlas_path, 'columns': columns, 'rows': rows,
                'frame_width': width, 'frame_height': height,
                'frame_count': len(decoded), 'fps': float(fps) if fps else None,
            }
        print(f'{category}: {width}x{height}, {len(frames)} frames, alpha={alpha_min}..{alpha_max}, blend={texture_data[category]["blend"]}')
    manifest={'schema':1,'game':'Dark Messiah of Might and Magic','derived_from_game_files':True,
        'sourceio_commit':actual,'pillow_version':PIL.__version__,'language':args.language,
        'vtf_decoder_sha256':sha((Path(__file__).resolve().parents[2]/'vtf_decode.py').read_bytes()),
        'sources':sources,'outputs':outputs,'sounds':sound_data,'textures':texture_data,
        'emitter_status':'Original material/texture/frame data ported. No portable external emitter definitions found in VPK inventory; native burst emitters are recreated.',
        'adrenaline_release_status':'Original Adrenaline_Reached ready cue found. No specifically named adrenaline-release event found; Slow_Motion_In/Out are distinct source cues, provided without claiming identical adrenaline semantics.'}
    native={category:{'files':[f['path'] for f in item['files']],'event':item['event'],'parameters':item['parameters']}
            for category,item in sound_data.items()}
    payload={'schema':1,'derived_from_game_files':True,'sounds':native,'textures':texture_data}
    write('scripts/brutal_assets/effects.lua',('-- Generated from local user-owned game files. Do not redistribute.\nreturn '+lua(payload)+'\n').encode())
    (args.output/'effects-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(f'Imported {len(outputs)} outputs; manifest: {args.output}/effects-manifest.json')

if __name__=='__main__':main()
