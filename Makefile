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

.PHONY: all hl clean test-hello test-all test-coreutils test-busybox \
       test-dynamic test-dynamic-coreutils test-perf test-multi-vcpu \
       test-haskell help

# ── Configuration ──────────────────────────────────────────────────
ENTITLEMENTS := entitlements.plist
SIGN_IDENTITY ?= -
BUILD_DIR := _build

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
YELLOW := \033[1;33m
RED    := \033[0;31m
RESET  := \033[0m

# ── Source files ───────────────────────────────────────────────────
HL_SRCS := hl.c guest.c elf.c syscall.c syscall_fs.c syscall_io.c \
           syscall_poll.c syscall_fd.c \
           syscall_time.c syscall_sys.c syscall_proc.c \
           proc_emulation.c syscall_exec.c fork_ipc.c \
           syscall_signal.c syscall_net.c stack.c \
           thread.c futex.c
HL_HDRS := guest.h elf.h syscall.h syscall_internal.h syscall_fs.h \
           syscall_io.h syscall_poll.h syscall_fd.h \
           syscall_time.h syscall_sys.h syscall_proc.h \
           proc_emulation.h syscall_exec.h fork_ipc.h \
           syscall_signal.h syscall_net.h stack.h \
           thread.h futex.h

# ── Default target ─────────────────────────────────────────────────
.DEFAULT_GOAL := help

## Build everything (hl + all test binaries)
all: hl $(TEST_DEPS)

# ── Build directory ────────────────────────────────────────────────
$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# ── Shim binary blob ──────────────────────────────────────────────

$(BUILD_DIR)/shim.o: shim.S | $(BUILD_DIR)
	@printf "$(GREEN)▸ Assembling$(RESET) shim.S\n"
	as -arch arm64 -o $@ $<

$(BUILD_DIR)/shim.bin: $(BUILD_DIR)/shim.o
	@printf "$(GREEN)▸ Creating binary$(RESET) shim.bin\n"
	nix shell nixpkgs#binutils -c objcopy -O binary $< $@

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

$(BUILD_DIR)/hl: $(HL_SRCS) $(HL_HDRS) $(BUILD_DIR)/shim_blob.h | $(BUILD_DIR)
	@printf "$(GREEN)▸ Compiling$(RESET) hl\n"
	clang -O2 -Wall -Wextra -Wpedantic \
		-I$(BUILD_DIR) \
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
		if output=$$($$@ 2>&1); then \
			printf "$(GREEN)✓ PASS$(RESET)\n"; \
			pass=$$((pass + 1)); \
		else \
			rc=$$?; \
			if [ "$$expected_rc" != "" ] && [ "$$rc" = "$$expected_rc" ]; then \
				printf "$(GREEN)✓ PASS$(RESET) (exit $$rc)\n"; \
				pass=$$((pass + 1)); \
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
	printf "\n$(BLUE)── O_CLOEXEC tests ──$(RESET)\n"; \
	run_test $(BUILD_DIR)/hl $(TEST_DIR)/test-cloexec; \
	printf "\n$(BLUE)━━━ Results: $$pass passed, $$fail failed ━━━$(RESET)\n"

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

## Run static Haskell hello world (GHC-produced aarch64-linux-musl ELF)
test-haskell: $(BUILD_DIR)/hl
	@if [ ! -x "$(HASKELL_HELLO)" ]; then \
		printf "$(RED)✗ Haskell hello not found.$(RESET) Run inside nix develop.\n"; \
		exit 1; \
	fi
	@printf "$(BLUE)▸ Running$(RESET) Haskell hello-hyper\n"
	$(BUILD_DIR)/hl $(HASKELL_HELLO)

# ── Multi-vCPU validation test ─────────────────────────────────────

## Build the multi-vCPU HVF validation test (native macOS binary)
$(BUILD_DIR)/test-multi-vcpu: test/test-multi-vcpu.c $(BUILD_DIR)/shim_blob.h | $(BUILD_DIR)
	@printf "$(GREEN)▸ Compiling$(RESET) test-multi-vcpu (native)\n"
	clang -O2 -Wall -Wextra -Wpedantic \
		-I$(BUILD_DIR) \
		-o $@ $< \
		-framework Hypervisor -arch arm64
	@printf "$(GREEN)▸ Signing$(RESET) test-multi-vcpu\n"
	codesign --entitlements $(ENTITLEMENTS) -f -s "$(SIGN_IDENTITY)" $@

## Run multi-vCPU validation tests (5 tests)
test-multi-vcpu: $(BUILD_DIR)/test-multi-vcpu
	$(BUILD_DIR)/test-multi-vcpu

# ── Cleanup ────────────────────────────────────────────────────────

## Remove all build artifacts
clean:
	rm -rf $(BUILD_DIR)

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
