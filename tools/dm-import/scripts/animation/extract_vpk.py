#!/usr/bin/env python3
"""List or selectively extract Valve VPK v1/v2 assets, checking CRC32."""
import argparse
import os
import time
from pathlib import Path, PurePosixPath
import struct
import zlib


def entries(path):
    data=path.read_bytes();magic,version,size=struct.unpack_from('<III',data)
    if magic!=0x55aa1234 or version not in (1,2):raise ValueError('Not VPK v1/v2')
    start=12 if version==1 else 28;pos=start
    def string():
        nonlocal pos
        end=data.index(b'\0',pos);result=data[pos:end].decode('utf8');pos=end+1;return result
    while ext:=string():
        while folder:=string():
            while name:=string():
                crc,preload,index,offset,length,term=struct.unpack_from('<IHHIIH',data,pos);pos+=18
                if term!=0xffff:raise ValueError('Invalid VPK entry terminator')
                prefix=data[pos:pos+preload];pos+=preload
                filename='/'.join(x for x in [folder if folder!=' ' else '',name+('.'+ext if ext!=' ' else '')] if x)
                yield filename,(crc,index,offset,length,prefix,start+size)


def extract(path,entry):
    crc,index,offset,length,prefix,tree_end=entry
    archive=path if index==0x7fff else path.with_name(path.stem.removesuffix('_dir')+f'_{index:03d}.vpk')
    with archive.open('rb') as f:
        f.seek(offset+(tree_end if index==0x7fff else 0));data=prefix+f.read(length)
    if zlib.crc32(data)!=crc:raise ValueError(f'CRC32 mismatch: {archive}')
    return data


def store(path,data):
    """Write a cache file atomically, so steps running side by side never read half of it.

    Steps that need the same archive entry may extract it at the same time. On
    Windows an existing cache file is never replaced: replacing deletes the old
    file, and a step opening it at that moment gets "access denied". The first
    copy wins; later identical copies are dropped.
    """
    temporary=path.with_name(f'{path.name}.{os.getpid()}.part')
    temporary.write_bytes(data)
    for attempt in range(100):
        try:
            if os.name=='nt':
                os.rename(temporary,path)
            else:
                os.replace(temporary,path)
            return
        except FileExistsError:
            temporary.unlink();return
        except PermissionError:
            # A virus scanner may still hold the new file; try again shortly.
            time.sleep(.1)
    os.rename(temporary,path)


def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('vpk',type=Path);ap.add_argument('--contains',default='');ap.add_argument('--out',type=Path)
    args=ap.parse_args()
    for name,entry in entries(args.vpk):
        if args.contains.lower() not in name.lower():continue
        if args.out:
            parts=PurePosixPath(name).parts
            if '..' in parts or name.startswith('/'):raise ValueError('Unsafe archive path')
            target=args.out.joinpath(*parts);target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(extract(args.vpk,entry))
        print(name)

if __name__=='__main__':main()
