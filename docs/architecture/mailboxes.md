# Mailbox protocol — NanoCamelid 32XCD

Design: a tiny distributed inference runtime inside a game console. Four CPUs,
three hardware comm channels, one shared-memory extension area. All messages
are fixed-format, no dynamic allocation, every loop bounded with a timeout
counter that latches an error code into the status block.

**Status: DRAFT — validated by Milestone 2's deterministic mailbox test.**

Message word format (16-bit comm registers): `[8-bit opcode | 8-bit arg]`,
with larger payloads in the SDRAM mailbox extension area (`memory-map.md`)
or Word RAM. Sequence numbers prevent lost/duplicate command processing:
every command carries a 4-bit seq in the arg's high nibble where noted.

## Channel 1 — Main 68K → Master SH-2 (32X COMM0/COMM1)

| Opcode | Name | Payload |
|---|---|---|
| `0x01` | `NOP_PING` | seq; SH2M echoes to COMM2 |
| `0x10` | `INPUT_READY` | prompt token count in COMM1; tokens in SDRAM mailbox ext |
| `0x11` | `START_GEN` | max tokens in COMM1 |
| `0x12` | `STOP_GEN` | — |
| `0x1F` | `RESET` | re-init inference state |

## Channel 2 — Master SH-2 → Main 68K (COMM2/COMM3)

| Opcode | Name | Payload |
|---|---|---|
| `0x81` | `PONG` | echoed seq |
| `0x90` | `TOKEN_OUT` | token ID (9 bits: COMM2 low byte + COMM3 bit) , also appended to SDRAM token buffer |
| `0x91` | `STATUS` | state machine value |
| `0x92` | `DEBUG_CTR` | counter ID + value via mailbox ext |
| `0x9E` | `ERROR` | error code |
| `0x9F` | `DONE` | total tokens generated |

## Channel 3 — Master SH-2 ↔ Slave SH-2 (COMM4/COMM5 + SDRAM)

| Direction | Opcode | Name | Payload |
|---|---|---|---|
| M→S | `0x20` | `SCORE_REQ` | work descriptor index in COMM4; descriptor in SDRAM: context vector ptr, candidate slice [start,len), model block ptr |
| M→S | `0x21` | `BARRIER` | sync point id |
| M→S | `0x2F` | `S_RESET` | — |
| S→M | `0xA0` | `SCORE_DONE` | best local candidate index + score in SDRAM result slot; COMM5 = done flag + seq |
| S→M | `0xA1` | `BARRIER_ACK` | sync point id |
| S→M | `0xA7` | `S_HEARTBEAT` | free-running counter in COMM7 |

Slave loop: spin on COMM4 (cache-bypassed read), execute, write result, bump
COMM7. The slave never touches the model double-buffer swap — only the master
flips buffers, at a barrier.

## Channel 4 — Sub 68K → Main/32X side (gate array status regs)

| Opcode | Name | Payload |
|---|---|---|
| `0x40` | `SHARD_READY` | shard ID + Word RAM offset + size + checksum in status regs |
| `0x41` | `CACHE_HIT` | shard ID served from PRG-RAM cache |
| `0x42` | `CD_READ_STATUS` | sectors remaining / busy |
| `0x43` | `DECOMP_DONE` | shard ID |
| `0x4E` | `CD_ERROR` | error code (read fail, checksum mismatch) |
| `0x4F` | `SUB_HEARTBEAT` | free-running counter (one status reg dedicated) |

## Channel 5 — Main/32X side → Sub 68K (gate array command regs)

| Opcode | Name | Payload |
|---|---|---|
| `0x50` | `LOAD_SHARD` | shard ID |
| `0x51` | `PRELOAD_NEXT` | shard ID (fill the other Word RAM half / staging) |
| `0x52` | `RESET_STREAM` | — |
| `0x53` | `CACHE_FLUSH` | — |

Note: the SH-2s never talk to the Sub 68K directly — the Main 68K is the
relay (gate array is on the 68K bus). The model data path is:
`CD → PRG RAM staging (Sub 68K decompress) → Word RAM → Main 68K relay via
32X DREQ FIFO → SDRAM model buffer`. If experiments (hardware-notes.md Q1)
show the SH-2s can read Word RAM directly, the relay step drops out and this
file gets updated.

## Pipeline (steady state, Milestone 9)

```
Sub 68K:   [read+decompress shard N+1]      [read+decompress shard N+2]
Main 68K:  [relay shard N to SDRAM B][render tok t-1][relay N+1]
SH-2 M:    [score half cands, reduce, select tok t from buffer A][swap A/B]
SH-2 S:    [score half cands              ][idle@barrier]
```

Invariants:
- SH-2s never read a buffer the relay is writing (master owns the swap, swap
  only after `SHARD_READY` + relay-done flag).
- Sub 68K never blocks on the SH-2s; it fills staging as long as there is a
  free buffer.
- Main 68K render work is interruptible between tokens; it never holds the
  relay longer than one shard.

## Heartbeats (Milestone 1 contract)

Every CPU increments a free-running counter every main-loop iteration:

| CPU | Location |
|---|---|
| MAIN68K | Work RAM `0xFF6000` (u32) |
| SUB68K | gate array status reg, snapshotted by Main 68K to `0xFF6004` |
| SH2M | COMM6 (u16, wraps) + SDRAM shared state (u32) |
| SH2S | COMM7 (u16, wraps) + SDRAM shared state (u32) |

The headless verifier samples these across N frames; "alive" = strictly
increasing across samples.
