# Host-side tools for the FFMQ GBA port.
#   make -C port/tools          builds snesrun (needs a C compiler, libpng and zlib)

CC ?= cc
CFLAGS ?= -O2 -g
LAKESNES := ../third_party/lakesnes
LAKESNES_SRC := $(wildcard $(LAKESNES)/*.c)

all: snesrun

snesrun: snesrun.c $(LAKESNES_SRC) $(wildcard $(LAKESNES)/*.h)
	$(CC) $(CFLAGS) -w -I$(LAKESNES) -o $@ snesrun.c $(LAKESNES_SRC) -lpng -lz -lm

clean:
	rm -f snesrun

.PHONY: all clean
