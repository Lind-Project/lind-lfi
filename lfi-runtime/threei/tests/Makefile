# Builds and runs the 3i tests. Requires the LFI toolchain (x86_64_lfi-linux-musl-clang)
# on PATH and a built runtime (meson/ninja in ../..).
ROOT   := $(abspath ../..)
LFICC  ?= x86_64_lfi-linux-musl-clang
RUN    ?= $(ROOT)/build/tools/lfi-3i-run/lfi-3i-run
CFLAGS ?= -O2 -g0 -Wall -Wno-unused-function -Wno-format -Wno-unused-variable -Wno-unused-but-set-variable -Wno-infinite-recursion
INC    := -I../guest/include -I../include
OUT    := out

GUEST  := ../guest/lfi3i.c
# Cages: plain programs, no 3i library needed.
vpath %.c . lind
CAGES  := $(patsubst %.c,$(OUT)/%,$(notdir $(wildcard c_*.c lind/c_*.c)))
# Grates and tests that use the 3i API.
GRATES := $(patsubst %.c,$(OUT)/%,$(notdir $(wildcard g_*.c lind/g_*.c)))

all: $(CAGES) $(GRATES)

$(OUT):
	mkdir -p $(OUT)

$(OUT)/c_%: c_%.c | $(OUT)
	$(LFICC) $(CFLAGS) -static-pie -o $@ $<

$(OUT)/g_%: g_%.c $(GUEST) | $(OUT)
	$(LFICC) $(CFLAGS) $(INC) -static-pie -o $@ $< $(GUEST)

check: all
	@./run_tests.sh $(RUN) $(OUT)

clean:
	rm -rf $(OUT)

.PHONY: all check clean
