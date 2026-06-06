/*
 * Slave SH-2 runtime — Milestone 2: parallel scoring-worker skeleton.
 * Polls COMM4 for work from the master, computes the high half of the
 * deterministic table-sum, posts the partial + completion seq.
 */
#include "../shared/mailbox.h"

__attribute__((noreturn)) void cmain(void)
{
    uint16_t last_seq = 0;
    uint32_t hb = 0;

    for (;;) {
        SHARED(SH_HB_SLAVE) = ++hb;
        COMM(7) = (uint16_t)hb;

        uint16_t work = COMM(4);
        if (CMD_OP(work) == OP_WORK && CMD_SEQ(work) != last_seq) {
            uint16_t seq = CMD_SEQ(work);
            uint16_t seed = COMM(1);

            SHARED(SH_SLAVE_PART) = m2_partial(seed, M2_SPLIT, M2_K);
            SHARED(SH_JOBS_SLAVE) += 1;

            last_seq = seq;
            COMM(5) = seq;                       /* completion to master */
        }
    }
}
