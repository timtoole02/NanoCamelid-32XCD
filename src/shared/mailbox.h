/*
 * NanoCamelid 32XCD — mailbox protocol + shared memory layout.
 * Single source of truth for all CPUs; mirrors docs/architecture/
 * (mailboxes.md, memory-map.md). The Python verifiers replicate the
 * constants and the M2 work function exactly.
 *
 * COMM register allocation (8 x u16):
 *   COMM0  68K -> SH2M : command word (opcode<<8 | seq). SH2M ACKs by
 *                        clearing to 0; 68K waits for 0 before reusing.
 *   COMM1  68K -> SH2s : command argument (e.g. M2 seed)
 *   COMM2  SH2M -> 68K : result high 16
 *   COMM3  SH2M -> 68K : result low 16
 *   COMM4  SH2M -> SH2S: work word (opcode<<8 | seq)
 *   COMM5  SH2S -> SH2M: completion seq echo
 *   COMM6  SH2M heartbeat (u16, wraps)
 *   COMM7  SH2S heartbeat (u16, wraps)
 */
#ifndef NC32X_MAILBOX_H
#define NC32X_MAILBOX_H

#include <stdint.h>

typedef volatile uint16_t vu16;
typedef volatile uint32_t vu32;

/* --- COMM registers --- */
#ifdef NC32X_CPU_M68K
#define COMM_BASE 0xA15120
#else /* SH-2 */
#define COMM_BASE 0x20004020
#endif
#define COMM(n) (((vu16 *)COMM_BASE)[n])

/* --- opcodes (COMM0/COMM4 high byte) --- */
#define OP_NOP   0x00
#define OP_WORK  0x10  /* M2: split table-sum over candidate indices */
#define OP_TOKEN 0x11  /* 68K->SH2M: append prompt token (id in COMM1) */
#define OP_GEN   0x12  /* 68K->SH2M: generate (qtype in COMM1) */
#define OP_SCORE 0x20  /* SH2M->SH2S: score the high candidate slots */

/* --- generation streaming (master -> 68K during OP_GEN) ---
 * COMM2 = 0xA000|token  (one generated token)
 *         0xB000|count  (generation done; count = tokens emitted)
 * COMM3 = emit sequence number (1-based); 0xFFFF on done.
 * 68K ACKs each token by writing the emit number back to COMM1; the
 * master waits for the ACK before overwriting COMM2 (lossless stream). */
#define MSG_TOKEN 0xA000
#define MSG_DONE  0xB000

#define CMD(op, seq)  ((uint16_t)(((op) << 8) | ((seq) & 0xFF)))
#define CMD_OP(w)     ((uint16_t)(w) >> 8)
#define CMD_SEQ(w)    ((w) & 0xFF)

/* --- SDRAM shared state (offsets from SDRAM base; uncached for
 *     cross-CPU + verifier visibility). memory-map.md: 0x18000 block. --- */
#define SDRAM_UNCACHED 0x26000000
#define SHARED(off) (*(vu32 *)(SDRAM_UNCACHED + 0x18000 + (off)))

#define SH_HB_MASTER   0x00  /* u32 master heartbeat */
#define SH_HB_SLAVE    0x04  /* u32 slave heartbeat */
#define SH_JOBS_MASTER 0x10  /* u32 master jobs completed (role counter) */
#define SH_JOBS_SLAVE  0x14  /* u32 slave jobs completed (role counter) */
#define SH_SLAVE_PART  0x20  /* u32 slave partial sum for current seq */
#define SH_RESULT      0x28  /* u32 last combined result */
#define SH_GEN_COUNT   0x30  /* u32 generated-token count (verifier) */
#define SH_GEN_FALLBACK 0x34 /* u32 fallback level of the last generation */
#define SH_SCORE_JOBS_M 0x38 /* u32 candidate-slots scored by master */
#define SH_SCORE_JOBS_S 0x3C /* u32 candidate-slots scored by slave */

/* --- SDRAM mailbox extension block (0x19000, uncached): the per-step
 * scoring workspace shared master -> slave. Master writes everything,
 * bumps COMM4 seq; slave reads, scores slots [8, count), replies. --- */
#define SCORE_BLOCK    (SDRAM_UNCACHED + 0x19000)
#define SCB_CV         0x00  /* i32[16] context vector            */
#define SCB_CANDS      0x40  /* 16 x (u16 tok, i8 ng, u8 pad)     */
#define SCB_COUNT      0x80  /* u32 candidate count               */
#define SCB_RECENT     0x84  /* u16[8] recent tokens (newest last) */
#define SCB_RECENT_LEN 0x94  /* u32 */
#define SCB_EMITTED    0x98  /* u32 emitted count (EOS gating)    */
#define SCB_SLAVE_SLOT 0xA0  /* u32 slave best slot (abs index)   */
#define SCB_SLAVE_SCORE 0xA4 /* i32 slave best score              */
#define SCB(off) (*(vu32 *)(SCORE_BLOCK + (off)))
#define SCB16(off) (*(vu16 *)(SCORE_BLOCK + (off)))

/* generated tokens for the verifier (SDRAM mirror; the 68K work-RAM
 * mirror at 0xFF6100 is the canonical receipt location) */
#define SH_GEN_BUF     (SDRAM_UNCACHED + 0x18100) /* u16 count + u16 ids[] */

/* --- M2 deterministic work function ---------------------------------
 * "table" entry (no rodata needed):  tab(i) = (i*0x9E37 + 0x4242) & 0xFFFF
 * partial(seed, lo, hi) = sum_{i=lo}^{hi-1} tab(i) * ((seed+i) & 0xFFFF)
 * master computes i in [0,32), slave i in [32,64); u32 wraparound.
 * Python reference lives in scripts/verify_m2.py — keep in sync. */
#define M2_K 64
#define M2_SPLIT 32

static inline uint16_t m2_tab(uint32_t i)
{
    return (uint16_t)(i * 0x9E37u + 0x4242u);
}

static inline uint32_t m2_partial(uint16_t seed, uint32_t lo, uint32_t hi)
{
    uint32_t sum = 0;
    for (uint32_t i = lo; i < hi; i++)
        sum += (uint32_t)m2_tab(i) * (uint16_t)(seed + i);
    return sum;
}

#endif /* NC32X_MAILBOX_H */
