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

.PHONY: all hl sign clean test-hello help

# ── Configuration ──────────────────────────────────────────────────
ENTITLEMENTS := entitlements.plist
SIGN_IDENTITY ?= -
BUILD_DIR := _build

# Cross-compiler prefix for aarch64-linux test binaries
CROSS ?= aarch64-unknown-linux-musl-

# Colors
GREEN  := \033[0;32m
BLUE   := \033[0;34m
YELLOW := \033[1;33m
RED    := \033[0;31m
RESET  := \033[0m

# ── Source files ───────────────────────────────────────────────────
HL_SRCS := hl.c guest.c elf.c syscall.c
HL_HDRS := guest.h elf.h syscall.h

# ── Default target ─────────────────────────────────────────────────
.DEFAULT_GOAL := help

## Build everything (hl + test binaries)
all: hl test-hello

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

# ── Test binaries ──────────────────────────────────────────────────

## Build and run the assembly hello world test
test-hello: $(BUILD_DIR)/hl $(BUILD_DIR)/test-hello
	@printf "$(BLUE)▸ Running$(RESET) test-hello\n"
	$(BUILD_DIR)/hl $(BUILD_DIR)/test-hello

$(BUILD_DIR)/test-hello: test/hello.S test/simple.ld | $(BUILD_DIR)
	@printf "$(GREEN)▸ Cross-assembling$(RESET) test/hello.S\n"
	$(CROSS)as -o $(BUILD_DIR)/test-hello.o test/hello.S
	@printf "$(GREEN)▸ Cross-linking$(RESET) test-hello\n"
	$(CROSS)ld -T test/simple.ld -o $@ $(BUILD_DIR)/test-hello.o

# ── Legacy vm target (preserved) ──────────────────────────────────

## Build the legacy vm executable (original PoC)
vm: $(BUILD_DIR)/vm
	@printf "$(GREEN)✓ vm built$(RESET)\n"

$(BUILD_DIR)/vm: vm.c | $(BUILD_DIR)
	clang -o $@ vm.c -framework Hypervisor -arch arm64
	codesign --entitlements $(ENTITLEMENTS) -f -s "$(SIGN_IDENTITY)" $@

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
	@printf "\n$(GREEN)Cross-compiler:$(RESET) CROSS=$(CROSS)\n"
	@printf "  Override with: make test-hello CROSS=aarch64-linux-gnu-\n"
