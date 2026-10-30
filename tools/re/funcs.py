#!/usr/bin/env python3
"""Function bounds of the UFC3 eboot from .eh_frame_hdr (flat image, VA = file - 0x4000)."""
import struct
import sys

import reargs

FUNCS = reargs.funcs()
PATH = reargs.eboot()
DELTA = 0x4000
MAGIC = 0x3B031B01

data = open(PATH, 'rb').read()


def eh_frame_hdr():
    off = data.find(struct.pack('<I', MAGIC))
    while off >= 0:
        ver, eh_ptr_enc, cnt_enc, tab_enc = data[off:off + 4]
        if ver == 1 and (eh_ptr_enc & 0x0F) == 0x0B and (cnt_enc & 0x0F) == 0x03 and (tab_enc & 0x0F) == 0x0B:
            return off
        off = data.find(struct.pack('<I', MAGIC), off + 1)
    raise SystemExit('no eh_frame_hdr')


def functions():
    off = eh_frame_hdr()
    va = off - DELTA
    ptr, cnt = struct.unpack_from('<Ii', data, off + 4)
    table = off + 12
    funcs = []
    for i in range(cnt):
        loc, fde = struct.unpack_from('<ii', data, table + 8 * i)
        funcs.append((va + loc, 0))
    starts = [f[0] for f in funcs]
    # FDE -> size: the FDE's CIE pointer at +4, then pc_begin at +8 (sdata4), range at +12.
    fde_vas = {}
    for i in range(cnt):
        loc, fde = struct.unpack_from('<ii', data, table + 8 * i)
        fva = va + fde
        foff = fva + DELTA
        begin, rng = struct.unpack_from('<ii', data, foff + 8)
        fde_vas[va + loc] = rng
    return [(s, fde_vas.get(s, 0)) for s in starts]


def main():
    funcs = functions()
    funcs.sort()
    with open(FUNCS, 'w') as f:
        for s, size in funcs:
            f.write('0x%x %d\n' % (s, size))
    print('functions', len(funcs), 'first 0x%x last 0x%x' % (funcs[0][0], funcs[-1][0]))
    for target in sys.argv[1:]:
        t = int(target, 0)
        owner = None
        for s, size in funcs:
            if s <= t < s + size:
                owner = (s, size)
        print('0x%x -> function 0x%x size %d (offset +0x%x)' % (t, owner[0], owner[1], t - owner[0]) if owner else '0x%x -> none' % t)


if __name__ == '__main__':
    main()
