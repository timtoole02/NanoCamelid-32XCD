# Hardware notes — Sega Genesis + 32X + Sega CD combined stack

Emulator-first target: **PicoDrive** (the only emulator family with credible
32X+CD combined support). Facts below are from public hardware documentation;
items marked **[verify]** must be confirmed by experiment before code relies
on them (Milestones 1–3).

## Processors

| CPU | Clock (NTSC) | Where | Notes |
|---|---|---|---|
| Master SH-2 | 23.01 MHz | 32X | 32-bit RISC, 5-stage pipeline, `MAC.W`/`MAC.L`, 4KB unified cache |
| Slave SH-2 | 23.01 MHz | 32X | identical, shares SDRAM bus with master |
| Main 68000 | 7.67 MHz | Genesis | boots the system; UI/VDP owner |
| Sub 68000 | 12.5 MHz | Sega CD | runs from PRG RAM; owns CDC/CDD (CD drive) |
| Z80 | 3.58 MHz | Genesis | sound; unused until late milestones |

Key compute facts:

- SH-2 has single-cycle issue, hardware `MULS.W` (16×16→32) and `MAC.W`
  with a 64-bit MAC register pair — this is the int8/int16 dot-product engine.
- The two SH-2s **share** the SDRAM bus: simultaneous SDRAM access stalls.
  Scoring kernels should run from cache and walk disjoint memory regions.
- SH-2 cache is 4KB, 4-way, 16-byte lines; can be configured partially as
  scratch RAM. Inner scoring loop + hot tables must be cache-resident.
- No FPU anywhere in the system. All runtime math is integer/fixed-point.

## Memory inventory (totals)

| Region | Size | Owner / access |
|---|---|---|
| 32X SDRAM | 256 KB | both SH-2s (SH-2 addr `0x0600_0000`, uncached mirror `0x2600_0000`) |
| 32X framebuffers | 2 × 128 KB | SH-2 (`0x0400_0000`), one displayed one drawable |
| Sega CD PRG RAM | 512 KB | Sub 68K (always); Main 68K via 128 KB banked window **[verify in combined mode]** |
| Sega CD Word RAM | 256 KB | switchable Main/Sub 68K (2M mode) or split 2×128 KB (1M mode) |
| Genesis Work RAM | 64 KB | Main 68K |
| Genesis VRAM | 64 KB | VDP (text UI tiles) |
| Z80 RAM | 8 KB | Z80 |

## Communication hardware

- **32X comm registers**: 8 × 16-bit, Main 68K side `0xA15120–0xA1512F`,
  SH-2 side `0x2000_4020–0x2000_402F`. Both SH-2s and the 68K see them.
  This is the mailbox backbone for Main 68K ↔ SH-2 and SH-2 ↔ SH-2.
- **Sega CD gate array**: Main side `0xA12000+`; comm flags + 16 bytes of
  main→sub command registers + 16 bytes of sub→main status registers.
  Mailbox backbone for Main 68K ↔ Sub 68K.
- **32X DREQ/FIFO DMA**: 68K-side writes to FIFO (`0xA15112`), SH-2 DMAC
  drains it — the fast path for pushing CD-loaded model data into SDRAM
  **[verify in combined mode]**.
- SH-2s additionally interrupt each other / get CMD interrupts via the 32X
  system registers (VRES/V/H/CMD/PWM int lines).

## The 32X+CD combined boot path (the hard part)

Used by the handful of real "32X CD" titles (Night Trap 32X, Corpse Killer
32X, …):

1. 32X sits in the cart slot with **no cartridge on top**; Sega CD attached.
2. Sega CD BIOS boots the disc: standard `SEGADISCSYSTEM` header, IP
   (initial program, runs on Main 68K) + SP (sub program, runs on Sub 68K).
3. Main 68K enables the 32X (ADEN), uploads SH-2 programs into SDRAM via the
   comm/DMA path, and points the SH-2s at their entry vectors. **[verify:
   exact handshake sequence in PicoDrive]**
4. From then on: four CPUs live. Sub 68K owns CD reads; Main 68K owns
   VDP/UI; SH-2s own compute.

Open questions to resolve by experiment (Milestone 1–3):

- **Q1**: Can the SH-2s read Sega CD Word RAM directly through the `0x0200_0000`
  ROM window in combined mode, or must the Main 68K relay data into SDRAM via
  DREQ DMA? (Determines the model-shard data path; assume relay until proven.)
- **Q2**: Exact 32X activation sequence when booted from CD BIOS (RV bit
  handling, security/boot ROM behavior with no cart).
- **Q3**: PicoDrive accuracy limits on the combined path (it is the reference
  emulator but the combined mode is exercised by ~5 commercial titles).

## Emulator + BIOS requirements

- **PicoDrive** (libretro core, built from `irixxxx/picodrive`), driven by a
  custom headless frontend in `tools/emulator` for deterministic runs and
  memory inspection (receipts).
- **Sega CD BIOS required**: `bios_CD_U.bin` (US model 1) or equivalent.
  Copyrighted — user-supplied, gitignored, hash recorded in receipts.
  *Not found on this machine as of 2026-06-06 — must be sourced before
  Milestone 1's CD half can run. The 32X heartbeat half of Milestone 1 can
  proceed cart-only without it.*

## Performance envelope (back-of-envelope, to be measured)

- 2 × 23 MHz SH-2, `MAC.W` ≈ 1 multiply-accumulate per ~2-3 cycles from cache.
- Score step v1: K=16 candidates × dim 16 dot product ≈ 256 MACs + overhead
  → trivially sub-frame *if* data is cache/SDRAM-resident.
- The real costs will be: CD seek/read latency (hidden by double-buffered
  prefetch), 68K↔SH-2 relay bandwidth, and bus contention. Measure, don't
  assume (Milestone 11).
