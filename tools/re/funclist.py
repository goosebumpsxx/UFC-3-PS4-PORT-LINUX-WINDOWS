#!/usr/bin/env python3
"""List functions (from funcs.txt) inside a VA range."""
import sys

import reargs

FUNCS = reargs.funcs()
lo = int(sys.argv[1], 0)
hi = int(sys.argv[2], 0)
for line in open(FUNCS):
    a, b = line.split()
    va = int(a, 16)
    if lo <= va < hi:
        print('0x%-9x size %d' % (va, int(b)))
