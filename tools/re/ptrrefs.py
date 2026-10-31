#!/usr/bin/env python3
"""Find image pointers (0x800000000 + VA) to a VA: vtable slots, function tables."""
import struct
import sys

import reargs

PATH = reargs.eboot()
DELTA = -0x12850  # VA = file offset - DELTA: eboot.elf offset = VA + 0x12850 (notes §3.4.1)
BASE = 0x800000000
data = open(PATH, 'rb').read()


def main():
    for arg in sys.argv[1:]:
        va = int(arg, 0)
        pat = struct.pack('<Q', BASE + va)
        hits = []
        i = data.find(pat)
        while i >= 0:
            hits.append(i)
            i = data.find(pat, i + 1)
        print('VA 0x%x: %d pointer(s)' % (va, len(hits)))
        for h in hits[:8]:
            v = h - DELTA
            start = h - 8 * 4
            words = struct.unpack_from('<8Q', data, start)
            print('  at image 0x%x; neighbours:' % v)
            for k, w in enumerate(words):
                slot_va = (start - DELTA) + 8 * k
                print('     0x%x: 0x%x' % (slot_va, w))


if __name__ == '__main__':
    main()
