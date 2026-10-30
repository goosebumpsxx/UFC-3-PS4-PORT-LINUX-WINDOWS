#!/usr/bin/env python3
"""Find `mov [reg+disp32], src` stores with a given displacement in the flat UFC3 image."""
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


def main():
    disp = int(sys.argv[1], 0)
    tail = bytes([disp & 0xff, (disp >> 8) & 0xff, (disp >> 16) & 0xff, (disp >> 24) & 0xff])
    hits = []
    i = data.find(b'\x48\x89')
    while i >= 0:
        if i + 7 < len(data) and data[i + 3:i + 7] == tail:
            modrm = data[i + 2]
            if (modrm & 0xC0) == 0x80 and (modrm & 0x07) != 0x04:
                hits.append(i - DELTA)
        i = data.find(b'\x48\x89', i + 1)
    print('disp 0x%x: %d store(s)' % (disp, len(hits)))
    for h in hits[:30]:
        f = owner(h)
        print('  at 0x%x  %s  bytes %s' % (h, ('function 0x%x (+0x%x)' % (f[0], h - f[0])) if f else '(data)',
                                           data[h + DELTA - 4:h + DELTA + 8].hex(' ')))


if __name__ == '__main__':
    main()
