#!/usr/bin/env python3
"""RIP-relative references to a code/data address in the flat UFC3 image (VA = file - 0x4000)."""
import struct
import sys

import reargs

FUNCS = reargs.funcs()
PATH = reargs.eboot()
DELTA = 0x4000
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


def refs_to(target_va):
    hits = []
    for i in range(DELTA, len(data) - 8):
        b0 = data[i]; b1 = data[i + 1]; b2 = data[i + 2]
        if b0 in (0x48, 0x4C, 0x49, 0x4D) and b1 in (0x8D, 0x8B, 0x89, 0x39, 0x3B, 0x03) and (b2 & 0xC7) == 0x05:
            disp, = struct.unpack_from('<i', data, i + 3)
            if (i - DELTA) + 7 + disp == target_va:
                hits.append(i - DELTA)
        elif b0 in (0x8D, 0x8B, 0x89) and (b1 & 0xC7) == 0x05:
            disp, = struct.unpack_from('<i', data, i + 2)
            if (i - DELTA) + 6 + disp == target_va:
                hits.append(i - DELTA)
    return hits


def main():
    t = int(sys.argv[1], 0)
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else 40
    hits = refs_to(t)
    print('address 0x%x: %d references' % (t, len(hits)))
    seen = set()
    for h in hits[:limit]:
        f = owner(h)
        key = f[0] if f else None
        print('  ref 0x%x  function 0x%x (+0x%x)' % (h, f[0], h - f[0]) if f else '  ref 0x%x (no function)' % h)


if __name__ == '__main__':
    main()
