# Self-documenting Makefile for hl — aarch64-linux ELF executor on macOS
#
# Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
# SPDX-License-Identifier: Apache-2.0
#
# Usage:
#   make <target> [SIGN_IDENTITY="Your Signing Identity"]
#
# Example: make test-hello
#          make hl SIGN_IDENTITY="Apple Development: ..."

.PHONY: all hl clean dist pkg release test-hello test-all test-coreutils \
       test-busybox test-static-bins test-dynamic test-dynamic-coreutils \
       test-glibc-dynamic test-glibc-coreutils \
       test-perf test-multi-vcpu test-rwx test-haskell test-haskell-bins \
       test-x64-hello test-x64-all test-x64-coreutils test-x64-busybox \
       test-x64-static-bins \
       test-x64-musl-dynamic test-x64-musl-coreutils \
       test-x64-glibc-dynamic test-x64-glibc-coreutils \
       test-x64-haskell test-x64-haskell-bins \
       test-full \
       test-matrix test-matrix-hl-aarch64 test-matrix-hl-x64 \
       test-matrix-lima-aarch64 test-matrix-lima-x64 \
       lint analyze format shellcheck \
       site site-serve release-interactive help

# ── Configuration ──────────────────────────────────────────────────
ENTITLEMENTS := entitlements.plist
SIGN_IDENTITY ?= -
BUILD_DIR := _build
VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo "unknown")

# GNU objcopy for Mach-O → raw binary.  The clang wrapper in newer nixpkgs
# shadows binutils' objcopy with llvm-objcopy, which doesn't handle
# Mach-O -O binary correctly.  GNU_OBJCOPY is set by the nix flake.
ifdef GNU_OBJCOPY
  OBJCOPY := $(GNU_OBJCOPY)
else
  OBJCOPY ?= objcopy
endif

# Cross-compiler prefix (only used when GUEST_TEST_BINARIES is unset)
CROSS ?= aarch64-unknown-linux-musl-

# Test binary directory: pre-built from nix or locally cross-compiled.
# When GUEST_TEST_BINARIES is set (by nix develop), test binaries were
# already built on x86_64-linux and are ready to use.  Otherwise fall
# back to local cross-compilation via $(CROSS)gcc.
ifdef GUEST_TEST_BINARIES
  TEST_DIR  := $(GUEST_TEST_BINARIES)/bin
  TEST_DEPS :=
else
  TEST_DIR  := $(BUILD_DIR)
  TEST_C_SRCS := $(wildcard test/*.c)
  TEST_C_BINS := $(patsubst test/%.c,$(BUILD_DIR)/%,$(TEST_C_SRCS))
  TEST_DEPS := $(BUILD_DIR)/test-hello $(TEST_C_BINS)
endif

# Colors
GREEN  := \033[0;32m
BLUE   := \033[0;34m
CYAN   := \033[0;36m
YELLOW := \033[1;33m
RED    := \033[0;31m
RESET  := \033[0m

# ── Compiler flags ────────────────────────────────────────────────
CFLAGS := -O2 -Wall -Wextra -Wpedantic \
          -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
          -Wformat=2 -Wimplicit-fallthrough -Wundef \
          -Wnull-dereference -Wno-unused-parameter

# ── Source layout ─────────────────────────────────────────────────
SRC_DIR := src

HL_SRCS := $(addprefix $(SRC_DIR)/,hl.c guest.c elf.c syscall.c \
           syscall_fs.c syscall_io.c syscall_poll.c syscall_fd.c \
           syscall_inotify.c syscall_time.c syscall_sys.c \
           syscall_proc.c proc_emulation.c syscall_exec.c \
           fork_ipc.c syscall_signal.c syscall_net.c stack.c \
           thread.c futex.c vdso.c crash_report.c rosetta.c \
           gdb_stub.c)
HL_HDRS := $(addprefix $(SRC_DIR)/,guest.h elf.h syscall.h \
           syscall_internal.h syscall_fs.h syscall_io.h \
           syscall_poll.h syscall_fd.h syscall_inotify.h \
           syscall_time.h syscall_sys.h syscall_proc.h \
           proc_emulation.h syscall_exec.h fork_ipc.h \
           syscall_signal.h syscall_net.h stack.h hv_util.h \
           thread.h futex.h vdso.h crash_report.h rosetta.h \
           gdb_stub.h)

# ── Default target ─────────────────────────────────────────────────
.DEFAULT_GOAL := help

## Build everything (hl + all test binaries)
all: hl $(TEST_DEPS)

# ── Build directory ────────────────────────────────────────────────
$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# ── Version header ────────────────────────────────────────────────
# Regenerates when HEAD or index changes (commit/checkout/stage).
# cmp trick avoids unnecessary rebuilds when version is unchanged.
# Override: make hl VERSION=v0.1.0 (used by nix build in sandbox).
# When .git/ is absent (nix sandbox), skip git-file deps.
VERSION_DEPS := $(wildcard .git/HEAD .git/index)
$(BUILD_DIR)/version.h: $(VERSION_DEPS) | $(BUILD_DIR)
	@printf '#define HL_VERSION "%s"\n' "$(VERSION)" > $@.tmp
	@cmp -s $@.tmp $@ 2>/dev/null || mv $@.tmp $@
	@rm -f $@.tmp

# ── Shim binary blob ──────────────────────────────────────────────

$(BUILD_DIR)/shim.o: $(SRC_DIR)/shim.S | $(BUILD_DIR)
	@printf "$(GREEN)▸ Assembling$(RESET) shim.S\n"
	as -arch arm64 -o $@ $<

$(BUILD_DIR)/shim.bin: $(BUILD_DIR)/shim.o
	@printf "$(GREEN)▸ Creating binary$(RESET) shim.bin\n"
	$(OBJCOPY) -O binary $< $@

## Generate shim_blob.h from shim.bin (C byte array)
$(BUILD_DIR)/shim_blob.h: $(BUILD_DIR)/shim.bin
	@printf "$(GREEN)▸ Generating$(RESET) shim_blob.h\n"
	@printf 'static const unsigned char shim_bin[] = {\n' > $@
	xxd -i < $< >> $@
	@printf '};\nstatic const unsigned int shim_bin_len = sizeof(shim_bin);\n' >> $@

# ── hl executable ─────────────────────────────────────────────────

## Build the hl executable
hl: $(BUILD_DIR)/hl
	@printf "$(GREEN)✓ hl built successfully$(RESET)\n"

$(BUILD_DIR)/hl: $(HL_SRCS) $(HL_HDRS) $(BUILD_DIR)/shim_blob.h $(BUILD_DIR)/version.h | $(BUILD_DIR)
	@printf "$(GREEN)▸ Compiling$(RESET) hl\n"
	clang $(CFLAGS) \
		-I$(BUILD_DIR) -I$(SRC_DIR) \
		-o $@ $(HL_SRCS) \
		-framework Hypervisor -arch arm64
	@printf "$(GREEN)▸ Signing$(RESET) hl\n"
	codesign --entitlements $(ENTITLEMENTS) -f -s "$(SIGN_IDENTITY)" $@

# ── Test binaries (local cross-compilation fallback) ─────────────
# These rules are only used when GUEST_TEST_BINARIES is NOT set.

ifndef GUEST_TEST_BINARIES
# Assembly hello world (uses custom linker script)
$(BUILD_DIR)/test-hello: test/hello.S test/simple.ld | $(BUILD_DIR)
	@printf "$(GREEN)▸ Cross-assembling$(RESET) test/hello.S\n"
	$(CROSS)as -o $(BUILD_DIR)/test-hello.o test/hello.S
	@printf "$(GREEN)▸ Cross-linking$(RESET) test-hello\n"
	$(CROSS)ld -T test/simple.ld -o $@ $(BUILD_DIR)/test-hello.o

# Pattern rule: build static musl binaries from test/*.c
$(BUILD_DIR)/%: test/%.c | $(BUILD_DIR)
	@printf "$(GREEN)▸ Cross-compiling$(RESET) $<\n"
	$(CROSS)gcc -static -O2 -o $@ $<

# test-pthread needs -lpthread (musl static pthread support)
$(BUILD_DIR)/test-pthread: test/test-pthread.c | $(BUILD_DIR)
	@printf "$(GREEN)▸ Cross-compiling$(RESET) $< (with -lpthread)\n"
	$(CROSS)gcc -static -O2 -o $@ $< -lpthread
endif

# ── Test targets ──────────────────────────────────────────────────

## Build and run the assembly hello world test
test-hello: $(BUILD_DIR)/hl $(TEST_DEPS)
	@printf "$(BLUE)▸ Running$(RESET) test-hello\n"
	$(BUILD_DIR)/hl $(TEST_DIR)/test-hello

## Run all tests
test-all: $(BUILD_DIR)/hl $(TEST_DEPS)
	@printf "\n$(BLUE)━━━ Running test suite ━━━$(RESET)\n\n"
	@pass=0; fail=0; \
	run_test() { \
		name=$$(basename "$$2"); \
		printf "$(YELLOW)▸ %-20s$(RESET) " "$$name"; \
		if output=$$(timeout 60 $$@ 2>&1); then \
			printf "$(GREEN)✓ PASS$(RESET)\n"; \
			pass=$$((pass + 1)); \
		else \
			rc=$$?; \
			if [ "$$expected_rc" != "" ] && [ "$$rc" = "$$expected_rc" ]; then \
				printf "$(GREEN)✓ PASS$(RESET) (exit $$rc)\n"; \
				pass=$$((pass + 1)); \
			elif [ "$$rc" = "124" ]; then \
				printf "$(RED)✗ FAIL$(RESET) (timeout after 60s)\n"; \
				fail=$$((fail + 1)); \
			else \
				printf "$(RED)✗ FAIL$(RESET) (exit $$rc)\n"; \
				printf "  %s\n" "$$output" | head -5; \
				fail=$$((fail + 1)); \
			fi; \
		fi; \
	}; \
	printf "$(BLUE)── Assembly tests ──$(RESET)\n"; \
	run_test $(BUILD_DIR)/hl $(TEST_DIR)/test-hello; \
	printf "\n$(BLUE)── C tests (musl static) ──$(RESET)\n"; \
	run_test $(BUILD_DIR)/hl $(TEST_DIR)/hello-musl; \
	run_test $(BUILD_DIR)/hl $(TEST_DIR)/hello-write; \
	run_test $(BUILD_DIR)/hl $(TEST_DIR)/echo-test hello world; \
	run_test $(BUILD_DIR)/hl $(TEST_DIR)/test-argc a b c; \
	expected_rc=42 run_test $(BUILD_DIR)/hl $(TEST_DIR)/test-complex; \
	run_test $(BUILD_DIR)/hl $(TEST_DIR)/test-fileio CLAUDE.md; \
	run_test $(BUILD_DIR)/hl $(TEST_DIR)/test-string; \
	run_test $(BUILD_DIR)/hl $(TEST_DIR)/test-malloc; \
	run_test $(BUILD_DIR)/hl $(TEST_DIR)/test-cat test/hello.S; \
	run_test $(BUILD_DIR)/hl $(TEST_DIR)/test-ls test/; \
	run_test $(BUILD_DIR)/hl $(TEST_DIR)/test-roundtrip; \
	run_test $(BUILD_DIR)/hl $(TEST_DIR)/test-comprehensive; \
	printf "\n$(BLUE)── Process tests ──$(RESET)\n"; \
	run_test $(BUILD_DIR)/hl $(TEST_DIR)/test-exec $(TEST_DIR)/echo-test exec-works; \
	run_test $(BUILD_DIR)/hl $(TEST_DIR)/test-fork; \
	printf "\n$(BLUE)── Signal tests ──$(RESET)\n"; \
	run_test $(BUILD_DIR)/hl $(TEST_DIR)/test-signal; \
	printf "\n$(BLUE)── Socket tests ──$(RESET)\n"; \
	run_test $(BUILD_DIR)/hl $(TEST_DIR)/test-socket; \
	printf "\n$(BLUE)── Syscall coverage tests ──$(RESET)\n"; \
	run_test $(BUILD_DIR)/hl $(TEST_DIR)/test-file-ops; \
	run_test $(BUILD_DIR)/hl $(TEST_DIR)/test-sysinfo; \
	run_test $(BUILD_DIR)/hl $(TEST_DIR)/test-io-opt; \
	run_test $(BUILD_DIR)/hl $(TEST_DIR)/test-poll; \
	printf "\n$(BLUE)── I/O subsystem tests ──$(RESET)\n"; \
	run_test $(BUILD_DIR)/hl $(TEST_DIR)/test-eventfd; \
	run_test $(BUILD_DIR)/hl $(TEST_DIR)/test-signalfd; \
	run_test $(BUILD_DIR)/hl $(TEST_DIR)/test-epoll; \
	run_test $(BUILD_DIR)/hl $(TEST_DIR)/test-timerfd; \
	printf "\n$(BLUE)── /proc and /dev emulation tests ──$(RESET)\n"; \
	run_test $(BUILD_DIR)/hl $(TEST_DIR)/test-proc; \
	printf "\n$(BLUE)── Network tests ──$(RESET)\n"; \
	run_test $(BUILD_DIR)/hl $(TEST_DIR)/test-net; \
	printf "\n$(BLUE)── Threading tests ──$(RESET)\n"; \
	run_test $(BUILD_DIR)/hl $(TEST_DIR)/test-thread; \
	run_test $(BUILD_DIR)/hl $(TEST_DIR)/test-pthread; \
	printf "\n$(BLUE)── Stress tests ──$(RESET)\n"; \
	run_test $(BUILD_DIR)/hl $(TEST_DIR)/test-stress; \
	printf "\n$(BLUE)── Negative / error-path tests ──$(RESET)\n"; \
	run_test $(BUILD_DIR)/hl $(TEST_DIR)/test-negative; \
	printf "\n$(BLUE)── Signal + thread tests ──$(RESET)\n"; \
	run_test $(BUILD_DIR)/hl $(TEST_DIR)/test-signal-thread; \
	printf "\n$(BLUE)── Fork edge cases ──$(RESET)\n"; \
	run_test $(BUILD_DIR)/hl $(TEST_DIR)/test-fork-exec $(TEST_DIR)/echo-test; \
	printf "\n$(BLUE)── COW fork isolation tests ──$(RESET)\n"; \
	run_test $(BUILD_DIR)/hl $(TEST_DIR)/test-cow-fork; \
	printf "\n$(BLUE)── O_CLOEXEC tests ──$(RESET)\n"; \
	run_test $(BUILD_DIR)/hl $(TEST_DIR)/test-cloexec; \
	printf "\n$(BLUE)── Guard page / mmap edge cases ──$(RESET)\n"; \
	run_test $(BUILD_DIR)/hl $(TEST_DIR)/test-guard-page; \
	printf "\n$(BLUE)── Scatter-gather I/O tests ──$(RESET)\n"; \
	run_test $(BUILD_DIR)/hl $(TEST_DIR)/test-readv-writev; \
	printf "\n$(BLUE)── inotify emulation tests ──$(RESET)\n"; \
	run_test $(BUILD_DIR)/hl $(TEST_DIR)/test-inotify; \
	printf "\n$(BLUE)── PI futex + EINTR regression tests ──$(RESET)\n"; \
	run_test $(BUILD_DIR)/hl $(TEST_DIR)/test-futex-pi; \
	printf "\n$(BLUE)── SIGILL / null guard tests ──$(RESET)\n"; \
	run_test $(BUILD_DIR)/hl $(TEST_DIR)/test-sigill; \
	printf "\n$(BLUE)── X11 raw protocol tests ──$(RESET)\n"; \
	run_test $(BUILD_DIR)/hl $(TEST_DIR)/test-x11; \
	printf "\n$(BLUE)━━━ Results: $$pass passed, $$fail failed ━━━$(RESET)\n"; \
	[ "$$fail" -eq 0 ]

# ── Coreutils integration test ───────────────────────────────────

# Path to static aarch64-linux-musl coreutils bin directory.
# Auto-detected from GUEST_COREUTILS env var (set by nix develop); override with COREUTILS_BIN=...
COREUTILS_BIN ?= $(GUEST_COREUTILS)/bin

## Run GNU coreutils 9.9 integration tests (104 tools)
test-coreutils: $(BUILD_DIR)/hl
	@if [ ! -d "$(COREUTILS_BIN)" ]; then \
		printf "$(RED)✗ Coreutils not found.$(RESET) Build first:\n"; \
		printf "  nix build --impure --expr 'let nixpkgs = import <nixpkgs> {}; in nixpkgs.pkgsCross.aarch64-multiplatform-musl.pkgsStatic.coreutils'\n"; \
		exit 1; \
	fi
	@bash test/test-coreutils.sh $(BUILD_DIR)/hl $(COREUTILS_BIN)

# ── Busybox integration test ─────────────────────────────────────

BUSYBOX_BIN ?= $(GUEST_BUSYBOX)/bin/busybox

## Run busybox applet smoke tests
test-busybox: $(BUILD_DIR)/hl
	@if [ ! -x "$(BUSYBOX_BIN)" ]; then \
		printf "$(RED)✗ Busybox not found in PATH.$(RESET) Run inside nix develop.\n"; \
		exit 1; \
	fi
	@bash test/test-busybox.sh $(BUILD_DIR)/hl $(BUSYBOX_BIN)

# ── Static binary integration tests ──────────────────────────────

STATIC_BINS_DIR ?= $(GUEST_STATIC_BINS)/bin

## Run static binary smoke tests (bash, lua, gawk, jq, sqlite, etc.)
test-static-bins: $(BUILD_DIR)/hl
	@if [ ! -d "$(STATIC_BINS_DIR)" ]; then \
		printf "$(RED)✗ Static bins not found.$(RESET) Run inside nix develop.\n"; \
		exit 1; \
	fi
	@bash test/test-static-bins.sh $(BUILD_DIR)/hl $(STATIC_BINS_DIR)

# ── Dynamic linking tests ────────────────────────────────────────

# Musl sysroot with dynamic linker + libc.so (set by nix develop)
SYSROOT_DIR ?= $(GUEST_SYSROOT)
DYNAMIC_COREUTILS_BIN ?= $(GUEST_DYNAMIC_COREUTILS)/bin

## Run dynamic linking smoke test (hello-dynamic via --sysroot)
test-dynamic: $(BUILD_DIR)/hl
	@if [ -z "$(SYSROOT_DIR)" ] || [ ! -d "$(SYSROOT_DIR)" ]; then \
		printf "$(RED)✗ Sysroot not found.$(RESET) Run inside nix develop.\n"; \
		exit 1; \
	fi
	@printf "$(BLUE)▸ Running$(RESET) dynamic hello-dynamic (--sysroot)\n"
	$(BUILD_DIR)/hl --sysroot $(SYSROOT_DIR) $(GUEST_DYNAMIC_TESTS)/bin/hello-dynamic

## Run dynamically-linked coreutils tests (--sysroot)
test-dynamic-coreutils: $(BUILD_DIR)/hl
	@if [ -z "$(SYSROOT_DIR)" ] || [ ! -d "$(SYSROOT_DIR)" ]; then \
		printf "$(RED)✗ Sysroot not found.$(RESET) Run inside nix develop.\n"; \
		exit 1; \
	fi
	@if [ ! -d "$(DYNAMIC_COREUTILS_BIN)" ]; then \
		printf "$(RED)✗ Dynamic coreutils not found.$(RESET) Run inside nix develop.\n"; \
		exit 1; \
	fi
	@bash test/test-dynamic-coreutils.sh $(BUILD_DIR)/hl $(SYSROOT_DIR) $(DYNAMIC_COREUTILS_BIN)

# ── glibc dynamic linking tests ───────────────────────────────────

# glibc sysroot with dynamic linker + libc.so (set by nix develop)
GLIBC_SYSROOT_DIR ?= $(GUEST_GLIBC_SYSROOT)
GLIBC_DYNAMIC_COREUTILS_BIN ?= $(GUEST_GLIBC_DYNAMIC_COREUTILS)/bin

## Run glibc dynamic linking smoke test (hello-dynamic via --sysroot)
test-glibc-dynamic: $(BUILD_DIR)/hl
	@if [ -z "$(GLIBC_SYSROOT_DIR)" ] || [ ! -d "$(GLIBC_SYSROOT_DIR)" ]; then \
		printf "$(RED)✗ glibc sysroot not found.$(RESET) Run inside nix develop.\n"; \
		exit 1; \
	fi
	@printf "$(BLUE)▸ Running$(RESET) glibc hello-dynamic (--sysroot)\n"
	$(BUILD_DIR)/hl --sysroot $(GLIBC_SYSROOT_DIR) $(GUEST_GLIBC_DYNAMIC_TESTS)/bin/hello-dynamic

## Run glibc dynamically-linked coreutils tests (--sysroot)
test-glibc-coreutils: $(BUILD_DIR)/hl
	@if [ -z "$(GLIBC_SYSROOT_DIR)" ] || [ ! -d "$(GLIBC_SYSROOT_DIR)" ]; then \
		printf "$(RED)✗ glibc sysroot not found.$(RESET) Run inside nix develop.\n"; \
		exit 1; \
	fi
	@if [ ! -d "$(GLIBC_DYNAMIC_COREUTILS_BIN)" ]; then \
		printf "$(RED)✗ glibc dynamic coreutils not found.$(RESET) Run inside nix develop.\n"; \
		exit 1; \
	fi
	@bash test/test-glibc-coreutils.sh $(BUILD_DIR)/hl $(GLIBC_SYSROOT_DIR) $(GLIBC_DYNAMIC_COREUTILS_BIN)

# ── Performance benchmark ─────────────────────────────────────────

## Run performance benchmarks (native vs hl, 10 iterations each)
test-perf: $(BUILD_DIR)/hl
	@if [ ! -d "$(COREUTILS_BIN)" ]; then \
		printf "$(RED)✗ Coreutils not found.$(RESET) Run inside nix develop.\n"; \
		exit 1; \
	fi
	@bash test/test-perf.sh $(BUILD_DIR)/hl $(COREUTILS_BIN)

# ── Haskell test ──────────────────────────────────────────────────

HASKELL_HELLO ?= $(GUEST_HASKELL_HELLO)/bin/hello-hyper

## Run Haskell hello world (GHC-produced aarch64-linux-musl ELF)
test-haskell: $(BUILD_DIR)/hl
	@if [ ! -x "$(HASKELL_HELLO)" ]; then \
		printf "$(RED)✗ Haskell hello not found.$(RESET) Run inside nix develop.\n"; \
		exit 1; \
	fi
	@printf "$(BLUE)▸ Running$(RESET) Haskell hello-hyper\n"
	$(BUILD_DIR)/hl --sysroot $(SYSROOT_DIR) $(HASKELL_HELLO)

# ── Haskell binary integration tests ────────────────────────────────

HASKELL_BINS_DIR ?= $(GUEST_HASKELL_BINS)/bin
HASKELL_SYSROOT ?=

## Run Haskell binary integration tests (pandoc, shellcheck)
test-haskell-bins: $(BUILD_DIR)/hl
	@if [ ! -d "$(HASKELL_BINS_DIR)" ]; then \
		printf "$(RED)✗ Haskell bins not found.$(RESET) Run inside nix develop.\n"; \
		exit 1; \
	fi
	@bash test/test-haskell-bins.sh $(BUILD_DIR)/hl $(HASKELL_BINS_DIR) $(HASKELL_SYSROOT)

# ── x86_64-linux via Rosetta tests ─────────────────────────────────

# x86_64-linux-musl test binary directory (set by nix develop)
X64_TEST_DIR ?= $(GUEST_X64_TEST_BINARIES)/bin
X64_COREUTILS_BIN ?= $(GUEST_X64_COREUTILS)/bin
X64_BUSYBOX_BIN ?= $(GUEST_X64_BUSYBOX)/bin/busybox

## Run x86_64 assembly hello world via rosetta
test-x64-hello: $(BUILD_DIR)/hl
	@if [ ! -d "$(X64_TEST_DIR)" ]; then \
		printf "$(RED)✗ x86_64 test binaries not found.$(RESET) Run inside nix develop.\n"; \
		exit 1; \
	fi
	@printf "$(BLUE)▸ Running$(RESET) x86_64 test-hello (via rosetta)\n"
	$(BUILD_DIR)/hl $(X64_TEST_DIR)/test-hello

## Run x86_64 test suite (same tests as aarch64, via rosetta)
test-x64-all: $(BUILD_DIR)/hl
	@if [ ! -d "$(X64_TEST_DIR)" ]; then \
		printf "$(RED)✗ x86_64 test binaries not found.$(RESET) Run inside nix develop.\n"; \
		exit 1; \
	fi
	@printf "\n$(BLUE)━━━ Running x86_64 test suite (via rosetta) ━━━$(RESET)\n\n"
	@pass=0; fail=0; xfail=0; \
	run_test() { \
		name=$$(basename "$$2"); \
		printf "$(YELLOW)▸ %-20s$(RESET) " "$$name"; \
		if output=$$(timeout 60 $$@ 2>&1); then \
			printf "$(GREEN)✓ PASS$(RESET)\n"; \
			pass=$$((pass + 1)); \
		else \
			rc=$$?; \
			if [ "$$expected_rc" != "" ] && [ "$$rc" = "$$expected_rc" ]; then \
				printf "$(GREEN)✓ PASS$(RESET) (exit $$rc)\n"; \
				pass=$$((pass + 1)); \
			elif [ "$$rc" = "124" ]; then \
				printf "$(RED)✗ FAIL$(RESET) (timeout after 60s)\n"; \
				fail=$$((fail + 1)); \
			else \
				printf "$(RED)✗ FAIL$(RESET) (exit $$rc)\n"; \
				printf "  %s\n" "$$output" | head -5; \
				fail=$$((fail + 1)); \
			fi; \
		fi; \
	}; \
	run_xfail() { \
		name=$$1; reason=$$2; \
		printf "$(YELLOW)▸ %-20s$(RESET) $(BLUE)⊘ XFAIL$(RESET) (%s)\n" "$$name" "$$reason"; \
		xfail=$$((xfail + 1)); \
	}; \
	printf "$(BLUE)── Assembly tests (x86_64) ──$(RESET)\n"; \
	run_test $(BUILD_DIR)/hl $(X64_TEST_DIR)/test-hello; \
	printf "\n$(BLUE)── C tests (x86_64 musl static, via rosetta) ──$(RESET)\n"; \
	run_test $(BUILD_DIR)/hl $(X64_TEST_DIR)/hello-musl; \
	run_test $(BUILD_DIR)/hl $(X64_TEST_DIR)/hello-write; \
	run_test $(BUILD_DIR)/hl $(X64_TEST_DIR)/echo-test hello world; \
	run_test $(BUILD_DIR)/hl $(X64_TEST_DIR)/test-argc arg1 arg2; \
	expected_rc=42 run_test $(BUILD_DIR)/hl $(X64_TEST_DIR)/test-complex; \
	run_test $(BUILD_DIR)/hl $(X64_TEST_DIR)/test-fileio CLAUDE.md; \
	run_test $(BUILD_DIR)/hl $(X64_TEST_DIR)/test-string; \
	run_test $(BUILD_DIR)/hl $(X64_TEST_DIR)/test-malloc; \
	run_test $(BUILD_DIR)/hl $(X64_TEST_DIR)/test-cat test/hello.S; \
	run_test $(BUILD_DIR)/hl $(X64_TEST_DIR)/test-ls test/; \
	run_test $(BUILD_DIR)/hl $(X64_TEST_DIR)/test-roundtrip; \
	run_test $(BUILD_DIR)/hl $(X64_TEST_DIR)/test-comprehensive; \
	printf "\n$(BLUE)── Process tests (x86_64) ──$(RESET)\n"; \
	run_test $(BUILD_DIR)/hl $(X64_TEST_DIR)/test-exec $(X64_TEST_DIR)/echo-test exec-works; \
	run_test $(BUILD_DIR)/hl $(X64_TEST_DIR)/test-fork; \
	run_test $(BUILD_DIR)/hl $(X64_TEST_DIR)/test-fork-exec $(X64_TEST_DIR)/echo-test exec-works; \
	printf "\n$(BLUE)── Signal tests (x86_64) ──$(RESET)\n"; \
	run_xfail test-signal "rosetta: SA_RESETHAND not reset (also fails in Lima, 3/4 subtests pass)"; \
	printf "\n$(BLUE)── Socket tests (x86_64) ──$(RESET)\n"; \
	run_test $(BUILD_DIR)/hl $(X64_TEST_DIR)/test-socket; \
	printf "\n$(BLUE)── Syscall coverage tests (x86_64) ──$(RESET)\n"; \
	run_test $(BUILD_DIR)/hl $(X64_TEST_DIR)/test-file-ops; \
	run_test $(BUILD_DIR)/hl $(X64_TEST_DIR)/test-sysinfo; \
	run_test $(BUILD_DIR)/hl $(X64_TEST_DIR)/test-io-opt; \
	run_test $(BUILD_DIR)/hl $(X64_TEST_DIR)/test-poll; \
	printf "\n$(BLUE)── I/O subsystem tests (x86_64) ──$(RESET)\n"; \
	run_test $(BUILD_DIR)/hl $(X64_TEST_DIR)/test-eventfd; \
	run_test $(BUILD_DIR)/hl $(X64_TEST_DIR)/test-signalfd; \
	run_test $(BUILD_DIR)/hl $(X64_TEST_DIR)/test-epoll; \
	run_test $(BUILD_DIR)/hl $(X64_TEST_DIR)/test-timerfd; \
	printf "\n$(BLUE)── /proc and /dev emulation tests (x86_64) ──$(RESET)\n"; \
	run_test $(BUILD_DIR)/hl $(X64_TEST_DIR)/test-proc; \
	printf "\n$(BLUE)── Network tests (x86_64) ──$(RESET)\n"; \
	run_test $(BUILD_DIR)/hl $(X64_TEST_DIR)/test-net; \
	printf "\n$(BLUE)── Threading tests (x86_64) ──$(RESET)\n"; \
	run_xfail test-thread "rosetta: raw clone(CLONE_THREAD) hangs (also hangs in Lima)"; \
	run_test $(BUILD_DIR)/hl $(X64_TEST_DIR)/test-pthread; \
	printf "\n$(BLUE)── Stress tests (x86_64) ──$(RESET)\n"; \
	run_xfail test-stress "rosetta: raw clone hangs (also hangs in Lima)"; \
	printf "\n$(BLUE)── Negative / error-path tests (x86_64) ──$(RESET)\n"; \
	run_test $(BUILD_DIR)/hl $(X64_TEST_DIR)/test-negative; \
	printf "\n$(BLUE)── Signal + thread tests (x86_64) ──$(RESET)\n"; \
	run_xfail test-signal-thread "rosetta: SA_RESETHAND not reset (also fails in Lima, 4/5 subtests pass)"; \
	printf "\n$(BLUE)── O_CLOEXEC tests (x86_64) ──$(RESET)\n"; \
	run_test $(BUILD_DIR)/hl $(X64_TEST_DIR)/test-cloexec; \
	printf "\n$(BLUE)── Guard page / mmap edge cases (x86_64) ──$(RESET)\n"; \
	run_test $(BUILD_DIR)/hl $(X64_TEST_DIR)/test-guard-page; \
	printf "\n$(BLUE)── Scatter-gather I/O tests (x86_64) ──$(RESET)\n"; \
	run_test $(BUILD_DIR)/hl $(X64_TEST_DIR)/test-readv-writev; \
	printf "\n$(BLUE)── inotify emulation tests (x86_64) ──$(RESET)\n"; \
	run_test $(BUILD_DIR)/hl $(X64_TEST_DIR)/test-inotify; \
	printf "\n$(BLUE)── COW fork isolation tests (x86_64) ──$(RESET)\n"; \
	run_test $(BUILD_DIR)/hl $(X64_TEST_DIR)/test-cow-fork; \
	printf "\n$(BLUE)── PI futex + EINTR regression tests (x86_64) ──$(RESET)\n"; \
	run_xfail test-futex-pi "rosetta: raw clone(CLONE_THREAD) in dead-owner test hangs"; \
	printf "\n$(BLUE)── SIGILL / null guard tests (x86_64) ──$(RESET)\n"; \
	run_test $(BUILD_DIR)/hl $(X64_TEST_DIR)/test-sigill; \
	printf "\n$(BLUE)── X11 raw protocol tests (x86_64) ──$(RESET)\n"; \
	run_test $(BUILD_DIR)/hl $(X64_TEST_DIR)/test-x11; \
	printf "\n$(BLUE)━━━ x86_64 Results: $$pass passed, $$fail failed, $$xfail xfail ━━━$(RESET)\n"; \
	[ "$$fail" -eq 0 ]

## Run x86_64 coreutils integration tests (via rosetta)
test-x64-coreutils: $(BUILD_DIR)/hl
	@if [ ! -d "$(X64_COREUTILS_BIN)" ]; then \
		printf "$(RED)✗ x86_64 coreutils not found.$(RESET) Run inside nix develop.\n"; \
		exit 1; \
	fi
	@bash test/test-coreutils.sh $(BUILD_DIR)/hl $(X64_COREUTILS_BIN)

## Run x86_64 busybox integration tests (via rosetta)
test-x64-busybox: $(BUILD_DIR)/hl
	@if [ ! -x "$(X64_BUSYBOX_BIN)" ]; then \
		printf "$(RED)✗ x86_64 busybox not found.$(RESET) Run inside nix develop.\n"; \
		exit 1; \
	fi
	@bash test/test-busybox.sh $(BUILD_DIR)/hl $(X64_BUSYBOX_BIN)

X64_STATIC_BINS_DIR ?= $(GUEST_X64_STATIC_BINS)/bin

## Run x86_64 static binary smoke tests via rosetta (bash, lua, jq, sqlite, etc.)
test-x64-static-bins: $(BUILD_DIR)/hl
	@if [ ! -d "$(X64_STATIC_BINS_DIR)" ]; then \
		printf "$(RED)✗ x86_64 static bins not found.$(RESET) Run inside nix develop.\n"; \
		exit 1; \
	fi
	@bash test/test-static-bins.sh $(BUILD_DIR)/hl $(X64_STATIC_BINS_DIR)

# ── x86_64 musl dynamic linking tests (Rosetta) ──────────────────

X64_MUSL_SYSROOT_DIR ?= $(GUEST_X64_MUSL_SYSROOT)
X64_MUSL_DYNAMIC_COREUTILS_BIN ?= $(GUEST_X64_MUSL_DYNAMIC_COREUTILS)/bin

## Run x86_64 musl dynamic linking smoke test (via rosetta --sysroot)
test-x64-musl-dynamic: $(BUILD_DIR)/hl
	@if [ -z "$(X64_MUSL_SYSROOT_DIR)" ] || [ ! -d "$(X64_MUSL_SYSROOT_DIR)" ]; then \
		printf "$(RED)✗ x86_64 musl sysroot not found.$(RESET) Run inside nix develop.\n"; \
		exit 1; \
	fi
	@printf "$(BLUE)▸ Running$(RESET) x86_64 musl hello-dynamic (via rosetta --sysroot)\n"
	$(BUILD_DIR)/hl --sysroot $(X64_MUSL_SYSROOT_DIR) $(GUEST_X64_MUSL_DYNAMIC_TESTS)/bin/hello-dynamic

## Run x86_64 musl dynamically-linked coreutils tests (via rosetta --sysroot)
test-x64-musl-coreutils: $(BUILD_DIR)/hl
	@if [ -z "$(X64_MUSL_SYSROOT_DIR)" ] || [ ! -d "$(X64_MUSL_SYSROOT_DIR)" ]; then \
		printf "$(RED)✗ x86_64 musl sysroot not found.$(RESET) Run inside nix develop.\n"; \
		exit 1; \
	fi
	@if [ ! -d "$(X64_MUSL_DYNAMIC_COREUTILS_BIN)" ]; then \
		printf "$(RED)✗ x86_64 musl dynamic coreutils not found.$(RESET) Run inside nix develop.\n"; \
		exit 1; \
	fi
	@bash test/test-dynamic-coreutils.sh $(BUILD_DIR)/hl $(X64_MUSL_SYSROOT_DIR) $(X64_MUSL_DYNAMIC_COREUTILS_BIN)

# ── x86_64 Haskell hello test ─────────────────────────────────────

X64_HASKELL_HELLO ?= $(GUEST_X64_HASKELL_HELLO)/bin/hello-hyper

## Run x86_64 Haskell hello world via rosetta (GHC-produced x86_64-linux-musl ELF)
test-x64-haskell: $(BUILD_DIR)/hl
	@if [ ! -x "$(X64_HASKELL_HELLO)" ]; then \
		printf "$(RED)✗ x86_64 Haskell hello not found.$(RESET) Run inside nix develop.\n"; \
		exit 1; \
	fi
	@printf "$(BLUE)▸ Running$(RESET) x86_64 Haskell hello-hyper (via rosetta)\n"
	$(BUILD_DIR)/hl --sysroot $(X64_MUSL_SYSROOT_DIR) $(X64_HASKELL_HELLO)

# ── x86_64 glibc dynamic linking tests (Rosetta) ─────────────────

X64_GLIBC_SYSROOT_DIR ?= $(GUEST_X64_GLIBC_SYSROOT)
X64_GLIBC_DYNAMIC_COREUTILS_BIN ?= $(GUEST_X64_GLIBC_DYNAMIC_COREUTILS)/bin

## Run x86_64 glibc dynamic linking smoke test (via rosetta --sysroot)
test-x64-glibc-dynamic: $(BUILD_DIR)/hl
	@if [ -z "$(X64_GLIBC_SYSROOT_DIR)" ] || [ ! -d "$(X64_GLIBC_SYSROOT_DIR)" ]; then \
		printf "$(RED)✗ x86_64 glibc sysroot not found.$(RESET) Run inside nix develop.\n"; \
		exit 1; \
	fi
	@printf "$(BLUE)▸ Running$(RESET) x86_64 glibc hello-dynamic (via rosetta --sysroot)\n"
	$(BUILD_DIR)/hl --sysroot $(X64_GLIBC_SYSROOT_DIR) $(GUEST_X64_GLIBC_DYNAMIC_TESTS)/bin/hello-dynamic

## Run x86_64 glibc dynamically-linked coreutils tests (via rosetta --sysroot)
test-x64-glibc-coreutils: $(BUILD_DIR)/hl
	@if [ -z "$(X64_GLIBC_SYSROOT_DIR)" ] || [ ! -d "$(X64_GLIBC_SYSROOT_DIR)" ]; then \
		printf "$(RED)✗ x86_64 glibc sysroot not found.$(RESET) Run inside nix develop.\n"; \
		exit 1; \
	fi
	@if [ ! -d "$(X64_GLIBC_DYNAMIC_COREUTILS_BIN)" ]; then \
		printf "$(RED)✗ x86_64 glibc dynamic coreutils not found.$(RESET) Run inside nix develop.\n"; \
		exit 1; \
	fi
	@bash test/test-x64-glibc-coreutils.sh $(BUILD_DIR)/hl $(X64_GLIBC_SYSROOT_DIR) $(X64_GLIBC_DYNAMIC_COREUTILS_BIN)

# ── x86_64 Haskell binary integration tests ────────────────────────

X64_HASKELL_BINS_DIR ?= $(GUEST_X64_HASKELL_BINS)/bin
X64_HASKELL_SYSROOT ?=

## Run x86_64 Haskell binary integration tests (pandoc, shellcheck via rosetta)
test-x64-haskell-bins: $(BUILD_DIR)/hl
	@if [ ! -d "$(X64_HASKELL_BINS_DIR)" ]; then \
		printf "$(RED)✗ x86_64 Haskell bins not found.$(RESET) Run inside nix develop.\n"; \
		exit 1; \
	fi
	@bash test/test-haskell-bins.sh $(BUILD_DIR)/hl $(X64_HASKELL_BINS_DIR) $(X64_HASKELL_SYSROOT)

# ── Test matrix (4-way: hl + lima, aarch64 + x86_64) ────────────────

## Run full test matrix (all modes: hl + lima, aarch64 + x86_64)
test-matrix: $(BUILD_DIR)/hl
	@bash test/test-matrix.sh all

## Run test matrix: hl aarch64 mode
test-matrix-hl-aarch64: $(BUILD_DIR)/hl
	@bash test/test-matrix.sh hl-aarch64

## Run test matrix: hl x86_64 (rosetta) mode
test-matrix-hl-x64: $(BUILD_DIR)/hl
	@bash test/test-matrix.sh hl-x64

## Run test matrix: Lima aarch64 mode
test-matrix-lima-aarch64: $(BUILD_DIR)/hl
	@bash test/test-matrix.sh lima-aarch64

## Run test matrix: Lima x86_64 (rosetta) mode
test-matrix-lima-x64: $(BUILD_DIR)/hl
	@bash test/test-matrix.sh lima-x64

# ── Full test suite ──────────────────────────────────────────────────

## Run the complete test suite (aarch64 + x86_64 + coreutils + busybox + static + dynamic + haskell)
test-full: $(BUILD_DIR)/hl
	@printf "\n$(CYAN)╔══════════════════════════════════════════════════════╗$(RESET)\n"
	@printf "$(CYAN)║              hl full test suite                      ║$(RESET)\n"
	@printf "$(CYAN)╚══════════════════════════════════════════════════════╝$(RESET)\n"
	@fail=0; \
	printf "\n$(BLUE)━━━ [1/16] aarch64 unit tests ━━━$(RESET)\n"; \
	$(MAKE) --no-print-directory test-all || fail=$$((fail + 1)); \
	printf "\n$(BLUE)━━━ [2/16] aarch64 coreutils (static musl) ━━━$(RESET)\n"; \
	$(MAKE) --no-print-directory test-coreutils || fail=$$((fail + 1)); \
	printf "\n$(BLUE)━━━ [3/16] aarch64 busybox ━━━$(RESET)\n"; \
	$(MAKE) --no-print-directory test-busybox || fail=$$((fail + 1)); \
	printf "\n$(BLUE)━━━ [4/16] aarch64 static bins (bash, jq, sqlite, lua, ...) ━━━$(RESET)\n"; \
	$(MAKE) --no-print-directory test-static-bins || fail=$$((fail + 1)); \
	printf "\n$(BLUE)━━━ [5/16] aarch64 dynamic coreutils (musl --sysroot) ━━━$(RESET)\n"; \
	$(MAKE) --no-print-directory test-dynamic-coreutils || fail=$$((fail + 1)); \
	printf "\n$(BLUE)━━━ [6/16] aarch64 dynamic coreutils (glibc --sysroot) ━━━$(RESET)\n"; \
	$(MAKE) --no-print-directory test-glibc-coreutils || fail=$$((fail + 1)); \
	printf "\n$(BLUE)━━━ [7/16] aarch64 haskell bins (pandoc, shellcheck) ━━━$(RESET)\n"; \
	$(MAKE) --no-print-directory test-haskell-bins || fail=$$((fail + 1)); \
	printf "\n$(BLUE)━━━ [8/16] x86_64 unit tests (via rosetta) ━━━$(RESET)\n"; \
	$(MAKE) --no-print-directory test-x64-all || fail=$$((fail + 1)); \
	printf "\n$(BLUE)━━━ [9/16] x86_64 coreutils (static musl, via rosetta) ━━━$(RESET)\n"; \
	$(MAKE) --no-print-directory test-x64-coreutils || fail=$$((fail + 1)); \
	printf "\n$(BLUE)━━━ [10/16] x86_64 busybox (via rosetta) ━━━$(RESET)\n"; \
	$(MAKE) --no-print-directory test-x64-busybox || fail=$$((fail + 1)); \
	printf "\n$(BLUE)━━━ [11/16] x86_64 static bins (bash, jq, sqlite, lua, via rosetta) ━━━$(RESET)\n"; \
	$(MAKE) --no-print-directory test-x64-static-bins || fail=$$((fail + 1)); \
	printf "\n$(BLUE)━━━ [12/16] x86_64 dynamic coreutils (musl, via rosetta) ━━━$(RESET)\n"; \
	$(MAKE) --no-print-directory test-x64-musl-coreutils || fail=$$((fail + 1)); \
	printf "\n$(BLUE)━━━ [13/16] x86_64 dynamic coreutils (glibc, via rosetta) ━━━$(RESET)\n"; \
	$(MAKE) --no-print-directory test-x64-glibc-coreutils || fail=$$((fail + 1)); \
	printf "\n$(BLUE)━━━ [14/16] x86_64 haskell hello (via rosetta) ━━━$(RESET)\n"; \
	$(MAKE) --no-print-directory test-x64-haskell || fail=$$((fail + 1)); \
	printf "\n$(BLUE)━━━ [15/16] aarch64 haskell hello ━━━$(RESET)\n"; \
	$(MAKE) --no-print-directory test-haskell || fail=$$((fail + 1)); \
	printf "\n$(BLUE)━━━ [16/16] x86_64 haskell bins (pandoc, shellcheck, via rosetta) ━━━$(RESET)\n"; \
	$(MAKE) --no-print-directory test-x64-haskell-bins || fail=$$((fail + 1)); \
	printf "\n$(CYAN)╔══════════════════════════════════════════════════════╗$(RESET)\n"; \
	if [ "$$fail" -eq 0 ]; then \
		printf "$(CYAN)║  $(GREEN)✓ All 16 suites passed$(CYAN)                              ║$(RESET)\n"; \
	else \
		printf "$(CYAN)║  $(RED)✗ $$fail suite(s) had failures$(CYAN)                        ║$(RESET)\n"; \
	fi; \
	printf "$(CYAN)╚══════════════════════════════════════════════════════╝$(RESET)\n"; \
	[ "$$fail" -eq 0 ]

# ── Multi-vCPU validation test ─────────────────────────────────────

## Build the multi-vCPU HVF validation test (native macOS binary)
$(BUILD_DIR)/test-multi-vcpu: test/test-multi-vcpu.c $(BUILD_DIR)/shim_blob.h | $(BUILD_DIR)
	@printf "$(GREEN)▸ Compiling$(RESET) test-multi-vcpu (native)\n"
	clang $(CFLAGS) \
		-I$(BUILD_DIR) \
		-o $@ $< \
		-framework Hypervisor -arch arm64
	@printf "$(GREEN)▸ Signing$(RESET) test-multi-vcpu\n"
	codesign --entitlements $(ENTITLEMENTS) -f -s "$(SIGN_IDENTITY)" $@

## Run multi-vCPU validation tests (5 tests)
test-multi-vcpu: $(BUILD_DIR)/test-multi-vcpu
	$(BUILD_DIR)/test-multi-vcpu

# ── RWX page table entry test ───────────────────────────────────

## Build the RWX W^X validation test (native macOS binary)
$(BUILD_DIR)/test-rwx: test/test-rwx.c $(BUILD_DIR)/shim_blob.h | $(BUILD_DIR)
	@printf "$(GREEN)▸ Compiling$(RESET) test-rwx (native)\n"
	clang $(CFLAGS) \
		-I$(BUILD_DIR) \
		-o $@ $< \
		-framework Hypervisor -arch arm64
	@printf "$(GREEN)▸ Signing$(RESET) test-rwx\n"
	codesign --entitlements $(ENTITLEMENTS) -f -s "$(SIGN_IDENTITY)" $@

## Run RWX page table entry test (does HVF allow W+X?)
test-rwx: $(BUILD_DIR)/test-rwx
	$(BUILD_DIR)/test-rwx

# ── Static analysis ────────────────────────────────────────────────

.PHONY: lint analyze format shellcheck

## Run clang-tidy on all source files
lint: $(BUILD_DIR)/shim_blob.h $(BUILD_DIR)/version.h
	@printf "$(BLUE)▸ Running$(RESET) clang-tidy\n"
	clang-tidy $(HL_SRCS) -- $(CFLAGS) -I$(SRC_DIR) -I$(BUILD_DIR)

## Run clang static analyzer (scan-build)
analyze:
	@printf "$(BLUE)▸ Running$(RESET) scan-build\n"
	scan-build --use-cc=clang $(MAKE) -B hl

## Run clang-format on all source files (check only, no changes)
format:
	@printf "$(BLUE)▸ Checking$(RESET) code formatting\n"
	clang-format --dry-run --Werror $(HL_SRCS) $(HL_HDRS)

# Shell scripts to lint (all .sh files in dist/, site/, test/)
SHELL_SCRIPTS := $(wildcard dist/*.sh site/*.sh test/*.sh)

## Run shellcheck on all shell scripts (warnings + errors)
shellcheck:
	@printf "$(BLUE)▸ Running$(RESET) shellcheck on %d scripts\n" $(words $(SHELL_SCRIPTS))
	@fail=0; \
	for f in $(SHELL_SCRIPTS); do \
		if shellcheck --severity=warning "$$f" 2>&1; then \
			printf "  $(GREEN)✓$(RESET) %s\n" "$$f"; \
		else \
			printf "  $(RED)✗$(RESET) %s\n" "$$f"; \
			fail=$$((fail + 1)); \
		fi; \
	done; \
	if [ "$$fail" -eq 0 ]; then \
		printf "$(GREEN)✓ All %d scripts pass shellcheck$(RESET)\n" $(words $(SHELL_SCRIPTS)); \
	else \
		printf "$(RED)✗ %d script(s) have shellcheck warnings$(RESET)\n" "$$fail"; \
		exit 1; \
	fi

# ── Cleanup ────────────────────────────────────────────────────────

## Remove all build artifacts
clean:
	rm -rf $(BUILD_DIR)

# ── Distribution ─────────────────────────────────────────────────

DIST_OUT  := dist/out
DIST_NAME := hl-$(VERSION)

## Build distribution zip archive
dist: $(BUILD_DIR)/hl
	@rm -rf dist/staging/$(DIST_NAME)
	@mkdir -p dist/staging/$(DIST_NAME) $(DIST_OUT)
	@printf "$(GREEN)▸ Assembling$(RESET) $(DIST_NAME)/\n"
	@cp $(BUILD_DIR)/hl           dist/staging/$(DIST_NAME)/hl
	@cp hl.1                      dist/staging/$(DIST_NAME)/hl.1
	@cp dist/configure            dist/staging/$(DIST_NAME)/configure
	@cp dist/Makefile.install     dist/staging/$(DIST_NAME)/Makefile
	@cp dist/README               dist/staging/$(DIST_NAME)/README
	@cp LICENSE                   dist/staging/$(DIST_NAME)/LICENSE
	@cp entitlements.plist        dist/staging/$(DIST_NAME)/entitlements.plist
	@chmod 755 dist/staging/$(DIST_NAME)/hl
	@chmod 755 dist/staging/$(DIST_NAME)/configure
	@printf "$(GREEN)▸ Creating$(RESET) $(DIST_OUT)/$(DIST_NAME).zip\n"
	@cd dist/staging && ditto -c -k --keepParent $(DIST_NAME) ../out/$(DIST_NAME).zip
	@rm -rf dist/staging/$(DIST_NAME)
	@printf "$(GREEN)✓ $(DIST_OUT)/$(DIST_NAME).zip$(RESET)\n"

## Build .pkg macOS installer
pkg: $(BUILD_DIR)/hl
	@sh dist/build-pkg.sh "$(VERSION)" "$(BUILD_DIR)/hl" hl.1

## Full signed + notarized release (requires SIGN_IDENTITY, INSTALLER_SIGN_IDENTITY)
release:
	@sh dist/build-release.sh "$(VERSION)"

## Interactive release: changelog, version bump, tag, push (uses claude)
release-interactive:
	@sh dist/release.sh

# ── Website ────────────────────────────────────────────────────────

## Open the hyper-linux.app landing page in a browser
site:
	@printf "$(GREEN)▸ Opening$(RESET) site/index.html\n"
	@open site/index.html

## Serve the site locally on port 8080
site-serve:
	@printf "$(GREEN)▸ Serving$(RESET) site/ at http://localhost:8080\n"
	@cd site && python3 -m http.server 8080

# ── Help ───────────────────────────────────────────────────────────

## Display this help message
help:
	@printf "$(BLUE)hl — aarch64-linux ELF executor on macOS Apple Silicon$(RESET)\n\n"
	@printf "$(GREEN)Usage:$(RESET) make <target> [SIGN_IDENTITY=\"...\"]\n\n"
	@printf "$(GREEN)Targets:$(RESET)\n"
	@awk '/^[a-zA-Z\-\_0-9%:\\]+:/ { \
		helpMessage = match(lastLine, /^## (.*)/); \
		if (helpMessage) { \
			helpCommand = $$1; sub(/:$$/, "", helpCommand); \
			helpMessage = substr(lastLine, RSTART + 3, RLENGTH); \
			printf "  $(YELLOW)%-20s$(RESET) %s\n", helpCommand, helpMessage; \
		} \
	} \
	{ lastLine = $$0 }' $(MAKEFILE_LIST)
ifdef GUEST_TEST_BINARIES
	@printf "\n$(GREEN)Test binaries:$(RESET) pre-built from nix ($(TEST_DIR))\n"
else
	@printf "\n$(GREEN)Cross-compiler:$(RESET) CROSS=$(CROSS)\n"
	@printf "  Override with: make test-hello CROSS=aarch64-linux-gnu-\n"
endif
