"""Read individual files from a Morrowind (TES3) BSA archive.

Layout: a 12-byte header (version 0x100, hash table offset, file count), then
per-file size/offset pairs, name offsets, the name block, the hash table and
the file data. Offsets of files are relative to the end of the hash table.
"""
from pathlib import Path
import struct


class BsaError(Exception):
    pass


class Bsa:
    def __init__(self, path):
        self.path = Path(path)
        with self.path.open('rb') as archive:
            version, hash_offset, count = struct.unpack('<3I', archive.read(12))
            if version != 0x100:
                raise BsaError(f'{self.path} is not a Morrowind BSA archive')
            records = struct.unpack(f'<{2 * count}I', archive.read(8 * count))
            name_offsets = struct.unpack(f'<{count}I', archive.read(4 * count))
            names = archive.read(hash_offset - 12 * count)
        self.data_start = 12 + hash_offset + 8 * count
        self.files = {}
        for index, start in enumerate(name_offsets):
            name = names[start:names.index(b'\0', start)].decode('latin-1')
            self.files[name.lower().replace('/', '\\')] = (records[2 * index], records[2 * index + 1])

    def __contains__(self, name):
        return name.lower().replace('/', '\\') in self.files

    def read(self, name):
        key = name.lower().replace('/', '\\')
        if key not in self.files:
            raise BsaError(f'{self.path.name} does not contain {name}')
        size, offset = self.files[key]
        with self.path.open('rb') as archive:
            archive.seek(self.data_start + offset)
            data = archive.read(size)
        if len(data) != size:
            raise BsaError(f'{self.path.name} is truncated at {name}')
        return data

    def extract(self, name, root):
        """Write `name` below `root`, keeping its archive path, and return the new file."""
        target = Path(root).joinpath(*name.replace('\\', '/').split('/'))
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(self.read(name))
        return target
