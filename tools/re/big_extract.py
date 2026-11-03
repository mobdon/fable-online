"""Extract textures from a Fable: The Lost Chapters graphics .big bank (e.g. frontend.big).

Reads the player's own install. Never commit the output: it is Lionhead's art.

Usage: python big_extract.py <file.big> <out_dir> [name_filter]

Format (worked out from frontend.big, version 0x64):
  header: 'BIGB', u32 version, u32 bank_index_offset, u32 ?
  bank index: u32 bank_count, then per bank: cstring name, u32 id, u32 entry_count,
              u32 entries_offset, u32 entries_size, ...
  entries table: u32 ?, u32 ?, u32 count?, then per entry:
     u32 magic(0x2a), u32 id, u32 type, u32 data_size, u32 data_offset, u32 crc,
     u32 name_len, name, u32 crc, u32 source_count, (u32 len, path) * source_count,
     u32 subheader_len, subheader
  texture subheader: u16 width, u16 height, u16 depth, u16 frame_w, u16 frame_h,
     u16 frames, u16 format, u16 ?, u8 alpha, u8 mips, u16 ?, u32 mip0_size, u32 mip0_compressed
  data: u16 chunk_len (0xFFFF -> u32 follows), then LZO1X data that inflates to mip0_size.
     DXT1 when mip0_size == w*h/2, DXT3/5 when == w*h, else raw 32-bit.
"""
import struct, sys, os
from io import BytesIO
import lzallright
from PIL import Image

def dds(w, h, fourcc, data):
    hdr = struct.pack('<4sIIIIIII44s', b'DDS ', 124, 0x81007, h, w, len(data), 0, 1, b'\0'*44)
    hdr += struct.pack('<II4sIIIII', 32, 4, fourcc, 0, 0, 0, 0, 0)
    hdr += struct.pack('<IIIII', 0x1000, 0, 0, 0, 0)
    return Image.open(BytesIO(hdr + data))

def main(path, out, filt=''):
    d = open(path, 'rb').read()
    assert d[:4] == b'BIGB'
    bank_index = struct.unpack_from('<I', d, 8)[0]
    p = bank_index + 4
    name_end = d.index(b'\0', p)
    p = name_end + 1
    _id, count, eoff, esize = struct.unpack_from('<IIII', d, p)
    p = eoff + 12
    lzo = lzallright.LZOCompressor()
    os.makedirs(out, exist_ok=True)
    for _ in range(count):
        magic, eid, etype, size, off, crc, nlen = struct.unpack_from('<7I', d, p); p += 28
        name = d[p:p+nlen].decode(); p += nlen
        crc2, nsrc = struct.unpack_from('<II', d, p); p += 8
        for _s in range(nsrc):
            l = struct.unpack_from('<I', d, p)[0]; p += 4 + l
        shl = struct.unpack_from('<I', d, p)[0]; p += 4
        sh = d[p:p+shl]; p += shl
        if filt and filt not in name:
            continue
        try:
            w, h, depth, fw, fh, frames, fmt, _u, alpha, mips, _u2, msize, mcomp = struct.unpack_from('<8H2BH2I', sh)
            q = off
            cl = struct.unpack_from('<H', d, q)[0]; q += 2
            if cl == 0xFFFF:
                cl = struct.unpack_from('<I', d, q)[0]; q += 4
            raw = d[q:q+cl] if mcomp != msize else d[off:off+msize]
            if mcomp != msize:
                # The decoder stops 3 bytes short of mip0_size; pad (it's the last DXT block).
                raw = lzo.decompress(raw, output_size_hint=msize).ljust(msize, b'\0')
            frame_h = h // max(frames, 1) if frames > 1 else h
            if msize == w*h//2*max(frames,1) or msize == w*h//2:
                im = dds(w, len(raw)*2//w, b'DXT1', raw)
            elif msize in (w*h, w*h*max(frames,1)):
                im = dds(w, len(raw)//w, b'DXT5' if alpha else b'DXT3', raw)
            else:
                im = Image.frombytes('RGBA', (w, len(raw)//(w*4)), raw, 'raw', 'BGRA')
            if fw and fh and fw <= im.width and fh <= im.height:
                im = im.crop((0, 0, fw, fh))
            im.save(os.path.join(out, name + '.png'))
            print(f'{name}: {w}x{h} frame {fw}x{fh} x{frames} fmt {fmt} alpha {alpha}')
        except Exception as e:
            print(f'{name}: skipped ({e})')

if __name__ == '__main__':
    main(*sys.argv[1:])
