"""Linear disassembly helper: disasm.py ROM ADDR [COUNT] [--m 0|1] [--x 0|1] [--cov FILE...]

Flags come from coverage when the address was executed, else from --m/--x and
REP/SEP seen along the way.
"""

import sys, os
sys.path.insert(0, os.path.dirname(__file__))
from rom import Rom
from op65816 import OPS, length, fmt


def load_cov(paths):
    modes = {}
    for p in paths:
        for line in open(p):
            if line.startswith('I '):
                f = line.split()
                modes[int(f[1], 16)] = modes.get(int(f[1], 16), 0) | int(f[2], 16)
    return modes


def main():
    args = sys.argv[1:]
    rom = Rom(args[0])
    addr = int(args[1], 16)
    count = int(args[2]) if len(args) > 2 and not args[2].startswith('--') else 40
    m = x = 1
    cov = []
    i = 0
    while i < len(args):
        if args[i] == '--m': m = int(args[i + 1])
        if args[i] == '--x': x = int(args[i + 1])
        if args[i] == '--cov': cov = args[i + 1:]; break
        i += 1
    modes = load_cov(cov)
    for _ in range(count):
        mask = modes.get(addr, 0)
        mark = ' '
        if mask:
            # pick the first mode seen (bit index m<<2|x<<1|e)
            for b in range(8):
                if mask & (1 << b):
                    m, x = (b >> 2) & 1, (b >> 1) & 1
                    break
            mark = '*' if bin(mask).count('1') == 1 else '+'
        op = rom.u8(addr)
        n = length(op, m, x)
        operand = rom.operand(addr, n - 1)
        raw = ' '.join('%02x' % rom.u8((addr & 0xff0000) | ((addr + k) & 0xffff)) for k in range(n))
        print('%06x%s %-12s %-20s m%d x%d' % (addr, mark, raw, fmt(op, operand, addr, m, x), m, x))
        mnem, mode = OPS[op]
        if mnem == 'REP':
            if operand & 0x20: m = 0
            if operand & 0x10: x = 0
        if mnem == 'SEP':
            if operand & 0x20: m = 1
            if operand & 0x10: x = 1
        addr = (addr & 0xff0000) | ((addr + n) & 0xffff)


if __name__ == '__main__':
    main()
