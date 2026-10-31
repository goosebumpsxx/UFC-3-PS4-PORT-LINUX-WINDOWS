#!/usr/bin/env python3
"""Find RIP-relative references to given strings in the UFC3 eboot (VA = eboot.elf offset - 0x12850)."""
import struct
import sys

import reargs

PATH = reargs.eboot()
DELTA = -0x12850  # VA = file offset - DELTA: eboot.elf offset = VA + 0x12850 (notes §3.4.1)

data = open(PATH, 'rb').read()
print('file size', hex(len(data)))

def find_all(needle):
    out, i = [], data.find(needle)
    while i >= 0:
        out.append(i)
        i = data.find(needle, i + 1)
    return out

def refs_to(target_va, lo=0, hi=None):
    hi = hi or len(data)
    hits = []
    for i in range(lo, min(hi, len(data)) - 8):
        b0 = data[i]; b1 = data[i + 1]; b2 = data[i + 2]
        if b0 in (0x48, 0x4C, 0x49, 0x4D) and b1 in (0x8D, 0x8B, 0x89, 0x39, 0x3B) and (b2 & 0xC7) == 0x05:
            disp, = struct.unpack_from('<i', data, i + 3)
            if (i - DELTA) + 7 + disp == target_va:
                hits.append(i)
        elif b0 in (0x8D, 0x8B, 0x89) and (b1 & 0xC7) == 0x05:
            disp, = struct.unpack_from('<i', data, i + 2)
            if (i - DELTA) + 6 + disp == target_va:
                hits.append(i)
    return hits

names = sys.argv[1:] or ['ROUND_NUMBER_CURRENT', 'FIGHTER_SCORE_CARD']
for name in names:
    print('\n=== %s ===' % name)
    for off in find_all(name.encode()):
        va = off - DELTA
        hits = refs_to(va)
        print('  string at file 0x%x  va 0x%x  refs %d' % (off, va, len(hits)))
        for h in hits[:12]:
            print('     ref at va 0x%x (file 0x%x) bytes %s' % (h - DELTA, h, data[h:h + 8].hex(' ')))
