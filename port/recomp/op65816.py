"""65816 opcode table for the FFMQ recompiler.

Each opcode maps to (mnemonic, addressing mode). Instruction length depends on
the mode and, for immediates, on the M or X flag in effect.

Addressing modes:
  imp    implied            acc    accumulator (ASL A ...)
  immm   #imm sized by M    immx   #imm sized by X     imm8   #imm 8-bit (REP, SEP, BRK, COP, WDM)
  dp     dp                 dpx    dp,X               dpy    dp,Y
  dpi    (dp)               dpix   (dp,X)             dpiy   (dp),Y
  dpil   [dp]               dpily  [dp],Y
  abs    abs                absx   abs,X              absy   abs,Y
  absl   long               abslx  long,X
  absi   (abs)  (JMP)       absix  (abs,X) (JMP, JSR)  absil  [abs] (JML)
  sr     sr,S               sriy   (sr,S),Y
  rel8   8-bit branch       rel16  16-bit branch (BRL, PER)
  bm     block move (MVN, MVP)
  imm16  PEA
"""

OPS = {}

def _row(base, mnem):
    # the eight "group 1" ALU instructions share one layout
    modes = {0x01: 'dpix', 0x03: 'sr', 0x05: 'dp', 0x07: 'dpil', 0x09: 'immm', 0x0d: 'abs', 0x0f: 'absl',
             0x11: 'dpiy', 0x12: 'dpi', 0x13: 'sriy', 0x15: 'dpx', 0x17: 'dpily', 0x19: 'absy', 0x1d: 'absx', 0x1f: 'abslx'}
    for off, mode in modes.items():
        OPS[base + off] = (mnem, mode)

for base, mnem in [(0x00, 'ORA'), (0x20, 'AND'), (0x40, 'EOR'), (0x60, 'ADC'), (0x80, 'STA'), (0xa0, 'LDA'), (0xc0, 'CMP'), (0xe0, 'SBC')]:
    _row(base, mnem)
del OPS[0x89]  # STA # does not exist: $89 is BIT #

OPS.update({
    0x00: ('BRK', 'imm8'), 0x02: ('COP', 'imm8'), 0x04: ('TSB', 'dp'), 0x06: ('ASL', 'dp'), 0x08: ('PHP', 'imp'),
    0x0a: ('ASL', 'acc'), 0x0b: ('PHD', 'imp'), 0x0c: ('TSB', 'abs'), 0x0e: ('ASL', 'abs'),
    0x10: ('BPL', 'rel8'), 0x14: ('TRB', 'dp'), 0x16: ('ASL', 'dpx'), 0x18: ('CLC', 'imp'), 0x1a: ('INC', 'acc'),
    0x1b: ('TCS', 'imp'), 0x1c: ('TRB', 'abs'), 0x1e: ('ASL', 'absx'),
    0x20: ('JSR', 'abs'), 0x22: ('JSL', 'absl'), 0x24: ('BIT', 'dp'), 0x26: ('ROL', 'dp'), 0x28: ('PLP', 'imp'),
    0x2a: ('ROL', 'acc'), 0x2b: ('PLD', 'imp'), 0x2c: ('BIT', 'abs'), 0x2e: ('ROL', 'abs'),
    0x30: ('BMI', 'rel8'), 0x34: ('BIT', 'dpx'), 0x36: ('ROL', 'dpx'), 0x38: ('SEC', 'imp'), 0x3a: ('DEC', 'acc'),
    0x3b: ('TSC', 'imp'), 0x3c: ('BIT', 'absx'), 0x3e: ('ROL', 'absx'),
    0x40: ('RTI', 'imp'), 0x42: ('WDM', 'imm8'), 0x44: ('MVP', 'bm'), 0x46: ('LSR', 'dp'), 0x48: ('PHA', 'imp'),
    0x4a: ('LSR', 'acc'), 0x4b: ('PHK', 'imp'), 0x4c: ('JMP', 'abs'), 0x4e: ('LSR', 'abs'),
    0x50: ('BVC', 'rel8'), 0x54: ('MVN', 'bm'), 0x56: ('LSR', 'dpx'), 0x58: ('CLI', 'imp'), 0x5a: ('PHY', 'imp'),
    0x5b: ('TCD', 'imp'), 0x5c: ('JML', 'absl'), 0x5e: ('LSR', 'absx'),
    0x60: ('RTS', 'imp'), 0x62: ('PER', 'rel16'), 0x64: ('STZ', 'dp'), 0x66: ('ROR', 'dp'), 0x68: ('PLA', 'imp'),
    0x6a: ('ROR', 'acc'), 0x6b: ('RTL', 'imp'), 0x6c: ('JMP', 'absi'), 0x6e: ('ROR', 'abs'),
    0x70: ('BVS', 'rel8'), 0x74: ('STZ', 'dpx'), 0x76: ('ROR', 'dpx'), 0x78: ('SEI', 'imp'), 0x7a: ('PLY', 'imp'),
    0x7b: ('TDC', 'imp'), 0x7c: ('JMP', 'absix'), 0x7e: ('ROR', 'absx'),
    0x80: ('BRA', 'rel8'), 0x82: ('BRL', 'rel16'), 0x84: ('STY', 'dp'), 0x86: ('STX', 'dp'), 0x88: ('DEY', 'imp'),
    0x89: ('BIT', 'immm'), 0x8a: ('TXA', 'imp'), 0x8b: ('PHB', 'imp'), 0x8c: ('STY', 'abs'), 0x8e: ('STX', 'abs'),
    0x90: ('BCC', 'rel8'), 0x94: ('STY', 'dpx'), 0x96: ('STX', 'dpy'), 0x98: ('TYA', 'imp'), 0x9a: ('TXS', 'imp'),
    0x9b: ('TXY', 'imp'), 0x9c: ('STZ', 'abs'), 0x9e: ('STZ', 'absx'),
    0xa0: ('LDY', 'immx'), 0xa2: ('LDX', 'immx'), 0xa4: ('LDY', 'dp'), 0xa6: ('LDX', 'dp'), 0xa8: ('TAY', 'imp'),
    0xaa: ('TAX', 'imp'), 0xab: ('PLB', 'imp'), 0xac: ('LDY', 'abs'), 0xae: ('LDX', 'abs'),
    0xb0: ('BCS', 'rel8'), 0xb4: ('LDY', 'dpx'), 0xb6: ('LDX', 'dpy'), 0xb8: ('CLV', 'imp'), 0xba: ('TSX', 'imp'),
    0xbb: ('TYX', 'imp'), 0xbc: ('LDY', 'absx'), 0xbe: ('LDX', 'absy'),
    0xc0: ('CPY', 'immx'), 0xc2: ('REP', 'imm8'), 0xc4: ('CPY', 'dp'), 0xc6: ('DEC', 'dp'), 0xc8: ('INY', 'imp'),
    0xca: ('DEX', 'imp'), 0xcb: ('WAI', 'imp'), 0xcc: ('CPY', 'abs'), 0xce: ('DEC', 'abs'),
    0xd0: ('BNE', 'rel8'), 0xd4: ('PEI', 'dp'), 0xd6: ('DEC', 'dpx'), 0xd8: ('CLD', 'imp'), 0xda: ('PHX', 'imp'),
    0xdb: ('STP', 'imp'), 0xdc: ('JML', 'absil'), 0xde: ('DEC', 'absx'),
    0xe0: ('CPX', 'immx'), 0xe2: ('SEP', 'imm8'), 0xe4: ('CPX', 'dp'), 0xe6: ('INC', 'dp'), 0xe8: ('INX', 'imp'),
    0xea: ('NOP', 'imp'), 0xeb: ('XBA', 'imp'), 0xec: ('CPX', 'abs'), 0xee: ('INC', 'abs'),
    0xf0: ('BEQ', 'rel8'), 0xf4: ('PEA', 'imm16'), 0xf6: ('INC', 'dpx'), 0xf8: ('SED', 'imp'), 0xfa: ('PLX', 'imp'),
    0xfb: ('XCE', 'imp'), 0xfc: ('JSR', 'absix'), 0xfe: ('INC', 'absx'),
})
assert len(OPS) == 256, len(OPS)

MODE_LEN = {
    'imp': 1, 'acc': 1, 'imm8': 2, 'dp': 2, 'dpx': 2, 'dpy': 2, 'dpi': 2, 'dpix': 2, 'dpiy': 2, 'dpil': 2,
    'dpily': 2, 'abs': 3, 'absx': 3, 'absy': 3, 'absl': 4, 'abslx': 4, 'absi': 3, 'absix': 3, 'absil': 3,
    'sr': 2, 'sriy': 2, 'rel8': 2, 'rel16': 3, 'bm': 3, 'imm16': 3,
}

BRANCHES = {'BPL', 'BMI', 'BVC', 'BVS', 'BCC', 'BCS', 'BNE', 'BEQ'}
# instructions whose operand size follows M (memory/accumulator) or X (index)
USES_M = {'ORA', 'AND', 'EOR', 'ADC', 'STA', 'LDA', 'CMP', 'SBC', 'BIT', 'TSB', 'TRB', 'ASL', 'LSR', 'ROL', 'ROR',
          'INC', 'DEC', 'STZ', 'PHA', 'PLA', 'TXA', 'TYA'}
USES_X = {'LDX', 'LDY', 'STX', 'STY', 'CPX', 'CPY', 'INX', 'INY', 'DEX', 'DEY', 'PHX', 'PHY', 'PLX', 'PLY',
          'TAX', 'TAY', 'TSX', 'TXY', 'TYX'}


def length(op, m, x):
    mnem, mode = OPS[op]
    if mode == 'immm':
        return 2 if m else 3
    if mode == 'immx':
        return 2 if x else 3
    return MODE_LEN[mode]


def depends_on_m(op):
    mnem, mode = OPS[op]
    return mnem in USES_M or mode == 'immm'


def depends_on_x(op):
    mnem, mode = OPS[op]
    return mnem in USES_X or mode in ('immx', 'absx', 'absy', 'dpx', 'dpy', 'dpix', 'dpiy', 'dpily', 'sriy', 'abslx', 'absix') \
        or mnem in ('MVN', 'MVP')


def fmt(op, operand, pc, m, x):
    """Assembly text for an instruction (operand is the little-endian operand value)."""
    mnem, mode = OPS[op]
    n = length(op, m, x) - 1
    o = operand
    if mode in ('imp',):
        return mnem
    if mode == 'acc':
        return mnem + ' A'
    if mode in ('immm', 'immx', 'imm8'):
        return '%s #$%0*x' % (mnem, n * 2, o)
    if mode == 'imm16':
        return '%s $%04x' % (mnem, o)
    if mode in ('rel8', 'rel16'):
        disp = o - 0x100 if (mode == 'rel8' and o >= 0x80) else (o - 0x10000 if (mode == 'rel16' and o >= 0x8000) else o)
        tgt = (pc & 0xff0000) | ((pc + 1 + n + disp) & 0xffff)
        return '%s $%06x' % (mnem, tgt)
    if mode == 'bm':
        return '%s $%02x,$%02x' % (mnem, o & 0xff, o >> 8)
    f = {'dp': '$%02x', 'dpx': '$%02x,X', 'dpy': '$%02x,Y', 'dpi': '($%02x)', 'dpix': '($%02x,X)', 'dpiy': '($%02x),Y',
         'dpil': '[$%02x]', 'dpily': '[$%02x],Y', 'abs': '$%04x', 'absx': '$%04x,X', 'absy': '$%04x,Y', 'absl': '$%06x',
         'abslx': '$%06x,X', 'absi': '($%04x)', 'absix': '($%04x,X)', 'absil': '[$%04x]', 'sr': '$%02x,S',
         'sriy': '($%02x,S),Y'}[mode]
    return mnem + ' ' + (f % o)
