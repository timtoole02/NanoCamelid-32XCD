# NanoCamelid 32XCD — top-level build
#
#   make all      build host tools, console binaries, model, CD image
#   make verify   run headless emulator, check parity, emit receipts
#
# Requires: source scripts/env.sh   (toolchain paths, CARGO_TARGET_DIR)

SHELL := /bin/bash
ROOT  := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

SH_PREFIX   ?= /Volumes/Untitled/toolchains/sh-elf/bin/sh-elf-
M68K_PREFIX ?= m68k-elf-
PICODRIVE_CORE ?= /Volumes/Untitled/emu/picodrive/picodrive_libretro.dylib
CD_BIOS     ?= $(ROOT)/assets/bios/bios_CD_U.bin

export CARGO_TARGET_DIR ?= /Volumes/Untitled/cargo-targets/nanocamelid-32XCD

.PHONY: all tools console model cdimage verify clean check-env

all: check-env tools console model cdimage

# --- host tools (Rust workspace) -------------------------------------------
tools:
	@echo "== host tools =="
	@if [ -f tools/Cargo.toml ]; then cargo build --release --manifest-path tools/Cargo.toml; \
	else echo "  (no tools workspace yet — Milestone 4+)"; fi

# --- console binaries -------------------------------------------------------
console:
	@echo "== console binaries =="
	@if [ -f src/Makefile ]; then $(MAKE) -C src SH_PREFIX=$(SH_PREFIX) M68K_PREFIX=$(M68K_PREFIX); \
	else echo "  (no console build yet — Milestone 1)"; fi

# --- model artifacts ---------------------------------------------------------
model:
	@echo "== model =="
	@if [ -x scripts/build-model.sh ]; then scripts/build-model.sh; \
	else echo "  (no model pipeline yet — Milestone 4)"; fi

# --- CD image ----------------------------------------------------------------
cdimage:
	@echo "== CD image =="
	@if [ -x scripts/build-cd.sh ]; then scripts/build-cd.sh; \
	else echo "  (no CD image build yet — Milestone 1/3)"; fi

# --- verification ------------------------------------------------------------
verify:
	@echo "== verify =="
	@if [ -x scripts/verify.sh ]; then scripts/verify.sh; \
	else echo "VERIFY FAIL: scripts/verify.sh not implemented yet (Milestone 1)"; exit 1; fi

check-env:
	@command -v $(M68K_PREFIX)gcc >/dev/null || { echo "missing $(M68K_PREFIX)gcc (brew install m68k-elf-gcc)"; exit 1; }
	@[ -x $(SH_PREFIX)gcc ] || echo "WARNING: $(SH_PREFIX)gcc not found (sh-elf toolchain still building?)"
	@[ -f $(PICODRIVE_CORE) ] || echo "WARNING: PicoDrive core not found at $(PICODRIVE_CORE)"
	@[ -f $(CD_BIOS) ] || echo "WARNING: Sega CD BIOS not found at $(CD_BIOS) — CD boot path unavailable"

clean:
	rm -rf build/* cd/out
	@if [ -f src/Makefile ]; then $(MAKE) -C src clean; fi
