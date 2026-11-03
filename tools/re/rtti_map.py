#!/usr/bin/env python3
"""Map every MSVC RTTI class in a 32-bit exe to its vtable(s) and base classes.

    python3 tools/re/rtti_map.py "<path>/Fable.exe" tools/re-notes/rtti.json

Works on Fable.exe because it has no ASLR: every address printed is the address
the class lives at at run time (image base 0x400000).
"""
import json, re, struct, sys
import pefile

exe, out = sys.argv[1], sys.argv[2]
pe = pefile.PE(exe, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
u32 = lambda va: struct.unpack_from('<I', img, va - base)[0]

def sec(name):
    for s in pe.sections:
        if s.Name.rstrip(b'\0') == name:
            return base + s.VirtualAddress, base + s.VirtualAddress + s.Misc_VirtualSize
    raise SystemExit(f'no section {name}')
text = sec(b'.text')
data_ranges = [sec(b'.rdata'), sec(b'.data')]
in_text = lambda v: text[0] <= v < text[1]

def demangle(n):
    # .?AVCFoo@NBar@@ -> NBar::CFoo (enough for search; not full MSVC demangling)
    parts = n[4:-2].split('@')
    return '::'.join(reversed([p for p in parts if p]))

# 1. TypeDescriptors: "?.AV" names, TD = name - 8
tds = {}
for m in re.finditer(rb'\.\?A[VU][A-Za-z0-9_@$?]{2,200}?@@\x00', img):
    va = base + m.start() - 8
    tds[va] = m.group(0)[:-1].decode()

# 2. Complete Object Locators: sig 0, pTypeDescriptor at +12
cols = {}
for lo, hi in data_ranges:
    for va in range(lo, hi - 20, 4):
        if u32(va) != 0:
            continue
        td = u32(va + 12)
        if td in tds:
            cols[va] = dict(td=td, offset=u32(va + 4), chd=u32(va + 16))

# 3. vtables: a pointer to a COL, followed by a code pointer
classes = {}
for lo, hi in data_ranges:
    for va in range(lo, hi - 8, 4):
        col = u32(va)
        if col in cols and in_text(u32(va + 4)):
            vt = va + 4
            n = 0
            while vt + 4 * n < hi and in_text(u32(vt + 4 * n)) and n < 2000:
                n += 1
            c = cols[col]
            name = tds[c['td']]
            entry = classes.setdefault(name, dict(name=demangle(name), mangled=name, vtables=[], bases=[]))
            entry['vtables'].append(dict(va=f'0x{vt:08X}', subobject_offset=c['offset'], slots=n))
            # Class Hierarchy Descriptor -> base class array
            try:
                chd = c['chd']
                nb, arr = u32(chd + 8), u32(chd + 12)
                bases = []
                for i in range(1, min(nb, 64)):
                    bcd = u32(arr + 4 * i)
                    t = u32(bcd)
                    if t in tds:
                        bases.append(demangle(tds[t]))
                entry['bases'] = bases
            except Exception:
                pass

result = sorted(classes.values(), key=lambda c: c['name'])
json.dump(dict(exe=exe.split('/')[-1], image_base=f'0x{base:08X}', classes=result), open(out, 'w'), indent=1)
print(f'{len(tds)} type names, {len(cols)} object locators, {len(result)} classes with vtables -> {out}')
