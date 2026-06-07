/*
 * Master SH-2 runtime — Milestones 6/7/8: the inference coordinator.
 *
 * Service loop ops:
 *   OP_WORK  — M2 mailbox proof (kept so the M2 receipt stays green)
 *   OP_TOKEN — append one prompt token id
 *   OP_GEN   — run the full greedy decode loop:
 *              per token: candidate merge (ROM tables) -> context vector ->
 *              master scores slots [0,8) while slave scores [8,count) ->
 *              reduce (first-wins tie-break) -> stream token to the 68K.
 *
 * Exact mirror of nc-model generate_packed(); parity is checked by
 * scripts/verify_m78.py via nc-verifier.
 */
#include "../shared/mailbox.h"
#include "../shared/infer.h"

#define ROM_BASE 0x02000000u  /* cached ROM mirror: read-only data, coherency-safe */

#define SEQ_MAX (24 + MDL_MAX_TOKENS)

static void emit_token(uint16_t tok, uint16_t n)
{
    COMM(2) = (uint16_t)(MSG_TOKEN | tok);
    COMM(3) = n;
    /* ACK is 0x4000|n so it can never collide with a stale qtype in COMM1 */
    while (COMM(1) != (uint16_t)(0x4000 | n)) {
        SHARED(SH_HB_MASTER) += 1;
    }
}

static void generate(const Model *m, uint16_t *seq, int seq_len, int qt)
{
    uint16_t out[MDL_MAX_TOKENS];
    int out_len = 0;
    uint16_t gen_seq = 0;
    uint8_t fallback = 0;
    int step;

    for (step = 0; step < MDL_MAX_TOKENS; step++) {
        uint16_t w2 = seq[seq_len - 1];
        uint16_t w1 = seq_len >= 2 ? seq[seq_len - 2] : TOK_PAD;
        uint16_t ctx[MDL_CTX];
        Cand cands[MDL_K];
        int32_t cv[MDL_DIM];
        uint8_t tier;
        int n, j, i;

        for (j = 0; j < MDL_CTX; j++)
            ctx[j] = (seq_len >= MDL_CTX - j) ? seq[seq_len - (MDL_CTX - j)] : TOK_PAD;

        n = infer_candidates(m, w1, w2, cands, &tier);
        infer_context_vector(m, ctx, qt, cv);

        /* publish the scoring workspace for the slave */
        for (i = 0; i < MDL_DIM; i++)
            SCB(SCB_CV + i * 4) = (uint32_t)cv[i];
        for (i = 0; i < n; i++) {
            SCB16(SCB_CANDS + i * 4) = cands[i].tok;
            *(volatile uint8_t *)(SCORE_BLOCK + SCB_CANDS + i * 4 + 2) = (uint8_t)cands[i].ng;
        }
        SCB(SCB_COUNT) = (uint32_t)n;
        {
            int rl = out_len < MDL_REP_WINDOW ? out_len : MDL_REP_WINDOW;
            for (i = 0; i < rl; i++)
                SCB16(SCB_RECENT + i * 2) = out[out_len - rl + i];
            SCB(SCB_RECENT_LEN) = (uint32_t)rl;
        }
        SCB(SCB_EMITTED) = (uint32_t)out_len;

        gen_seq = (uint16_t)((gen_seq & 0xFF) % 255 + 1); /* 1..255, never 0 */
        COMM(4) = CMD(OP_SCORE, gen_seq);

        /* master scores the low slots while the slave does the high ones */
        {
            int lo_n = n < 8 ? n : 8;
            int best = -1;
            int32_t best_s = MDL_SCORE_MIN;
            int32_t s;
            uint16_t chosen;
            for (i = 0; i < lo_n; i++) {
                s = infer_score(m, cv, cands[i].tok, cands[i].ng, out, out_len, out_len);
                SHARED(SH_SCORE_JOBS_M) += 1;
                if (s > best_s) {
                    best_s = s;
                    best = i;
                }
            }
            while (COMM(5) != gen_seq) {
                SHARED(SH_HB_MASTER) += 1;
            }
            if (n > 8) {
                int32_t ss = (int32_t)SCB(SCB_SLAVE_SCORE);
                int sslot = (int)SCB(SCB_SLAVE_SLOT);
                if (ss > best_s) {
                    best_s = ss;
                    best = sslot;
                }
            }
            if (best_s == MDL_SCORE_MIN) {
                chosen = mdl_be16(m->uni);
                tier = 3;
            } else {
                chosen = cands[best].tok;
            }
            if (tier > fallback)
                fallback = tier;

            if (chosen == TOK_EOS)
                break;
            out[out_len++] = chosen;
            if (seq_len < SEQ_MAX)
                seq[seq_len++] = chosen;

            /* verifier mirrors + stream to the 68K */
            SHARED(SH_GEN_COUNT) = (uint32_t)out_len;
            *(vu16 *)(SH_GEN_BUF) = (uint16_t)out_len;
            *(vu16 *)(SH_GEN_BUF + 2 + (out_len - 1) * 2) = chosen;
            emit_token(chosen, (uint16_t)out_len);
        }
    }

    /* done marker: 0xB000 | fallback<<6 | count (count <= 32 fits 6 bits) */
    SHARED(SH_GEN_FALLBACK) = fallback;
    COMM(2) = (uint16_t)(MSG_DONE | ((uint16_t)fallback << 6) | out_len);
    COMM(3) = 0xFFFF;
}

__attribute__((noreturn)) void cmain(void)
{
    uint16_t last_seq = 0;
    uint32_t hb = 0;
    uint16_t seq_buf[SEQ_MAX];
    int seq_len = 0;
    Model model;

    mdl_init(&model, ROM_BASE);

    for (;;) {
        SHARED(SH_HB_MASTER) = ++hb;
        COMM(6) = (uint16_t)hb;

        uint16_t cmd = COMM(0);
        uint16_t op = CMD_OP(cmd);
        uint16_t seq = CMD_SEQ(cmd);
        if (op == OP_NOP || seq == last_seq)
            continue;

        if (op == OP_WORK) {
            uint16_t seed = COMM(1);
            COMM(4) = cmd;
            uint32_t sum = m2_partial(seed, 0, M2_SPLIT);
            while (COMM(5) != seq)
                SHARED(SH_HB_MASTER) = ++hb;
            sum += SHARED(SH_SLAVE_PART);
            SHARED(SH_RESULT) = sum;
            COMM(2) = (uint16_t)(sum >> 16);
            COMM(3) = (uint16_t)sum;
            SHARED(SH_JOBS_MASTER) += 1;
            last_seq = seq;
            COMM(0) = 0;
        } else if (op == OP_TOKEN) {
            if (seq_len < SEQ_MAX - 1 - MDL_MAX_TOKENS)
                seq_buf[seq_len++] = COMM(1);
            last_seq = seq;
            COMM(0) = 0;
        } else if (op == OP_GEN) {
            int qt = COMM(1);
            if (qt > 11)
                qt = 0;
            seq_buf[seq_len++] = TOK_SEP;
            last_seq = seq;
            COMM(3) = 0;          /* clear stale done-marker before ACK */
            COMM(0) = 0;          /* ACK start; tokens stream via COMM2/3 */
            generate(&model, seq_buf, seq_len, qt);
            seq_len = 0;          /* ready for the next prompt */
        } else {
            last_seq = seq;
            COMM(0) = 0;
        }
    }
}
