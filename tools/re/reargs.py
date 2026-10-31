"""Paths shared by the tools/re scripts, taken from the command line.

--eboot PATH  the decrypted eboot.elf from your own UFC 3 dump (or UFC3_EBOOT)
--funcs PATH  the function list funcs.py writes (or UFC3_FUNCS; default funcs.txt)

Both options are removed from sys.argv, so each script reads its own arguments as before.

Limit: the scripts use one offset, DELTA = -0x12850 (eboot.elf offset = VA + 0x12850). That is
right for the code segment only (checked on the getter 0x1ac930 and its loads 0x1acb77/0x1acbd7).
eboot.elf is a PS4 SELF with several segments; strings, .eh_frame_hdr (funcs.py) and data sit in
segments with other offsets, so strrefs/funcs/addrrefs results are wrong until a per-segment map
replaces DELTA (docs/ufc3/BROADCAST-MOD-PHASE0.md section 3.4.1).
"""
import os
import sys


def _take(flag, env):
    value = os.environ.get(env)
    args = sys.argv
    for i in range(1, len(args)):
        if args[i] == flag and i + 1 < len(args):
            value = args[i + 1]
            del args[i:i + 2]
            break
        if args[i].startswith(flag + '='):
            value = args[i].split('=', 1)[1]
            del args[i]
            break
    return value


def eboot():
    path = _take('--eboot', 'UFC3_EBOOT')
    if not path:
        sys.exit('%s: give --eboot PATH (or set UFC3_EBOOT) to the eboot.elf of your own dump'
                 % os.path.basename(sys.argv[0]))
    return path


def funcs():
    return _take('--funcs', 'UFC3_FUNCS') or 'funcs.txt'
