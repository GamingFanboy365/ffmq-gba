# LakeSnes (vendored)

The SNES emulation core of [LakeSnes](https://github.com/angelo-wf/LakeSnes)
by angelo_wf and contributors, MIT licensed (see `LICENSE.txt`), at commit
`9db90b86e46a377609305e298dd92d71cd1d4c8a` (2023-12-02).

It is used only by host-side tools: the reference runner (`port/tools/snesrun.c`),
the lockstep checker, and as the reference PPU/DSP. Nothing from here is linked
into the GBA cartridge.

Local changes, kept minimal so the code can be compared with upstream:

- `cpu.h`/`cpu.c`: optional `onOpcode` and `onInterrupt` hooks with a context
  pointer, called before each opcode and on interrupt entry, for tracing.
