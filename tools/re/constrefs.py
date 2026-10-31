#!/usr/bin/env python3
"""Where the eboot mentions a given 32-bit constant (candidate stat ids), with a disassembly window."""
import struct
import subprocess
import sys

import reargs

FUNCS = reargs.funcs()
PATH = reargs.eboot()
DELTA = -0x12850  # VA = file offset - DELTA: eboot.elf offset = VA + 0x12850 (notes §3.4.1)
data = open(PATH, 'rb').read()
funcs = []
for line in open(FUNCS):
    a, b = line.split()
    funcs.append((int(a, 16), int(b)))
funcs.sort()


def owner(t):
    best = None
    for s, size in funcs:
        if s <= t < s + size:
            best = (s, size)
    return best


def main():
    val = int(sys.argv[1], 0)
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else 20
    pat = struct.pack('<I', val)
    hits = []
    i = data.find(pat)
    while i >= 0:
        hits.append(i)
        i = data.find(pat, i + 1)
    print('constant 0x%x: %d occurrences' % (val, len(hits)))
    shown = 0
    for h in hits:
        va = h - DELTA
        f = owner(va)
        if not f:
            continue
        print('  at va 0x%x in function 0x%x (+0x%x)  bytes %s'
              % (va, f[0], va - f[0], data[i0:i0 + 12].hex(' ') if False else data[h - 8:h + 8].hex(' ')))
        shown += 1
        if shown >= limit:
            break


if __name__ == '__main__':
    main()
