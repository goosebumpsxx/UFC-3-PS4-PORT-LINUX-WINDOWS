#!/usr/bin/env python3
"""Disassemble around a VA of the flat UFC3 image (VA = file - 0x4000), from its function start.

usage: disas.py VA [before] [after]
Only instructions inside [VA-before, VA+after] are printed; decoding starts at the function
start so that the alignment is real.
"""
import os
import subprocess
import sys
import tempfile

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
    va = int(sys.argv[1], 0)
    before = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x100
    after = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x100
    f = owner(va) or (va, 0)
    begin = f[0]
    end = min(va + after, begin + (f[1] or after))
    chunk = data[begin + DELTA:end + DELTA]
    fd, chunk_path = tempfile.mkstemp(suffix='.bin')
    with os.fdopen(fd, 'wb') as out_file:
        out_file.write(chunk)
    print('# function 0x%x size %d; decoding 0x%x..0x%x; showing 0x%x..0x%x'
          % (f[0], f[1], begin, end, va - before, va + after))
    out = subprocess.run(['objdump', '-D', '-b', 'binary', '-m', 'i386:x86-64', '-M', 'intel',
                          '--adjust-vma=0x%x' % begin, chunk_path],
                         capture_output=True, text=True).stdout
    os.remove(chunk_path)
    for line in out.splitlines():
        if ':\t' not in line:
            continue
        try:
            addr = int(line.split(':')[0].strip(), 16)
        except ValueError:
            continue
        if va - before <= addr <= va + after:
            print(line)


if __name__ == '__main__':
    main()
