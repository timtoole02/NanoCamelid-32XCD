/*
 * Slave SH-2 runtime — Milestones 6/7/8: the parallel scoring worker.
 *
 * OP_WORK  — M2 mailbox proof (kept green)
 * OP_SCORE — score candidate slots [8, count) from the SDRAM score block,
 *            reply with the best (absolute slot, score). First-wins within
 *            the slice (ascending slot scan, strict >). If count <= 8 the
 *            reply is (0, SCORE_MIN) so the master's reduce ignores it.
 */
#include "../shared/mailbox.h"
#include "../shared/infer.h"

#define ROM_BASE 0x22000000u

__attribute__((noreturn)) void cmain(void)
{
    uint16_t last_work = 0;
    uint16_t last_score = 0;
    uint32_t hb = 0;
    Model model;

    mdl_init(&model, ROM_BASE);

    for (;;) {
        SHARED(SH_HB_SLAVE) = ++hb;
        COMM(7) = (uint16_t)hb;

        uint16_t work = COMM(4);
        uint16_t op = CMD_OP(work);
        uint16_t seq = CMD_SEQ(work);

        if (op == OP_WORK && seq != last_work) {
            uint16_t seed = COMM(1);
            SHARED(SH_SLAVE_PART) = m2_partial(seed, M2_SPLIT, M2_K);
            SHARED(SH_JOBS_SLAVE) += 1;
            last_work = seq;
            COMM(5) = seq;
        } else if (op == OP_SCORE && seq != last_score) {
            int n = (int)SCB(SCB_COUNT);
            int best = 0;
            int32_t best_s = MDL_SCORE_MIN;

            if (n > 8) {
                int32_t cv[MDL_DIM];
                uint16_t recent[MDL_REP_WINDOW];
                int rl = (int)SCB(SCB_RECENT_LEN);
                int emitted = (int)SCB(SCB_EMITTED);
                int i;
                for (i = 0; i < MDL_DIM; i++)
                    cv[i] = (int32_t)SCB(SCB_CV + i * 4);
                for (i = 0; i < rl; i++)
                    recent[i] = SCB16(SCB_RECENT + i * 2);
                for (i = 8; i < n; i++) {
                    uint16_t tok = SCB16(SCB_CANDS + i * 4);
                    int8_t ng = (int8_t)*(volatile uint8_t *)(SCORE_BLOCK + SCB_CANDS + i * 4 + 2);
                    int32_t s = infer_score(&model, cv, tok, ng, recent, rl, emitted);
                    SHARED(SH_SCORE_JOBS_S) += 1;
                    if (s > best_s) {
                        best_s = s;
                        best = i;
                    }
                }
            }
            SCB(SCB_SLAVE_SLOT) = (uint32_t)best;
            SCB(SCB_SLAVE_SCORE) = (uint32_t)best_s;
            last_score = seq;
            COMM(5) = seq;
        }
    }
}
