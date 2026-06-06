# Memory map — NanoCamelid 32XCD

**Status: DRAFT.** Each region is marked with the milestone that validates it.
Nothing here is final until its milestone's verifier passes. Sizes are chosen
deliberately; change requires updating this file in the same commit.

## 32X SDRAM (256 KB) — SH-2 view `0x0600_0000`, uncached `0x2600_0000`

| Range (offset) | Size | Use | Validated |
|---|---|---|---|
| `0x00000–0x003FF` | 1 KB | SH-2 master vector table + boot stub | M1 |
| `0x00400–0x0FFFF` | 63 KB | SH-2 master code + rodata | M1 |
| `0x10000–0x17FFF` | 32 KB | SH-2 slave code + rodata | M1 |
| `0x18000–0x18FFF` | 4 KB | Shared inference state: context tokens, generated-token buffer (verifier reads this), status block, heartbeats | M1/M2 |
| `0x19000–0x19FFF` | 4 KB | Mailbox extension area (payloads too big for comm regs: prompt tokens, candidate batches, score results) | M2 |
| `0x1A000–0x39FFF` | 128 KB | **Model cache — double buffer**: Buffer A (64 KB, active) + Buffer B (64 KB, being filled from CD relay) | M3/M9 |
| `0x3A000–0x3DFFF` | 16 KB | Resident model hot set: tokenizer tables, context-window embeddings, biases (loaded once at init) | M7 |
| `0x3E000–0x3EFFF` | 4 KB | SH-2 master stack | M1 |
| `0x3F000–0x3FFFF` | 4 KB | SH-2 slave stack | M1 |

## 32X framebuffers (2 × 128 KB) — SH-2 `0x0400_0000`

Not used for inference data in v1 (display stays on the Genesis VDP for text
UI). Reserved as overflow model scratch **only if** Milestone 11 shows the
SDRAM cache is the bottleneck and FB access timing allows it. Validated: M11.

## Sega CD PRG RAM (512 KB) — Sub 68K

| Range | Size | Use | Validated |
|---|---|---|---|
| `0x000000–0x005FFF` | 24 KB | BIOS/system use + SP header (per Sega CD conventions) | M1 |
| `0x006000–0x01FFFF` | 104 KB | Sub 68K program: CD read loop, shard decompressor, cache manager | M1/M3 |
| `0x020000–0x05FFFF` | 256 KB | Shard staging: raw sector buffer + decompression output | M3 |
| `0x060000–0x07FFFF` | 128 KB | Shard cache (recently used shards, avoids CD re-reads) | M9 |

## Sega CD Word RAM (256 KB) — handoff area

2M mode, whole 256 KB swapped between Sub and Main 68K:

| Range | Size | Use | Validated |
|---|---|---|---|
| `0x00000–0x0FFFF` | 64 KB | Decompressed shard ready for relay to 32X SDRAM (matches one SDRAM model buffer) | M3 |
| `0x10000–0x1FFFF` | 64 KB | Next shard (lets Sub 68K stay ahead while Main 68K relays) | M9 |
| `0x20000–0x3DFFF` | 120 KB | Reserved (eval prompt data, demo assets) | M10+ |
| `0x3E000–0x3FFFF` | 8 KB | Sub→Main metadata block: shard table of contents, checksums, status | M3 |

*(1M split mode is the fallback if 2M swap latency stalls the pipeline — M9.)*

## Genesis Work RAM (64 KB) — Main 68K `0xFF0000`

| Range (offset) | Size | Use | Validated |
|---|---|---|---|
| `0x0000–0x00FF` | 256 B | Vectors/system | M1 |
| `0x0100–0x3FFF` | ~16 KB | Main 68K program (copied from CD by IP) + stack | M1 |
| `0x4000–0x5FFF` | 8 KB | Text UI state: input line, decoded token text ring, render queue | M8 |
| `0x6000–0x67FF` | 2 KB | Mailbox shadow + debug counters (heartbeat snapshot of all CPUs) | M1/M2 |
| `0x6800–0x7FFF` | 6 KB | Relay buffer bookkeeping for Word RAM → 32X DREQ transfers | M3 |
| `0x8000–0xFFFF` | 32 KB | Reserved / demo mode assets | M12 |

## Communication registers (fixed by hardware)

| Channel | Hardware | Allocation |
|---|---|---|
| Main 68K ↔ SH-2s | 32X COMM0–7 (8×16-bit, `0xA15120` / `0x2000_4020`) | COMM0: M68K→SH2M cmd; COMM1: cmd arg; COMM2: SH2M→M68K status/token; COMM3: SH2M debug ctr; COMM4: SH2M→SH2S work desc; COMM5: SH2S→SH2M result; COMM6: SH2M heartbeat; COMM7: SH2S heartbeat |
| Main 68K ↔ Sub 68K | Gate array comm flags + 2×16-byte cmd/status regs (`0xA12000+`) | see mailboxes.md |
| Bulk Main→SH-2 | 32X DREQ FIFO (`0xA15112`) | model shard relay into SDRAM Buffer A/B |

## Verifier-visible addresses (the receipt contract)

The headless harness reads these to produce receipts; they must stay stable
once validated:

- **Heartbeats**: Work RAM `0xFF6000` (MAIN68K), `0xFF6004` (SUB68K snapshot,
  relayed), COMM6/COMM7 (SH2M/SH2S live) + SDRAM shared-state copies.
- **Generated token buffer**: SDRAM offset `0x18000+`, format:
  `u16 count; u16 token_ids[64]`, mirrored to Work RAM `0xFF6100` for easy
  inspection. (Mirror validated M7.)
- **Status block**: SDRAM `0x18xxx`: current milestone state machine value,
  error code, shard counters, processor role counters.
