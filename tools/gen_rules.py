#!/usr/bin/env python3
"""Generate a preloader signature rule for a Minecraft Bedrock build.

The game classes export no symbols, so every native seam the preloader uses is
reached by RTTI: find the standalone type-name string in .rodata, locate its
typeinfo struct in .data.rel.ro (the word before the typeinfo pointer is the
vtable's offset-to-top, zero for a primary vtable), then read the slot. The
game binary also strips every method name, so a byte pattern in .text drifts
between builds while the RTTI seams survive -- this tool derives the RTTI part
of a rule, which is the part that does not need a fresh disassembly pass.

Offline, .data.rel.ro still holds the pre-relocation zeros: the loader applies
the R_AARCH64_RELATIVE addends at load time. This tool rebuilds them first, so a
static read matches what the process sees. That is the whole trick, and it is
why a plain byte scan for the typeinfo pointer finds nothing.

Usage:
    gen_rules.py --apk <file-or-url> --min 1.26.60.28 [--max 1.26.60.28]
                 [--out rule.json]

The output is a single rule object (the shape the launcher's rules file uses),
with the seams that resolved and a "probes" block naming what each one proved.
A seam that did not resolve is reported and omitted rather than guessed: an
unresolved slot must stay absent so the runtime fails closed.
"""
import argparse
import json
import os
import struct
import sys
import urllib.request
import zipfile

# The seams the preloader resolves by RTTI name. (type name, slot, rule key,
# what a successful resolution proves.) The slot is the *current* verified
# index; a build that moved it shows up as a mismatch against the live probe.
SEAMS = [
    ("18LivePlayerRenderer", 17, "playerRenderVtableIndex",
     "the player-render hook entry"),
    ("14ClientInstance", 31, None,
     "the local-player accessor (position/rotation feed)"),
    ("20ClientNetworkHandler", 40, "packetReadVtableIndex",
     "the player-join packet handler"),
]


class Elf:
    def __init__(self, path):
        self.d = bytearray(open(path, 'rb').read())
        d = self.d
        assert d[:4] == b'\x7fELF', 'not an ELF file'
        e_shoff = struct.unpack_from('<Q', d, 0x28)[0]
        e_shentsize = struct.unpack_from('<H', d, 0x3A)[0]
        e_shnum = struct.unpack_from('<H', d, 0x3C)[0]
        e_shstrndx = struct.unpack_from('<H', d, 0x3E)[0]
        self.secs = []
        for i in range(e_shnum):
            o = e_shoff + i * e_shentsize
            name, typ, flags, addr, off, size, link, info, align, entsize = \
                struct.unpack_from('<IIQQQQIIQQ', d, o)
            self.secs.append({'name': name, 'type': typ, 'addr': addr, 'off': off,
                              'size': size})
        shstr = self.secs[e_shstrndx]
        for s in self.secs:
            o = shstr['off'] + s['name']
            e = d.index(b'\x00', o)
            s['nm'] = d[o:e].decode()
        self._apply_relocations()

    def sec(self, nm):
        for s in self.secs:
            if s['nm'] == nm:
                return s
        raise KeyError(nm)

    def _apply_relocations(self):
        try:
            rela = self.sec('.rela.dyn')
        except KeyError:
            return
        d = self.d
        targets = sorted((s for s in self.secs if s['type'] == 1 and s['addr']),
                         key=lambda s: s['addr'])
        n = rela['size'] // 24
        for i in range(n):
            o = rela['off'] + i * 24
            r_offset, r_info, r_addend = struct.unpack_from('<QQq', d, o)
            if (r_info & 0xFFFFFFFF) != 1027:  # R_AARCH64_RELATIVE
                continue
            for s in targets:
                if s['addr'] <= r_offset < s['addr'] + s['size']:
                    fo = s['off'] + (r_offset - s['addr'])
                    struct.pack_into('<Q', d, fo, r_addend & 0xFFFFFFFFFFFFFFFF)
                    break

    def read_u64(self, vaddr):
        for s in self.secs:
            if s['addr'] <= vaddr < s['addr'] + s['size']:
                fo = s['off'] + (vaddr - s['addr'])
                return struct.unpack_from('<Q', self.d, fo)[0]
        return None

    def find_str(self, s):
        rd = self.sec('.rodata')
        b = s.encode()
        o = self.d.find(b, rd['off'], rd['off'] + rd['size'])
        if o < 0:
            return None
        return rd['addr'] + (o - rd['off'])

    def typeinfo_struct(self, name):
        str_addr = self.find_str(name)
        if str_addr is None:
            return None
        dr = self.sec('.data.rel.ro')
        n = dr['size'] // 8
        for i in range(1, n):
            if self.read_u64(dr['addr'] + i * 8) == str_addr:
                return dr['addr'] + (i - 1) * 8
        return None

    def vtable_address_points(self, name):
        ti = self.typeinfo_struct(name)
        if ti is None:
            return []
        dr = self.sec('.data.rel.ro')
        out = []
        n = dr['size'] // 8
        for i in range(1, n):
            if self.read_u64(dr['addr'] + i * 8) != ti:
                continue
            if self.read_u64(dr['addr'] + (i - 1) * 8) != 0:  # offset-to-top
                continue
            out.append((ti, dr['addr'] + (i + 1) * 8))
        return out

    def is_executable(self, vaddr):
        if vaddr is None:
            return False
        t = self.sec('.text')
        return t['addr'] <= vaddr < t['addr'] + t['size']


def fetch_apk(source, cache_dir):
    if not source.startswith(('http://', 'https://')):
        return source
    os.makedirs(cache_dir, exist_ok=True)
    dest = os.path.join(cache_dir, os.path.basename(source) or 'package.apk')
    if os.path.exists(dest) and os.path.getsize(dest) > 0:
        return dest
    print(f'[gen_rules] downloading {source}', file=sys.stderr)
    urllib.request.urlretrieve(source, dest)
    return dest


def extract_so(apk, dest):
    with zipfile.ZipFile(apk) as z:
        name = next((n for n in z.namelist()
                     if n.endswith('lib/arm64-v8a/libminecraftpe.so')), None)
        if name is None:
            raise SystemExit('no lib/arm64-v8a/libminecraftpe.so in the package')
        with z.open(name) as src, open(dest, 'wb') as out:
            out.write(src.read())
    return dest


def build_rule(elf, min_ver, max_ver):
    rule = {"min": min_ver, "max": max_ver}
    probes = {}
    for type_name, slot, key, what in SEAMS:
        aps = elf.vtable_address_points(type_name)
        resolved = None
        for _, ap in aps:
            v = elf.read_u64(ap + slot * 8)
            if elf.is_executable(v):
                resolved = v
                break
        label = key or f"{type_name} slot {slot}"
        if resolved is None:
            # Omitted on purpose: an unresolved seam must stay absent so the
            # runtime fails closed instead of detouring a data pointer.
            probes[label] = {
                "resolved": False,
                "reason": "no primary vtable slot resolved to executable .text",
            }
            print(f'[gen_rules] {label}: NOT RESOLVED (omitted)', file=sys.stderr)
            continue
        if key:
            rule[key] = slot
        probes[label] = {
            "resolved": True,
            "address": hex(resolved),
            "proves": what,
        }
        print(f'[gen_rules] {label}: slot {slot} -> {hex(resolved)}', file=sys.stderr)
    rule["probes"] = probes
    return rule


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--apk', required=True, help='APK path or URL')
    ap.add_argument('--min', required=True, dest='min_ver')
    ap.add_argument('--max', dest='max_ver', default=None)
    ap.add_argument('--out', default=None, help='write the rule JSON here')
    ap.add_argument('--cache-dir', default=os.path.expanduser('~/.cache/glowberry-rules'))
    args = ap.parse_args()

    apk = fetch_apk(args.apk, args.cache_dir)
    so = extract_so(apk, os.path.join(args.cache_dir, 'libminecraftpe.so'))
    elf = Elf(so)
    rule = build_rule(elf, args.min_ver, args.max_ver or args.min_ver)
    text = json.dumps(rule, indent=2)
    if args.out:
        with open(args.out, 'w') as f:
            f.write(text + '\n')
        print(f'[gen_rules] wrote {args.out}', file=sys.stderr)
    else:
        print(text)
    return 0


if __name__ == '__main__':
    sys.exit(main())
