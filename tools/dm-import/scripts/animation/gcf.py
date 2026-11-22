"""Read the unencrypted GCF format 6 files used by retail Dark Messiah installations.

Only metadata is cached; file data is read on demand, including fragmented files.
The layout and checksum algorithm are documented by HLLib's GCF reader:
https://github.com/mittorn/hllib/blob/master/HLLib/GCFFile.h
"""
from functools import lru_cache
from pathlib import Path
import struct
import zlib


class Gcf:
    def __init__(self, path):
        self.path = Path(path)
        size = self.path.stat().st_size
        with self.path.open('rb') as stream:
            def read(length):
                if length < 0 or length > size - stream.tell():
                    raise ValueError(f'Truncated GCF metadata: {path}')
                data = stream.read(length)
                if len(data) != length:
                    raise ValueError(f'Truncated GCF metadata: {path}')
                return data

            def words(count):
                return struct.unpack('<' + 'I' * count, read(count * 4))

            header = words(11)
            if header[:3] != (1, 1, 6):
                raise ValueError(f'Expected GCF version 6: {path}')
            block_header = words(8)
            self.block_count = block_header[0]
            self.blocks = read(self.block_count * 28)
            fragments = words(4)
            if fragments[0] != self.block_count or fragments[2] not in (0, 1):
                raise ValueError(f'Invalid GCF fragmentation header: {path}')
            self.fragments = read(fragments[0] * 4)
            directory_start = stream.tell()
            directory = words(14)
            self.item_count = directory[3]
            self.directory = read(self.item_count * 28)
            self.names = read(directory[7])
            remaining = directory_start + directory[6] - stream.tell()
            read(remaining)
            words(2)  # directory map header
            self.mapping = read(self.item_count * 4)
            checksum_header = words(2)
            self.checksums = read(checksum_header[1])
            if len(self.checksums) < 16:
                raise ValueError(f'Truncated GCF checksum header: {path}')
            magic, version, self.checksum_items, self.checksum_count = struct.unpack_from('<4I', self.checksums)
            if (magic, version) != (0x14893721, 1):
                raise ValueError(f'Invalid GCF checksum header: {path}')
            if 16 + self.checksum_items * 8 + self.checksum_count * 4 > len(self.checksums):
                raise ValueError(f'Truncated GCF checksums: {path}')
            data_header = words(6)
            _, count, self.block_size, self.data_offset, _, _ = data_header
            if count != self.block_count or self.block_size != header[8] or not self.block_size:
                raise ValueError(f'Invalid GCF data header: {path}')
            if self.data_offset < stream.tell() or self.data_offset > size:
                raise ValueError(f'Invalid GCF data offset: {path}')

    def item(self, index):
        if index >= self.item_count:
            raise ValueError(f'Invalid GCF directory index: {self.path}')
        return struct.unpack_from('<7I', self.directory, index * 28)

    def entries(self):
        paths = {0xffffffff: ''}

        def name(index):
            pending, seen = [], set()
            while index not in paths:
                if index in seen:
                    raise ValueError(f'Cyclic GCF directory: {self.path}')
                seen.add(index)
                item = self.item(index)
                offset = item[0]
                end = self.names.find(b'\0', offset)
                if end < 0 or offset >= len(self.names):
                    raise ValueError(f'Invalid GCF filename: {self.path}')
                part = self.names[offset:end].decode('cp1252')
                if part in ('.', '..') or '/' in part or '\\' in part or ':' in part:
                    raise ValueError(f'Unsafe GCF filename: {self.path}')
                pending.append((index, part))
                index = item[4]
            prefix = paths[index]
            for index, part in reversed(pending):
                prefix = '/'.join(p for p in (prefix, part) if p)
                paths[index] = prefix
            return prefix

        for index in range(self.item_count):
            if self.item(index)[3] & 0x4000:
                yield name(index), index

    def extract(self, index):
        _, length, checksum, flags, _, _, _ = self.item(index)
        if flags & 0x100:
            raise ValueError(f'Encrypted GCF entry cannot be imported: {self.path}')
        if length > self.path.stat().st_size:
            raise ValueError(f'Invalid GCF file size: {self.path}')
        result, seen, written = bytearray(length), set(), 0
        block = struct.unpack_from('<I', self.mapping, index * 4)[0]
        with self.path.open('rb') as stream:
            while block != self.block_count:
                if block >= self.block_count or block in seen:
                    raise ValueError(f'Invalid GCF block chain: {self.path}')
                seen.add(block)
                _, offset, count, fragment, next_block, _, owner = struct.unpack_from('<7I', self.blocks, block * 28)
                if owner != index or offset != written or offset + count > length or not count:
                    raise ValueError(f'Incomplete GCF entry: {self.path}')
                fragments = set()
                remaining = count
                while remaining:
                    if fragment >= self.block_count or fragment in fragments:
                        raise ValueError(f'Invalid GCF fragment chain: {self.path}')
                    fragments.add(fragment)
                    amount = min(remaining, self.block_size)
                    stream.seek(self.data_offset + fragment * self.block_size)
                    chunk = stream.read(amount)
                    if len(chunk) != amount:
                        raise ValueError(f'Truncated GCF data: {self.path}')
                    result[written:written + amount] = chunk
                    written += amount
                    remaining -= amount
                    fragment = struct.unpack_from('<I', self.fragments, fragment * 4)[0]
                block = next_block
        if written != length:
            raise ValueError(f'Incomplete GCF entry: {self.path}')
        if checksum != 0xffffffff and length:
            if checksum >= self.checksum_items:
                raise ValueError(f'Invalid GCF checksum index: {self.path}')
            count, first = struct.unpack_from('<2I', self.checksums, 16 + checksum * 8)
            if count != (length + 32767) // 32768 or first + count > self.checksum_count:
                raise ValueError(f'Incomplete GCF checksums: {self.path}')
            for part, offset in enumerate(range(0, length, 32768)):
                chunk = result[offset:offset + 32768]
                actual = zlib.adler32(chunk, 0) ^ zlib.crc32(chunk)
                position = 16 + self.checksum_items * 8 + (first + part) * 4
                expected = struct.unpack_from('<I', self.checksums, position)[0]
                if actual != expected:
                    raise ValueError(f'GCF checksum mismatch: {self.path}, entry {index}')
        return bytes(result)


@lru_cache(maxsize=16)
def open_gcf(path):
    return Gcf(path)
