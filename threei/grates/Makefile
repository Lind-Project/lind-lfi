# Example grates for the LFI 3i runtime (ported from lind-wasm-example-grates).
#
#   make            build all grates and their tests into out/
#   make check      build, then run every grate against its tests
#   make out/strace-grate
#
# Needs the LFI toolchain (x86_64_lfi-linux-musl-clang) on PATH and a built
# runtime; RUN points at lfi-3i-run.
ROOT   := $(abspath ../../)
LFICC  ?= x86_64_lfi-linux-musl-clang
RUN    ?= $(ROOT)/build/tools/lfi-3i-run/lfi-3i-run
CFLAGS ?= -O2 -g0 -Wall -Wno-unused-function -Wno-format -Wno-unused-variable \
          -Wno-unused-but-set-variable -Wno-unused-label
INC    := -I../include -I../guest/include
SHIM   := ../guest/lfi3i.c
OUT    := out

# Only the .c files are compiled; headers are listed as prerequisites so that
# editing one rebuilds the program.
LINK = $(LFICC) $(CFLAGS) $(INC) -static-pie -o $@ $(filter %.c,$^)

GRATES := geteuid-grate strace-grate seccomp-grate witness-grate imfs-grate
TESTS  := geteuid-test strace-test seccomp-chmod-test witness-exit-failure imfs-test exit-status seccomp-tamper-test

all: $(addprefix $(OUT)/,$(GRATES) $(TESTS))

$(OUT):
	mkdir -p $(OUT)

# ---- the grates ----------------------------------------------------------
$(OUT)/geteuid-grate: c/geteuid-grate/src/geteuid-grate.c $(SHIM) | $(OUT)
	$(LINK)
$(OUT)/strace-grate: c/strace-grate/src/strace-grate.c c/strace-grate/src/strace.c $(wildcard c/strace-grate/src/*.h) $(SHIM) | $(OUT)
	$(LINK)
$(OUT)/seccomp-grate: c/seccomp-grate/src/seccomp-grate.c c/seccomp-grate/src/seccomp.c $(wildcard c/seccomp-grate/src/*.h) $(SHIM) | $(OUT)
	$(LINK)
ED25519 := $(filter-out %/key_exchange.c,$(wildcard c/witness-grate/ed25519/*.c))
$(OUT)/witness-grate: c/witness-grate/src/witness-grate.c $(ED25519) $(SHIM) | $(OUT)
	$(LFICC) $(CFLAGS) $(INC) -Ic/witness-grate/ed25519 -static-pie -o $@ $(filter %.c,$^)
$(OUT)/imfs-grate: c/imfs-grate/src/imfs-grate.c c/imfs-grate/src/imfs.c $(wildcard c/imfs-grate/src/*.h) $(SHIM) | $(OUT)
	$(LINK)

# ---- the test cages (plain programs) --------------------------------------
$(OUT)/geteuid-test: c/geteuid-grate/test/geteuid.c | $(OUT)
	$(LINK)
$(OUT)/strace-test: c/strace-grate/test/strace_test.c | $(OUT)
	$(LINK)
$(OUT)/seccomp-chmod-test: c/seccomp-grate/test/seccomp_chmod_test.c | $(OUT)
	$(LINK)
$(OUT)/witness-exit-failure: c/witness-grate/tests/exit_failure.c | $(OUT)
	$(LINK)
$(OUT)/imfs-test: c/imfs-grate/test/imfs-test.c | $(OUT)
	$(LINK)
$(OUT)/seccomp-tamper-test: c/seccomp-grate/test/seccomp_tamper_test.c $(SHIM) | $(OUT)
	$(LINK)
$(OUT)/exit-status: c/test-common/exit_status.c | $(OUT)
	$(LINK)

# ---- Rust grates ------------------------------------------------------------
# Built with rust/cargo-lfi.sh (see rust/README.md); their test cages are the
# C programs in rust/<grate>/test/.
RUST_GRATES := $(patsubst rust/%/Cargo.toml,%,$(wildcard rust/*/Cargo.toml))
RUST_LIB    := $(shell find lib -name '*.rs' -o -name Cargo.toml) rust/cargo-lfi.sh rust/lfi-ld.py $(SHIM)
RUST_TESTS  := $(foreach g,$(RUST_GRATES),$(patsubst rust/$(g)/test/%.c,$(OUT)/rust-tests/$(g)/%,$(wildcard rust/$(g)/test/*.c)))

.SECONDEXPANSION:
$(OUT)/rust/%: $$(shell find rust/$$* -name '*.rs' -o -name Cargo.toml) $(RUST_LIB) | $(OUT)
	rust/cargo-lfi.sh rust/$* $@
define rust_test
$(OUT)/rust-tests/$(1)/$(2): rust/$(1)/test/$(2).c | $(OUT)
	@mkdir -p $$(dir $$@)
	$$(LINK)
endef
$(foreach g,$(RUST_GRATES),$(foreach c,$(wildcard rust/$(g)/test/*.c),$(eval $(call rust_test,$(g),$(basename $(notdir $(c)))))))

# The demos compose these grates; their test cages are in demos/<demo>/.
DEMO_TESTS := $(patsubst demos/%.c,$(OUT)/demo-tests/%,$(wildcard demos/*/*.c))
$(OUT)/demo-tests/%: demos/%.c | $(OUT)
	@mkdir -p $(dir $@)
	$(LINK)

rust: $(addprefix $(OUT)/rust/,$(RUST_GRATES)) $(RUST_TESTS) $(DEMO_TESTS)

check: all rust
	@RUN=$(RUN) OUT=$(OUT) ./run_tests.sh

clean:
	rm -rf $(OUT)

.PHONY: all rust check clean
