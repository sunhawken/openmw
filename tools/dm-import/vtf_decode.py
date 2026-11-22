"""Decode Dark Messiah's VTF textures (versions 7.0-7.2, DXT1 and DXT5) to RGBA bytes.

Pure numpy. 565 colours expand by bit replication, colour interpolation
truncates, DXT5 alpha interpolation rounds to nearest.
"""
import struct
import numpy as np

DXT1, DXT5 = 13, 15
BLOCK_BYTES = {DXT1: 8, DXT5: 16}


def expand565(c, mode):
    r = (c >> 11) & 31; g = (c >> 5) & 63; b = c & 31
    if mode == 'shift':
        return ((r << 3) | (r >> 2)), ((g << 2) | (g >> 4)), ((b << 3) | (b >> 2))
    return (r * 255 + 15) // 31, (g * 255 + 31) // 63, (b * 255 + 15) // 31


def mix(a, b, wa, wb, d, mode):
    if mode == 'floor':
        return (wa * a + wb * b) // d
    return (wa * a + wb * b + d // 2) // d


def decode_dxt(data, width, height, fmt, expand='shift', rounding='floor', alpha_rounding='round'):
    bw, bh = max(1, (width + 3) // 4), max(1, (height + 3) // 4)
    size = BLOCK_BYTES[fmt]
    blocks = np.frombuffer(data, dtype=np.uint8, count=bw * bh * size).reshape(bh * bw, size).astype(np.int32)
    color = blocks[:, -8:]
    c0 = color[:, 0] | (color[:, 1] << 8); c1 = color[:, 2] | (color[:, 3] << 8)
    bits = color[:, 4] | (color[:, 5] << 8) | (color[:, 6] << 16) | (color[:, 7] << 24)
    r0, g0, b0 = expand565(c0, expand); r1, g1, b1 = expand565(c1, expand)
    palette = np.zeros((len(blocks), 4, 4), np.int32)
    palette[:, 0, :3] = np.stack([r0, g0, b0], 1); palette[:, 1, :3] = np.stack([r1, g1, b1], 1)
    palette[:, :, 3] = 255
    four = (c0 > c1) | (fmt != DXT1)
    p0, p1 = palette[:, 0, :3], palette[:, 1, :3]
    palette[:, 2, :3] = np.where(four[:, None], mix(p0, p1, 2, 1, 3, rounding), mix(p0, p1, 1, 1, 2, rounding))
    palette[:, 3, :3] = np.where(four[:, None], mix(p0, p1, 1, 2, 3, rounding), 0)
    if fmt == DXT1:
        palette[:, 3, 3] = np.where(four, 255, 0)
    index = (bits[:, None] >> (2 * np.arange(16))) & 3
    pixels = np.take_along_axis(palette, index[:, :, None].repeat(4, 2), 1)
    if fmt == DXT5:
        a0, a1 = blocks[:, 0], blocks[:, 1]
        abits = np.zeros(len(blocks), np.int64)
        for i in range(6):
            abits |= blocks[:, 2 + i].astype(np.int64) << (8 * i)
        alphas = np.zeros((len(blocks), 8), np.int32)
        alphas[:, 0], alphas[:, 1] = a0, a1
        eight = a0 > a1
        for i in range(1, 7):
            alphas[:, i + 1] = np.where(eight, mix(a0, a1, 7 - i, i, 7, alpha_rounding), alphas[:, i + 1])
        for i in range(1, 5):
            alphas[:, i + 1] = np.where(~eight, mix(a0, a1, 5 - i, i, 5, alpha_rounding), alphas[:, i + 1])
        alphas[:, 6] = np.where(~eight, 0, alphas[:, 6]); alphas[:, 7] = np.where(~eight, 255, alphas[:, 7])
        aindex = ((abits[:, None] >> (3 * np.arange(16))) & 7).astype(np.int64)
        pixels[:, :, 3] = np.take_along_axis(alphas, aindex, 1)
    image = pixels.reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)
    return image[:height, :width].astype(np.uint8).tobytes()


def mip_bytes(width, height, fmt):
    return max(1, (width + 3) // 4) * max(1, (height + 3) // 4) * BLOCK_BYTES[fmt]


def load_vtf_texture_frames(vtf, **options):
    """Largest mip of every frame: ([rgba bytes, ...], width, height, is_float)."""
    major, minor, header = struct.unpack_from('<III', vtf, 4)
    width, height, flags, frames = struct.unpack_from('<HHIH', vtf, 16)
    fmt = struct.unpack_from('<i', vtf, 52)[0]
    mips = vtf[56]
    low_fmt = struct.unpack_from('<i', vtf, 57)[0]
    low_w, low_h = vtf[61], vtf[62]
    depth = struct.unpack_from('<H', vtf, 63)[0] if (major, minor) >= (7, 2) else 1
    if (major, minor) > (7, 2) or fmt not in BLOCK_BYTES or depth > 1 or flags & 0x4000:
        raise ValueError(f'Unsupported VTF {major}.{minor} format {fmt} depth {depth} flags {flags:#x}')
    offset = header + (mip_bytes(low_w, low_h, low_fmt) if low_fmt in BLOCK_BYTES and low_w and low_h else 0)
    for mip in range(mips - 1, 0, -1):
        offset += mip_bytes(max(1, width >> mip), max(1, height >> mip), fmt) * frames
    size = mip_bytes(width, height, fmt)
    if offset + size * frames != len(vtf):
        raise ValueError(f'VTF size mismatch: {offset + size * frames} != {len(vtf)}')
    return [decode_dxt(vtf[offset + i * size:offset + (i + 1) * size], width, height, fmt, **options)
            for i in range(frames)], width, height, False


def load_vtf_texture(vtf):
    """Largest mip of the first frame: (rgba bytes, width, height, is_float)."""
    frames, width, height, is_float = load_vtf_texture_frames(vtf)
    return frames[0], width, height, is_float
