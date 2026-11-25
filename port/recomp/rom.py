"""LoROM image access for the recompiler (FFMQ is a 512 KB LoROM)."""

import hashlib

FFMQ_USA_REV1_SHA1 = '6be74b73b736bc6027c0e92619874da7c498bed0'


class Rom:
    def __init__(self, path):
        data = open(path, 'rb').read()
        if len(data) % 0x8000 == 0x200:
            data = data[0x200:]  # copier header
        self.data = data
        self.sha1 = hashlib.sha1(data).hexdigest()

    def offset(self, addr):
        """File offset of a 24-bit CPU address in ROM, or None if it isn't ROM."""
        bank, lo = addr >> 16, addr & 0xffff
        if bank in (0x7e, 0x7f):
            return None
        b = bank & 0x7f
        if lo < 0x8000 and b < 0x40:
            return None  # RAM mirror, I/O, open bus
        if 0x70 <= b < 0x7e and lo < 0x8000:
            return None  # SRAM
        off = (b << 15) | (lo & 0x7fff)
        return off % len(self.data)

    def is_rom(self, addr):
        return self.offset(addr) is not None

    def u8(self, addr):
        return self.data[self.offset(addr)]

    def u16(self, addr):
        # operands are fetched with the bank fixed (pc wraps within the bank)
        return self.u8(addr) | (self.u8((addr & 0xff0000) | ((addr + 1) & 0xffff)) << 8)

    def u24(self, addr):
        b = addr & 0xff0000
        return self.u8(addr) | (self.u8(b | ((addr + 1) & 0xffff)) << 8) | (self.u8(b | ((addr + 2) & 0xffff)) << 16)

    def operand(self, addr, n):
        if n == 0:
            return 0
        if n == 1:
            return self.u8((addr & 0xff0000) | ((addr + 1) & 0xffff))
        if n == 2:
            return self.u16((addr & 0xff0000) | ((addr + 1) & 0xffff))
        return self.u24((addr & 0xff0000) | ((addr + 1) & 0xffff))
