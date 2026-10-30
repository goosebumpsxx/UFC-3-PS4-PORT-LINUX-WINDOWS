"""Paths shared by the tools/re scripts, taken from the command line.

--eboot PATH  the decrypted eboot.elf from your own UFC 3 dump (or UFC3_EBOOT)
--funcs PATH  the function list funcs.py writes (or UFC3_FUNCS; default funcs.txt)

Both options are removed from sys.argv, so each script reads its own arguments as before.
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
