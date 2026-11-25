# Final Fantasy Mystic Quest on the Game Boy Advance

This directory is the start of a port of *Final Fantasy Mystic Quest* (SNES,
USA Rev 1) to the Game Boy Advance, in the spirit of the Super Metroid GBA
port and its porting guide. Nothing runs on a GBA yet. What is here is the
ground truth the rest of the work will be checked against: a headless SNES
reference runner with tracing, the first pieces of a 65816 recompiler, and
what tracing the game has shown so far about how it drives the hardware.

## Why a recompiler, not a decompilation

The Super Metroid port works because a C decompilation of that game exists,
verified frame by frame against the original. Mystic Quest has none: the
nearest thing is the assembly disassembly in this repository
(`src/asm/banks/`, produced with DiztinGUIsh and then relabelled). Writing a
decompilation by hand would be a far larger project than the port itself, and
emulating a whole SNES on the GBA's 16.78 MHz ARM7 is far too slow.

The plan is to translate the game's 65816 machine code into C automatically
(static recompilation) and compile that C for the GBA, replacing only the
platform layer: video, sound, input, saves and timing. The translation runs at
build time, from the player's own ROM, so neither the ROM nor anything derived
from it is ever committed: this repository holds the recompiler, its
annotations (addresses and flags only) and the platform code. The relabelled
disassembly is not used as input, because some of its labels no longer match
the branch targets recorded in its own comments (for example `bra
Init_FinalSetup` at `$0080AB` really goes to `$0080DC`); the recompiler works
from the ROM bytes and from execution traces instead.

## The reference runner

`tools/snesrun.c` runs the original ROM headlessly on a vendored copy of
[LakeSnes](https://github.com/angelo-wf/LakeSnes) (MIT licensed, see
`third_party/lakesnes/README.ffmq-gba.md` for the two small hooks added). It
scripts the joypad, saves screenshots, memory dumps and save states, prints
per-frame RAM hashes, profiles, and writes a coverage file recording every
executed instruction with its M, X and E flags and its D and DB registers,
every call, indirect jump and interrupt, every return that did not go back to
its call (stack tricks), and every jump out of code running in RAM. That
coverage is what the recompiler will use to find code and to check its own
static analysis.

Build it (needs a C compiler, libpng and zlib) and run it from the repository
root, with your own ROM at `roms/ffmq.sfc` (ignored by git):

```bash
make -C port/tools
```

```bash
port/tools/snesrun roms/ffmq.sfc FRAMES=1500 OUT=out SHOT_EVERY=100 STATS=1
```

The input script is a list of `buttons*frames` runs, so this goes from power
on through the title screen and name entry into the opening scene, saving a
coverage file:

```bash
port/tools/snesrun roms/ffmq.sfc FRAMES=4000 OUT=out KEYS="-*1500 start*2 -*100 a*2 -*30 start*2 -*60" COV=out/run.cov
```

`RANDOM=seed` presses random buttons once the script ends, which is a cheap
way to cover a lot of code: from the opening scene, random input gets through
the intro dialogue and into the first battle within 3,000 frames.
`STATE_OUT=frame:file` and `STATE=file` save and resume a run, `TRACE=a:b`
logs every instruction of frames `a` to `b`, and `DUMP=frames` writes WRAM,
VRAM, CGRAM and OAM. The header of `snesrun.c` lists every option.
`recomp/disasm.py` disassembles ROM code, taking M and X from coverage files
where the code was executed:

```bash
python3 port/recomp/disasm.py roms/ffmq.sfc 0096a9 8 --cov out/run.cov
```

## What the game does with the hardware

| Fact | Value |
|------|-------|
| ROM | LoROM, SlowROM, 512 KB, 8 KB battery SRAM, USA Rev 1 |
| SHA-1 | `6be74b73b736bc6027c0e92619874da7c498bed0` |
| Video modes seen | Mode 1 (with and without BG3 priority), Mode 7 in the opening sequence |
| Visible picture | lines 8 to 215 (208 lines); the rest is forced blank |
| Frame interrupts | IRQs only, at lines 7 and 216; NMI is never enabled |
| Code executed in random play | about 24,000 instructions, in banks `$00`–`$02`, `$07`, `$0B`–`$0D` |

The game never enables the NMI. Each frame is driven by two IRQs instead. At
line 216 a first handler (`$00B82A`) schedules a second IRQ later on the same
line, re-enables interrupts and waits with `WAI`; the nested handler
(`$00B86C`) forces blank and does the frame's uploads and per-mode work, then
both return. At line 7 the same two-step pattern (`$00B898`, `$00B8DA`) turns
the display back on. The handlers read the scanline counter (`$2137`,
`$213D`, `$213F`) to time this. In the field the main thread spends roughly
nine tenths of each frame in a three-instruction loop at `$0096A9`, waiting
for a flag the handlers set, so the game logic itself is light.

The game runs code from RAM in three places, all of which the port must
handle at run time rather than translate ahead of time. The interrupt vectors
point at `JML` trampolines at `$0113`, `$0117` and `$011B` that the game
rewrites to switch handlers. Four 16-byte slots at `$011F` to `$015E` hold
`JSL`/`JSL`/`JSL`/`JML` chains copied in per game mode, which the frame
handlers call. And there are small copy routines built around `MVN`: the one
at `$0030` has its bank operands written (`STA $31`) just before each call,
and the decompressor at `$0B8690` calls two more at `$0918` and `$091E`,
running in RAM as bank `$0B`.

Three dispatchers move control through the stack, so a return instruction is
not always a return. `$0097BE` reads a jump table whose address the caller
pushed with `PEA`, rewrites the stack and jumps with `RTI`, so that the target
returns to the caller. `$00A84D` builds a return address and a 24-bit target
on the stack and jumps with `RTL`, a computed `JSL`. And `$009881` pushes a
return address and a computed target, then jumps with `RTS` into the middle of
an unrolled copy loop whose final `RTS` comes back to `$009886`. Other things
worth knowing: the Mode 7 in the opening sequence uses HDMA on `M7A` and
`M7D`, the game uses the PPU's multiplier (written at `$211B`/`$211C`, read at
`$2134`/`$2135`) as a fast signed multiply as well as the CPU's multiply and
divide registers, and no decimal-mode code was executed.

## The recompiler, as designed

`recomp/` so far has the 65816 opcode table (`op65816.py`), LoROM access
(`rom.py`), the coverage reader (`coverage.py`) and the disassembler. The
design for the rest, worked out from the findings above, is as follows.

Every routine the game calls becomes a C function per M/X state it is entered
with, and returns the address execution continues at. The 65816 stack lives in
emulated WRAM exactly as on the SNES, so `JSR` pushes a real return address
and `RTS` pops it. A call compares what the callee returned with the address
after the call; a mismatch means the callee changed its return address, and is
either a resume point inside the caller or passed up to the next caller. An
analysis of each function's stack contents (which bytes it pushed, and which
of them are known constants, P register values or bank numbers) tells a real
return (stack depth zero) from a computed jump (the function returns through
bytes it pushed itself), which becomes a call to the target followed by
dispatch on where that call returned to. The same analysis tracks M and X
through `PHP`/`PLP` and the data bank through `PHK`/`PLB` and `PEA`/`PLB`.

Code that cannot be translated ahead of time (the RAM routines, and any path
no trace or analysis reached) runs in an interpreter generated from the same
instruction templates as the translated code, so both share one definition of
each instruction; on the host it is logged, so coverage can be widened. Time
is counted in SNES master cycles per translated block, so the line 7 and 216
IRQs land where they would on the SNES and the scanline reads return the right
lines, while pure wait loops such as `$0096A9` skip straight to the next
interrupt.

The first check of the recompiler will be on the PC: LakeSnes with its CPU
replaced by the translated code, run in lockstep with unmodified LakeSnes on
the same input, comparing WRAM whenever the game is waiting for its frame
interrupt. The first frame that differs points at the bug.

## Next steps

| Step | What it proves |
|------|----------------|
| Function and M/X discovery from coverage plus static analysis | every traced instruction is found, with the flags the trace saw |
| C emitter, generated interpreter, host runtime on LakeSnes | the translated game boots to the title on the PC |
| Lockstep comparison with the reference over scripted and random runs | the translation matches the original frame for frame |
| GBA toolchain (clang and lld, own `crt0`, linker script, header, small libc) and a cartridge builder that appends the player's ROM | a cartridge boots in mGBA |
| SNES hardware layer on the GBA: VRAM, CGRAM, OAM, DMA, HDMA, the IRQ schedule; Mode 1 to GBA mode 0 | the title screen on a GBA |
| Screen fit (208 visible lines into 160), Mode 7 intro, sound (the SPC700 driver translated the same way, plus a mixer), saves | the game playable |

## Licences

The port's own code is public domain, like the rest of this repository.
`third_party/lakesnes/` is MIT licensed (`third_party/lakesnes/LICENSE.txt`)
and is only used by host tools.
