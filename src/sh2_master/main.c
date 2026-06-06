/*
 * Master SH-2 runtime — Milestone 2: inference-coordinator skeleton.
 * Service loop: heartbeat + command dispatch. OP_WORK splits the
 * deterministic table-sum across both SH-2s (this CPU takes the low
 * half), reduces, returns the result to the 68K, ACKs by clearing COMM0.
 */
#include "../shared/mailbox.h"

__attribute__((noreturn)) void cmain(void)
{
    uint16_t last_seq = 0;
    uint32_t hb = 0;

    for (;;) {
        SHARED(SH_HB_MASTER) = ++hb;
        COMM(6) = (uint16_t)hb;

        uint16_t cmd = COMM(0);
        if (CMD_OP(cmd) == OP_WORK && CMD_SEQ(cmd) != last_seq) {
            uint16_t seq = CMD_SEQ(cmd);
            uint16_t seed = COMM(1);

            COMM(4) = cmd;                       /* dispatch high half to slave */
            uint32_t sum = m2_partial(seed, 0, M2_SPLIT);

            while (COMM(5) != seq)               /* reduce: wait for slave */
                SHARED(SH_HB_MASTER) = ++hb;     /* stay visibly alive */
            sum += SHARED(SH_SLAVE_PART);

            SHARED(SH_RESULT) = sum;
            COMM(2) = (uint16_t)(sum >> 16);
            COMM(3) = (uint16_t)sum;
            SHARED(SH_JOBS_MASTER) += 1;

            last_seq = seq;
            COMM(0) = 0;                         /* ACK to 68K */
        }
    }
}
