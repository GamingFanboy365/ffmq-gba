// snesrun: headless SNES reference runner for the FFMQ GBA port.
//
// Runs the original ROM on the vendored LakeSnes core, with scripted input,
// and records what the port's tools need as ground truth:
//
//   snesrun ROM [KEY=VALUE ...]
//     FRAMES=n           frames to run (default 600)
//     KEYS=script        buttons per run of frames, e.g. "-*120 start*2 -*60 a+up*3"
//                        names: a b x y l r start select up down left right, "-" = none
//     OUT=dir            output directory (default "out")
//     SHOTS=f,f,...      PNG screenshots at these frames (after the frame ran)
//     SHOT_EVERY=n       a screenshot every n frames
//     DUMP=f,f,...       memory dumps (WRAM, VRAM, CGRAM, OAM, registers) at frames
//     COV=file           code coverage: executed instructions with M/X/E, D and DB,
//                        calls, indirect jumps, non-matching returns, interrupts
//     COUNTS=file        execution counts per instruction (a profile)
//     TRACE=from:to      instruction trace for frames [from, to) into OUT/trace.txt
//     HASH=1             print a hash of WRAM after every frame
//     STATS=1            print hardware usage (BG modes, HDMA, IRQ, APU ports)
//     STATE=file         load a LakeSnes save state before running
//     STATE_OUT=f:file   save a state after frame f
//     SRAM=file          battery RAM to load (and save back at exit)
//     RANDOM=seed        after the KEYS script ends, press random buttons (for coverage)
//
// Frame numbers count frames run by this process, from 1.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <png.h>
#include <sys/stat.h>

#include "snes.h"
#include "cpu.h"
#include "ppu.h"
#include "dma.h"

#define ADDR_SPACE (1 << 24)

typedef struct {
  uint32_t start, count;
  uint16_t buttons;
} KeyRun;

static struct {
  Snes* snes;
  const char* outDir;
  int frame; // frame being run (1-based)
  // coverage
  bool cov;
  uint8_t* flags;      // per address: bit (m<<2|x<<1|e) set when executed in that mode
  uint16_t* dpSeen;    // per address: D value, or 0xffff after varying (with dpVar)
  uint8_t* dpVar;      // bit0: D varies, bit1: DB varies, bit7: seen
  uint8_t* dbSeen;
  uint32_t* counts;
  // pending control transfer: decode at next opcode
  uint32_t lastPc;
  uint8_t lastOp;
  uint8_t lastMode;
  bool pendingIndirect;
  bool pendingReturn;
  uint32_t expectedReturn;
  // shadow call stack
  struct { uint16_t sp; uint32_t ret; uint8_t kind; } stack[256];
  int stackTop;
  // edge set (hash)
  struct Edge { uint32_t src, dst; uint8_t kind, mode; uint32_t count; } *edges;
  int edgeCount, edgeCap;
  // trace
  FILE* trace;
  int traceFrom, traceTo;
  // stats
  bool stats;
  uint32_t bgmodeCount[256];
  uint32_t nmitimenCount[256];
  uint32_t hdmaenCount[256];
  uint32_t regWrites[0x10000]; // writes per register address $2100-$43ff (low 16 bits)
  uint32_t apuReads, apuWrites;
  uint32_t nmiCount, irqCount;
  uint32_t hdmaTargets[8][256];
  uint32_t irqLines[512];
  uint32_t regReads[0x10000];
} g;

static uint32_t hashBytes(const uint8_t* p, size_t n) {
  uint32_t h = 2166136261u;
  for(size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
  return h;
}

// --- edges -----------------------------------------------------------------

// C: direct call, J: indirect jump/call, R: return to another address than the call's,
// P: return with no matching call (a pushed address used as a jump), N/Q: NMI/IRQ taken
// X: a jump or call from code running in RAM to code elsewhere
enum { EDGE_CALL = 'C', EDGE_IND = 'J', EDGE_RET = 'R', EDGE_PUSHED = 'P', EDGE_NMI = 'N', EDGE_IRQ = 'Q', EDGE_RAM = 'X' };

static bool isRam(uint32_t adr) {
  uint8_t bank = adr >> 16;
  return bank == 0x7e || bank == 0x7f || ((bank < 0x40 || (bank >= 0x80 && bank < 0xc0)) && (adr & 0xffff) < 0x2000);
}

static void addEdge(uint32_t src, uint32_t dst, uint8_t kind, uint8_t mode) {
  if(g.edgeCount * 2 >= g.edgeCap) {
    int oldCap = g.edgeCap;
    struct Edge* old = g.edges;
    g.edgeCap = oldCap ? oldCap * 2 : 1 << 16;
    g.edges = calloc(g.edgeCap, sizeof(struct Edge));
    g.edgeCount = 0;
    for(int i = 0; i < oldCap; i++) {
      if(old[i].count) {
        uint32_t h = (old[i].src * 2654435761u) ^ (old[i].dst * 40503u) ^ old[i].kind ^ (old[i].mode << 8);
        for(uint32_t j = h & (g.edgeCap - 1);; j = (j + 1) & (g.edgeCap - 1)) {
          if(!g.edges[j].count) { g.edges[j] = old[i]; g.edgeCount++; break; }
        }
      }
    }
    free(old);
  }
  uint32_t h = (src * 2654435761u) ^ (dst * 40503u) ^ kind ^ (mode << 8);
  for(uint32_t j = h & (g.edgeCap - 1);; j = (j + 1) & (g.edgeCap - 1)) {
    struct Edge* e = &g.edges[j];
    if(!e->count) {
      e->src = src; e->dst = dst; e->kind = kind; e->mode = mode; e->count = 1;
      g.edgeCount++;
      return;
    }
    if(e->src == src && e->dst == dst && e->kind == kind && e->mode == mode) {
      e->count++;
      return;
    }
  }
}

// --- side-effect-free reads for decoding -------------------------------------

static uint8_t peek(uint32_t adr) {
  uint8_t bank = adr >> 16;
  uint16_t a = adr & 0xffff;
  if(bank == 0x7e || bank == 0x7f) return g.snes->ram[((bank & 1) << 16) | a];
  if((bank < 0x40 || (bank >= 0x80 && bank < 0xc0)) && a < 0x2000) return g.snes->ram[a];
  Cart* c = g.snes->cart;
  if(((bank >= 0x70 && bank < 0x7e) || bank >= 0xf0) && a < 0x8000 && c->ramSize > 0)
    return c->ram[(((bank & 0xf) << 15) | a) & (c->ramSize - 1)];
  bank &= 0x7f;
  if(a >= 0x8000 || bank >= 0x40) return c->rom[((bank << 15) | (a & 0x7fff)) & (c->romSize - 1)];
  return 0;
}

static uint8_t modeOf(Cpu* cpu) {
  return (cpu->mf << 2) | (cpu->xf << 1) | cpu->e;
}

// --- hooks -------------------------------------------------------------------

static void onOpcode(void* ctx, Cpu* cpu) {
  (void)ctx;
  uint32_t pc = (cpu->k << 16) | cpu->pc;
  uint8_t mode = modeOf(cpu);
  if(g.pendingIndirect) {
    addEdge(g.lastPc, pc, EDGE_IND, g.lastMode);
    g.pendingIndirect = false;
  }
  if(g.pendingReturn) {
    if(pc != g.expectedReturn) addEdge(g.lastPc, pc, g.expectedReturn == 0xffffffff ? EDGE_PUSHED : EDGE_RET, g.lastMode);
    g.pendingReturn = false;
  }
  uint8_t op = peek(pc);
  if(g.cov && isRam(g.lastPc) && !isRam(pc)) {
    switch(g.lastOp) {
      case 0x4c: case 0x5c: case 0x20: case 0x22: case 0x6c: case 0x7c: case 0xdc: case 0xfc:
        addEdge(g.lastPc, pc, EDGE_RAM, g.lastMode);
    }
  }
  if(g.cov) {
    g.flags[pc] |= 1 << mode;
    g.counts[pc]++;
    if(!(g.dpVar[pc] & 0x80)) {
      g.dpVar[pc] = 0x80;
      g.dpSeen[pc] = cpu->dp;
      g.dbSeen[pc] = cpu->db;
    } else {
      if(g.dpSeen[pc] != cpu->dp) g.dpVar[pc] |= 1;
      if(g.dbSeen[pc] != cpu->db) g.dpVar[pc] |= 2;
    }
    switch(op) {
      case 0x20: case 0x22: case 0xfc: { // jsr abs, jsl, jsr (abs,x)
        int len = op == 0x22 ? 4 : 3;
        uint32_t ret = (pc & 0xff0000) | ((pc + len) & 0xffff);
        uint16_t spAfter = cpu->sp - (op == 0x22 ? 3 : 2);
        if(g.stackTop < 256) {
          g.stack[g.stackTop].sp = spAfter;
          g.stack[g.stackTop].ret = ret;
          g.stack[g.stackTop].kind = op == 0x22 ? 1 : 0;
          g.stackTop++;
        }
        if(op == 0x20) addEdge(pc, (pc & 0xff0000) | peek(pc + 1) | (peek(pc + 2) << 8), EDGE_CALL, mode);
        else if(op == 0x22) addEdge(pc, peek(pc + 1) | (peek(pc + 2) << 8) | (peek(pc + 3) << 16), EDGE_CALL, mode);
        else g.pendingIndirect = true;
        break;
      }
      case 0x6c: case 0x7c: case 0xdc: // jmp (abs), jmp (abs,x), jml [abs]
        g.pendingIndirect = true;
        break;
      case 0x60: case 0x6b: { // rts, rtl
        // find the call whose pushed return address this pops
        g.expectedReturn = 0xffffffff;
        for(int i = g.stackTop - 1; i >= 0; i--) {
          if(g.stack[i].sp == cpu->sp && g.stack[i].kind == (op == 0x6b)) {
            g.expectedReturn = g.stack[i].ret;
            g.stackTop = i;
            break;
          }
        }
        g.pendingReturn = true;
        break;
      }
      case 0x40: // rti
        g.expectedReturn = 0xffffffff; // interrupts: compare against the interrupted pc below
        for(int i = g.stackTop - 1; i >= 0; i--) {
          if(g.stack[i].sp == cpu->sp && g.stack[i].kind == 2) {
            g.expectedReturn = g.stack[i].ret;
            g.stackTop = i;
            break;
          }
        }
        g.pendingReturn = true;
        break;
    }
  }
  g.lastPc = pc;
  g.lastOp = op;
  g.lastMode = mode;
  if(g.trace && g.frame >= g.traceFrom && g.frame < g.traceTo) {
    fprintf(g.trace, "%06x %02x a=%04x x=%04x y=%04x s=%04x d=%04x db=%02x %c%c%c%c%c%c%c%c%c v=%3d h=%4d\n",
      pc, op, cpu->a, cpu->x, cpu->y, cpu->sp, cpu->dp, cpu->db,
      cpu->n ? 'N' : 'n', cpu->v ? 'V' : 'v', cpu->mf ? 'M' : 'm', cpu->xf ? 'X' : 'x',
      cpu->d ? 'D' : 'd', cpu->i ? 'I' : 'i', cpu->z ? 'Z' : 'z', cpu->c ? 'C' : 'c', cpu->e ? 'E' : 'e',
      g.snes->vPos, g.snes->hPos);
  }
}

static void onInterrupt(void* ctx, Cpu* cpu, bool nmi) {
  (void)ctx;
  // called before the interrupt pushes anything: k:pc is the interrupted pc
  uint32_t pc = (cpu->k << 16) | cpu->pc;
  if(nmi) g.nmiCount++; else { g.irqCount++; if(g.snes->vPos < 512) g.irqLines[g.snes->vPos]++; }
  if(g.pendingIndirect || g.pendingReturn) {
    // the interrupt was taken right after a jump: record the jump's target now
    if(g.pendingIndirect) addEdge(g.lastPc, pc, EDGE_IND, g.lastMode);
    if(g.pendingReturn && pc != g.expectedReturn) addEdge(g.lastPc, pc, g.expectedReturn == 0xffffffff ? EDGE_PUSHED : EDGE_RET, g.lastMode);
    g.pendingIndirect = g.pendingReturn = false;
  }
  // the interrupt pushes k, pc, p (4 bytes in native mode): RTI pops them at sp - 4
  if(g.cov && g.stackTop < 256) {
    g.stack[g.stackTop].sp = cpu->sp - (cpu->e ? 3 : 4);
    g.stack[g.stackTop].ret = pc;
    g.stack[g.stackTop].kind = 2;
    g.stackTop++;
  }
  uint16_t vec = nmi ? 0xffea : 0xffee;
  uint32_t dst = peek(vec) | (peek(vec + 1) << 8);
  if(g.cov) addEdge(pc, dst, nmi ? EDGE_NMI : EDGE_IRQ, modeOf(cpu));
}

static CpuWriteHandler origWrite;
static void statWrite(void* mem, uint32_t adr, uint8_t val) {
  uint8_t bank = adr >> 16;
  uint16_t a = adr & 0xffff;
  if((bank < 0x40 || (bank >= 0x80 && bank < 0xc0)) && a >= 0x2100 && a < 0x4400) {
    g.regWrites[a]++;
    if(a == 0x2105) g.bgmodeCount[val]++;
    if(a == 0x4200) g.nmitimenCount[val]++;
    if(a == 0x420c) g.hdmaenCount[val]++;
    if(a >= 0x2140 && a < 0x2144) g.apuWrites++;
  }
  origWrite(mem, adr, val);
}

static CpuReadHandler origRead;
static uint8_t statRead(void* mem, uint32_t adr) {
  uint8_t bank = adr >> 16;
  uint16_t a = adr & 0xffff;
  if((bank < 0x40 || (bank >= 0x80 && bank < 0xc0)) && a >= 0x2100 && a < 0x4400) {
    g.regReads[a]++;
    if(a >= 0x2140 && a < 0x2144) g.apuReads++;
  }
  return origRead(mem, adr);
}

// --- output ------------------------------------------------------------------

static void savePng(const char* path) {
  static uint8_t pixels[512 * 4 * 480];
  snes_setPixels(g.snes, pixels);
  FILE* f = fopen(path, "wb");
  if(!f) { perror(path); return; }
  png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  png_infop info = png_create_info_struct(png);
  png_init_io(png, f);
  png_set_IHDR(png, info, 256, 224, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
  png_write_info(png, info);
  uint8_t row[256 * 3];
  for(int y = 0; y < 224; y++) {
    const uint8_t* src = pixels + (16 + 2 * y) * 2048;
    for(int x = 0; x < 256; x++) {
      row[x * 3 + 0] = src[x * 8 + 7];
      row[x * 3 + 1] = src[x * 8 + 6];
      row[x * 3 + 2] = src[x * 8 + 5];
    }
    png_write_row(png, row);
  }
  png_write_end(png, NULL);
  png_destroy_write_struct(&png, &info);
  fclose(f);
}

static void saveDump(const char* path) {
  FILE* f = fopen(path, "wb");
  if(!f) { perror(path); return; }
  Ppu* ppu = g.snes->ppu;
  fwrite(g.snes->ram, 1, 0x20000, f);
  fwrite(ppu->vram, 2, 0x8000, f);
  fwrite(ppu->cgram, 2, 0x100, f);
  fwrite(ppu->oam, 2, 0x100, f);
  fwrite(ppu->highOam, 1, 0x20, f);
  fclose(f);
}

static void writeCoverage(const char* path) {
  FILE* f = fopen(path, "w");
  if(!f) { perror(path); return; }
  fprintf(f, "# snesrun coverage: I addr modes(bit m<<2|x<<1|e) D DB varies(bit0 D, bit1 DB) count\n");
  for(uint32_t a = 0; a < ADDR_SPACE; a++) {
    if(g.flags[a]) {
      fprintf(f, "I %06x %02x %04x %02x %x %u\n", a, g.flags[a], g.dpSeen[a], g.dbSeen[a], g.dpVar[a] & 3, g.counts[a]);
    }
  }
  for(int i = 0; i < g.edgeCap; i++) {
    struct Edge* e = &g.edges[i];
    if(e->count) fprintf(f, "%c %06x %06x %02x %u\n", e->kind, e->src, e->dst, 1 << e->mode, e->count);
  }
  fclose(f);
}

// --- input script ------------------------------------------------------------

static uint16_t parseButtons(const char* s, int n) {
  static const char* names[12] = {"b", "y", "select", "start", "up", "down", "left", "right", "a", "x", "l", "r"};
  uint16_t b = 0;
  int i = 0;
  while(i < n) {
    int j = i;
    while(j < n && s[j] != '+') j++;
    if(!(j - i == 1 && s[i] == '-')) {
      int k;
      for(k = 0; k < 12; k++) if((int)strlen(names[k]) == j - i && !strncmp(names[k], s + i, j - i)) break;
      if(k == 12) { fprintf(stderr, "unknown button '%.*s'\n", j - i, s + i); exit(1); }
      b |= 1 << k;
    }
    i = j + 1;
  }
  return b;
}

static KeyRun* parseKeys(const char* script, int* count) {
  KeyRun* runs = NULL;
  int n = 0, cap = 0;
  uint32_t frame = 1;
  const char* p = script;
  while(*p) {
    while(*p == ' ' || *p == ',') p++;
    if(!*p) break;
    const char* start = p;
    while(*p && *p != ' ' && *p != ',' && *p != '*') p++;
    int len = p - start;
    uint32_t frames = 1;
    if(*p == '*') { frames = strtoul(p + 1, (char**)&p, 10); }
    if(n == cap) { cap = cap ? cap * 2 : 64; runs = realloc(runs, cap * sizeof(KeyRun)); }
    runs[n].start = frame;
    runs[n].count = frames;
    runs[n].buttons = parseButtons(start, len);
    n++;
    frame += frames;
  }
  *count = n;
  return runs;
}

static bool inList(const char* list, int frame) {
  if(!list) return false;
  const char* p = list;
  while(*p) {
    int v = strtol(p, (char**)&p, 10);
    if(v == frame) return true;
    if(*p == ',') p++; else if(*p) p++;
  }
  return false;
}

static uint8_t* readFile(const char* path, int* size) {
  FILE* f = fopen(path, "rb");
  if(!f) return NULL;
  fseek(f, 0, SEEK_END);
  *size = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t* d = malloc(*size);
  if(fread(d, 1, *size, f) != (size_t)*size) { fclose(f); free(d); return NULL; }
  fclose(f);
  return d;
}

int main(int argc, char** argv) {
  if(argc < 2) {
    fprintf(stderr, "usage: snesrun ROM [FRAMES=n KEYS=... OUT=dir SHOTS=... DUMP=... COV=file ...]\n");
    return 1;
  }
  const char* opt[32][2];
  int nopt = 0;
  for(int i = 2; i < argc && nopt < 32; i++) {
    char* eq = strchr(argv[i], '=');
    if(!eq) { fprintf(stderr, "bad option %s\n", argv[i]); return 1; }
    *eq = 0;
    opt[nopt][0] = argv[i];
    opt[nopt][1] = eq + 1;
    nopt++;
  }
#define OPT(name) ({ const char* _v = NULL; for(int _i = 0; _i < nopt; _i++) if(!strcmp(opt[_i][0], name)) _v = opt[_i][1]; _v; })
  int frames = OPT("FRAMES") ? atoi(OPT("FRAMES")) : 600;
  g.outDir = OPT("OUT") ? OPT("OUT") : "out";
  mkdir(g.outDir, 0755);
  int nruns = 0;
  KeyRun* runs = OPT("KEYS") ? parseKeys(OPT("KEYS"), &nruns) : NULL;
  const char* shots = OPT("SHOTS");
  int shotEvery = OPT("SHOT_EVERY") ? atoi(OPT("SHOT_EVERY")) : 0;
  const char* dumps = OPT("DUMP");
  const char* covPath = OPT("COV");
  const char* countsPath = OPT("COUNTS");
  bool hash = OPT("HASH") != NULL;
  g.stats = OPT("STATS") != NULL;
  g.cov = covPath || countsPath;

  int romSize;
  uint8_t* rom = readFile(argv[1], &romSize);
  if(!rom) { fprintf(stderr, "can't read %s\n", argv[1]); return 1; }
  g.snes = snes_init();
  if(!snes_loadRom(g.snes, rom, romSize)) return 1;
  const char* sramPath = OPT("SRAM");
  if(sramPath) {
    int sz;
    uint8_t* sram = readFile(sramPath, &sz);
    if(sram) { snes_loadBattery(g.snes, sram, sz); free(sram); }
  }
  if(OPT("STATE")) {
    int sz;
    uint8_t* st = readFile(OPT("STATE"), &sz);
    if(!st || !snes_loadState(g.snes, st, sz)) { fprintf(stderr, "can't load state %s\n", OPT("STATE")); return 1; }
    free(st);
  }
  if(g.cov) {
    g.flags = calloc(ADDR_SPACE, 1);
    g.dpSeen = calloc(ADDR_SPACE, 2);
    g.dpVar = calloc(ADDR_SPACE, 1);
    g.dbSeen = calloc(ADDR_SPACE, 1);
    g.counts = calloc(ADDR_SPACE, 4);
  }
  g.snes->cpu->onOpcode = onOpcode;
  g.snes->cpu->onInterrupt = onInterrupt;
  origWrite = g.snes->cpu->write;
  origRead = g.snes->cpu->read;
  if(g.stats) {
    g.snes->cpu->write = statWrite;
    g.snes->cpu->read = statRead;
  }
  if(OPT("TRACE")) {
    char path[512];
    snprintf(path, sizeof(path), "%s/trace.txt", g.outDir);
    g.trace = fopen(path, "w");
    sscanf(OPT("TRACE"), "%d:%d", &g.traceFrom, &g.traceTo);
  }
  int stateOutFrame = -1;
  const char* stateOutPath = NULL;
  if(OPT("STATE_OUT")) {
    static char buf[512];
    snprintf(buf, sizeof(buf), "%s", OPT("STATE_OUT"));
    char* colon = strchr(buf, ':');
    if(colon) { *colon = 0; stateOutFrame = atoi(buf); stateOutPath = colon + 1; }
  }

  int run = 0;
  uint32_t rng = OPT("RANDOM") ? (uint32_t)atoi(OPT("RANDOM")) * 2654435761u + 1 : 0;
  int randomLeft = 0;
  uint16_t randomButtons = 0;
  int scriptEnd = nruns ? (int)(runs[nruns - 1].start + runs[nruns - 1].count) : 1;
  for(g.frame = 1; g.frame <= frames; g.frame++) {
    uint16_t buttons = 0;
    while(run < nruns && g.frame >= (int)(runs[run].start + runs[run].count)) run++;
    if(run < nruns && g.frame >= (int)runs[run].start) buttons = runs[run].buttons;
    if(rng && g.frame >= scriptEnd) {
      if(randomLeft-- <= 0) {
        rng = rng * 1103515245u + 12345u;
        uint32_t r = rng >> 8;
        // mostly single buttons, held for a few frames; directions held longer
        static const uint16_t choices[] = {0x010, 0x020, 0x040, 0x080, 0x010, 0x020, 0x040, 0x080, 0x100, 0x100, 0x100, 0x001, 0x001, 0x008, 0x200, 0x002, 0x004, 0x400, 0x800, 0};
        randomButtons = choices[r % (sizeof(choices) / sizeof(choices[0]))];
        randomLeft = (randomButtons & 0xf0) ? 4 + (r >> 8) % 40 : 1 + (r >> 8) % 4;
        if(!(randomButtons & 0xf0) && ((r >> 16) & 1)) randomButtons = 0, randomLeft = 2 + (r >> 9) % 6;
      }
      buttons = randomButtons;
    }
    g.snes->input1->currentState = buttons;
    snes_runFrame(g.snes);
    // collect HDMA targets at end of frame
    if(g.stats) {
      for(int c = 0; c < 8; c++) {
        DmaChannel* ch = &g.snes->dma->channel[c];
        if(ch->hdmaActive) g.hdmaTargets[c][ch->bAdr]++;
      }
    }
    if(hash) printf("frame %d %08x\n", g.frame, hashBytes(g.snes->ram, 0x20000));
    char path[512];
    if(inList(shots, g.frame) || (shotEvery && g.frame % shotEvery == 0)) {
      snprintf(path, sizeof(path), "%s/shot_%05d.png", g.outDir, g.frame);
      savePng(path);
    }
    if(inList(dumps, g.frame)) {
      snprintf(path, sizeof(path), "%s/dump_%05d.bin", g.outDir, g.frame);
      saveDump(path);
    }
    if(g.frame == stateOutFrame) {
      static uint8_t stateBuf[0x80000];
      int sz = snes_saveState(g.snes, stateBuf);
      FILE* f = fopen(stateOutPath, "wb");
      if(f) { fwrite(stateBuf, 1, sz, f); fclose(f); }
    }
  }
  if(covPath) writeCoverage(covPath);
  if(countsPath && !covPath) writeCoverage(countsPath);
  if(sramPath) {
    static uint8_t sram[0x20000];
    int sz = snes_saveBattery(g.snes, sram);
    FILE* f = fopen(sramPath, "wb");
    if(f) { fwrite(sram, 1, sz, f); fclose(f); }
  }
  if(g.stats) {
    printf("nmi %u irq %u apu reads %u writes %u\n", g.nmiCount, g.irqCount, g.apuReads, g.apuWrites);
    for(int i = 0; i < 256; i++) if(g.bgmodeCount[i]) printf("BGMODE %02x x%u\n", i, g.bgmodeCount[i]);
    for(int i = 0; i < 256; i++) if(g.nmitimenCount[i]) printf("NMITIMEN %02x x%u\n", i, g.nmitimenCount[i]);
    for(int i = 0; i < 256; i++) if(g.hdmaenCount[i]) printf("HDMAEN %02x x%u\n", i, g.hdmaenCount[i]);
    for(int c = 0; c < 8; c++) for(int i = 0; i < 256; i++) if(g.hdmaTargets[c][i]) printf("HDMA ch%d -> $21%02x frames %u\n", c, i, g.hdmaTargets[c][i]);
    for(int i = 0; i < 512; i++) if(g.irqLines[i]) printf("IRQ at line %d x%u\n", i, g.irqLines[i]);
    printf("register reads:");
    int nr = 0;
    for(int a = 0x2100; a < 0x4400; a++) if(g.regReads[a]) printf("%s%04x:%u", nr++ % 8 ? " " : "\n  ", a, g.regReads[a]);
    printf("\n");
    printf("register writes:");
    int n = 0;
    for(int a = 0x2100; a < 0x4400; a++) if(g.regWrites[a]) printf("%s%04x:%u", n++ % 8 ? " " : "\n  ", a, g.regWrites[a]);
    printf("\n");
  }
  if(g.trace) fclose(g.trace);
  return 0;
}
