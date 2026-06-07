# NanoCamelid 32XCD

**A tiny trained language model running locally across Sega 32X + Sega CD hardware.**

NanoCamelid 32XCD is a tiny trained language-model inference engine running
locally across Sega 32X + Sega CD hardware (emulator-first). It is **not** a
modern LLM, but it performs real next-token inference using trained compressed
model data, multi-processor scheduling, and verifier receipts.

The model is small, the constraints are absurd, and the generated token stream
is verified token-for-token against a Rust reference implementation.

## What actually runs where

| Processor | Clock | Role |
|---|---|---|
| 32X Master SH-2 | 23 MHz | Inference coordinator: generation loop, context, token selection |
| 32X Slave SH-2 | 23 MHz | Parallel scoring worker: half the candidate set, int8 dot products |
| Sega CD Sub 68000 | 12.5 MHz | Model streaming: reads/decompresses model shards from CD, double-buffered |
| Genesis Main 68000 | 7.67 MHz | UI, input, text rendering, orchestration |
| Z80 | 3.58 MHz | (optional, later) audio feedback |

No server. No hidden host inference. No canned answer lookup. The scoring loop
— int8 dot products over trained quantized weights, candidate reranking,
greedy decode — executes on the emulated console processors. Training,
quantization, and packing happen **offline** on the host; the console only
runs inference.

See [docs/claims.md](docs/claims.md) for the full honest-claims statement
(what this is and is not).

## Status

| Milestone | Description | Status |
|---|---|---|
| 0 | Repository scaffold | ✅ done |
| 1 | 32X boot proof, per-processor heartbeats + title screen | ✅ done (cart scope; CD half blocked on user-supplied BIOS) |
| 2 | Processor mailbox proof | ✅ done (68K→SH2M→SH2S→reduce→render, host-verified results) |
| 3 | CD model streaming proof | 🚧 blocked: needs Sega CD BIOS in `assets/bios/` |
| 4 | Host-side trainer | ✅ done (deterministic SGD reranker, int8 quantization, packed shards) |
| 5 | Rust reference inference | ✅ done (integer-exact, loads packed bins, 121 eval prompts) |
| 6 | SH-2 scoring kernel | ✅ done (int8 dot products, split 8/8 across both SH-2s) |
| 7 | First real generated token (parity vs reference) | ✅ done (exact match) |
| 8 | Multi-token generation, full parity | ✅ done (4 prompts, all token IDs match the reference) |
| 9 | Parallel streaming inference (CD prefetch during scoring) | — |
| 10 | Eval gates + receipts | ✅ done (422 prompts, 100% token+fallback parity, never blank) |
| 11 | Performance push | ✅ measured: ~1070 tok/s emulated, 0.056 frames/token, slave at 50% of split slots |
| 12 | Demo mode | ✅ done (controller-typed prompts, on-screen keyboard, live answers + stats) |

## Building

Prerequisites (macOS):

- `m68k-elf-gcc` / `m68k-elf-binutils` (Homebrew)
- `sh-elf-gcc` cross-toolchain at `/Volumes/Untitled/toolchains/sh-elf`
  (built by `/Volumes/Untitled/toolchains/build-sh-elf.sh`)
- Rust (host tools: trainer, packer, verifier, reference runtime)
- PicoDrive libretro core (the only emulator that boots the 32X+CD combined
  path) + the headless harness in `tools/emulator`
- **Sega CD BIOS** (`bios_CD_U.bin`) — required for the CD boot path; not
  distributed with this repo. Place in `assets/bios/` (gitignored).

```sh
source scripts/env.sh
make all      # build host tools, console binaries, model, CD image
make verify   # run headless emulator, check token parity, emit receipts
```

## Layout

```
docs/            architecture, claims, hardware notes, eval sets, receipts
tools/trainer    offline model training (Rust)
tools/packer     model shard packing + manifest + hashes (Rust)
tools/verifier   receipt generation + console-vs-reference parity (Rust)
tools/emulator   headless libretro frontend for PicoDrive (verification)
src/host         Rust reference inference runtime (source of truth)
src/genesis68k   Main 68000: boot, UI, input, text rendering
src/segacd68k    Sub 68000: CD shard streaming + decompression
src/sh2_master   Master SH-2: generation loop, token selection
src/sh2_slave    Slave SH-2: parallel candidate scoring
src/shared       mailbox protocol, model format headers shared across CPUs
assets/corpus    training corpus (curated, local)
model/           packed model artifacts (generated)
cd/              CD image staging + ISO build (generated)
build/           build output (gitignored)
scripts/         env setup, build helpers
```
