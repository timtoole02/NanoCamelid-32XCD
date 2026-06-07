/* Shared SH-2 inference core — exact mirror of tools/nc-model integer
 * semantics. See lib.rs header comment for the score contract. */
#ifndef NC32X_INFER_H
#define NC32X_INFER_H

#include "model.h"

typedef struct {
    uint16_t tok;
    int8_t ng;
} Cand;

/* Candidate merge for context (w1, w2): trigram top-8, bigram top-8
 * deduped, unigram fill; returns count (<= MDL_K). */
int infer_candidates(const Model *m, uint16_t w1, uint16_t w2, Cand *out);

/* cv[DIM] from the last CTX tokens (PAD-padded, most recent last) + qtype. */
void infer_context_vector(const Model *m, const uint16_t ctx[MDL_CTX], int qt,
                          int32_t cv[MDL_DIM]);

/* Integer score for one candidate. recent/emitted implement the
 * repetition penalty and EOS gating exactly as the reference. */
int32_t infer_score(const Model *m, const int32_t cv[MDL_DIM], uint16_t tok,
                    int8_t ng, const uint16_t *recent, int recent_len,
                    int emitted);

#endif
