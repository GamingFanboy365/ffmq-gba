"""Coverage files written by port/tools/snesrun (COV=...).

Lines:
  I addr modes d db varies count   an executed instruction: modes is a bit mask over
                                   (m << 2 | x << 1 | e); d/db the first D/DB seen and
                                   varies (bit 0 D, bit 1 DB) whether they changed
  C src dst modes count           a direct call (JSR/JSL), modes at the call
  J src dst modes count           an indirect jump or call (JMP (a), JMP (a,X), JML [a], JSR (a,X))
  R src dst modes count           a return that went elsewhere than its call's return address
  P src dst modes count           a return with no matching call: a pushed address used as a jump
  X src dst modes count           a transfer from code running in RAM to code elsewhere
  N/Q src dst modes count         an NMI/IRQ taken at src
"""

from collections import defaultdict


def mode_list(mask):
    """(m, x, e) tuples for a mode mask."""
    return [((b >> 2) & 1, (b >> 1) & 1, b & 1) for b in range(8) if mask & (1 << b)]


class Coverage:
    def __init__(self, paths=()):
        self.modes = {}                 # addr -> mode mask
        self.count = defaultdict(int)   # addr -> times executed
        self.dvar = {}                  # addr -> (d, db, varies)
        self.edges = defaultdict(int)   # (kind, src, dst, modes) -> count
        for p in paths:
            self.load(p)

    def load(self, path):
        for line in open(path):
            if not line.strip() or line.startswith('#'):
                continue
            f = line.split()
            if f[0] == 'I':
                a = int(f[1], 16)
                self.modes[a] = self.modes.get(a, 0) | int(f[2], 16)
                self.count[a] += int(f[6])
                d, db, var = int(f[3], 16), int(f[4], 16), int(f[5], 16)
                if a in self.dvar:
                    od, odb, ovar = self.dvar[a]
                    var |= ovar | (1 if od != d else 0) | (2 if odb != db else 0)
                    d, db = od, odb
                self.dvar[a] = (d, db, var)
            else:
                self.edges[(f[0], int(f[1], 16), int(f[2], 16), int(f[3], 16))] += int(f[4])

    def edges_of(self, kinds):
        for (k, s, d, m), n in self.edges.items():
            if k in kinds:
                yield k, s, d, m, n

    def mx_at(self, addr):
        """(m, x) pairs seen at an address (native mode only)."""
        return sorted({(m, x) for m, x, e in mode_list(self.modes.get(addr, 0)) if not e})
