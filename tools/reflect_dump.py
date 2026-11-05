"""Static dump of Enshrouded's reflection table from enshrouded.exe.

Layout facts: docs/reflection-format.md. Reads a copy of the exe, never the game folder.
Usage: python -I tools/reflect_dump.py <exe> [out.json]
"""
import json, struct, sys
from collections import Counter

import pefile

STRUCT = 0x12


class Img:
    def __init__(self, path):
        self.pe = pefile.PE(path, fast_load=True)
        self.raw = open(path, 'rb').read()
        self.ib = self.pe.OPTIONAL_HEADER.ImageBase
        self.secs = {s.Name.rstrip(b'\0').decode(): s for s in self.pe.sections}

    def fo(self, va):
        rva = va - self.ib
        for s in self.pe.sections:
            if s.VirtualAddress <= rva < s.VirtualAddress + s.SizeOfRawData:
                return s.PointerToRawData + rva - s.VirtualAddress
        raise ValueError(f'va {va:#x} not in file')

    def va(self, fo):
        for s in self.pe.sections:
            if s.PointerToRawData <= fo < s.PointerToRawData + s.SizeOfRawData:
                return self.ib + s.VirtualAddress + fo - s.PointerToRawData
        raise ValueError(f'fo {fo:#x} not in a section')

    def q(self, va, off=0):
        return struct.unpack_from('<Q', self.raw, self.fo(va) + off)[0]

    def str(self, ptr, ln):
        return self.raw[self.fo(ptr):self.fo(ptr) + ln].decode(errors='replace') if ptr else ''

    def in_sec(self, va, name):
        s = self.secs[name]
        return 0 <= va - self.ib - s.VirtualAddress < s.Misc_VirtualSize

    def find_ptr(self, target, start):
        # first 8-aligned qword equal to target, at or after file offset `start`
        needle, i = struct.pack('<Q', target), start
        while (i := self.raw.find(needle, i)) >= 0:
            if i % 8 == 0:
                return i
            i += 1
        raise LookupError(f'no pointer to {target:#x}')


def find_table(img):
    # The anchors: the "BlobString" and "uint32" descriptors are the first
    # two table entries, and a {types*, count} header in .rdata points at the table.
    rd = img.secs['.rdata'].PointerToRawData
    tms = [img.va(img.find_ptr(img.va(img.raw.find(s, rd) + 1), rd))
           for s in (b'\0BlobString\0', b'\0uint32\0')]
    arr = img.raw.find(struct.pack('<QQ', *tms), rd)
    hdr = img.find_ptr(img.va(arr), rd)
    tbl, cnt = struct.unpack_from('<QQ', img.raw, hdr)
    return img.va(hdr), tbl, cnt


def read_ns(img, p):
    parts = []
    while p:
        parts.append(img.str(img.q(p), img.q(p, 8)))
        p = img.q(p, 0x10)
    return '::'.join(reversed(parts))


def read_type(img, p):
    b = img.fo(p)
    size, align, ealign, fc, prim, flags = struct.unpack_from('<IHHIBB', img.raw, b + 0x40)
    qh, ih = struct.unpack_from('<II', img.raw, b + 0x50)
    t = dict(name=img.str(img.q(p), img.q(p, 8)), qualified_name=img.str(img.q(p, 0x20), img.q(p, 0x28)),
             impact_name=img.str(img.q(p, 0x10), img.q(p, 0x18)), namespace=read_ns(img, img.q(p, 0x30)),
             inner=img.q(p, 0x38), size=size, align=align, elem_align=ealign, prim=prim, flags=flags,
             qualified_hash=qh, internal_hash=ih, fields=[], enum=[])
    fa, ea = img.q(p, 0x58), img.q(p, 0x60)
    for i in range(fc if fa else 0):
        f = fa + 0x30 * i
        t['fields'].append(dict(name=img.str(img.q(f), img.q(f, 8)), type=img.q(f, 0x10), offset=img.q(f, 0x18)))
    for i in range(fc if ea else 0):
        e = ea + 0x28 * i
        t['enum'].append(dict(name=img.str(img.q(e), img.q(e, 8)), value=img.q(e, 0x10)))
    return t


def dump(path):
    img = Img(path)
    hdr, tbl, cnt = find_table(img)
    ptrs = [img.q(tbl, 8 * i) for i in range(cnt)]
    types = [read_type(img, p) for p in ptrs]
    by_ptr = {p: t['qualified_name'] for p, t in zip(ptrs, types)}
    for t in types:  # type refs are descriptor pointers; resolve them to names
        t['inner'] = by_ptr.get(t['inner'])
        for f in t['fields']:
            f['type'] = by_ptr.get(f['type'])
    text_refs = sum(1 for p in ptrs for o in range(0, 0x90, 8) if img.in_sec(img.q(p, o), '.text'))
    return dict(header_va=hdr, table_va=tbl, count=cnt, text_refs=text_refs, types=types)


def fnv(s):
    h = 0x811c9dc5
    for c in s.encode():
        h = ((h ^ c) * 0x1000193) & 0xffffffff
    return h


def base_root(by_name, t):
    seen = 0
    while t['prim'] == STRUCT and t['inner'] in by_name and seen < 32:
        t, seen = by_name[t['inner']], seen + 1
    return t['qualified_name'] if seen else None


def stats(d):
    ts = d['types']
    by_name = {t['qualified_name']: t for t in ts}
    runtime = [t for t in ts if t['prim'] == STRUCT and not t['qualified_name'].startswith('keen::ds::')]
    roots = Counter(base_root(by_name, t) for t in runtime)
    return dict(types=d['count'], structs=sum(t['prim'] == STRUCT for t in ts),
                desc_qwords_into_text=d['text_refs'],
                hash_is_fnv=sum(t['qualified_hash'] == fnv(t['qualified_name']) for t in ts),
                ecs_components=roots['keen::ecs::Component'], ecs_events=roots['keen::ecs::Event'],
                impact_nodes=roots['keen::impact_nodes::ImpactNode'],
                has_ds=sum(t['flags'] & 1 for t in ts),
                has_ds_named_ds=sum(t['flags'] & 1 and t['qualified_name'].startswith('keen::ds::') for t in ts))


def check(d, st):
    by_name = {t['qualified_name']: t for t in d['types']}
    hp = by_name['keen::ecs::Health']
    assert st['types'] > 10_000, st
    assert st['hash_is_fnv'] == st['types'], 'qualified_hash is no longer FNV-1a of qualified_name'
    assert hp['size'] == 0x28 and hp['inner'] == 'keen::ecs::SInt32Attribute', hp
    assert hp['fields'] == [dict(name='dataStorage', type='keen::StaticArray<keen::sint32,8>', offset=8)], hp


if __name__ == '__main__':
    d = dump(sys.argv[1])
    st = stats(d)
    check(d, st)
    print(f"header {d['header_va']:#x}  table {d['table_va']:#x}")
    for k, v in st.items():
        print(f'{k:24} {v}')
    if len(sys.argv) > 2:
        with open(sys.argv[2], 'w') as f:
            json.dump(d, f, indent=1)
        print('wrote', sys.argv[2])
