#include "infer.h"

static int push_unique(Cand *out, int n, uint16_t tok, int8_t sc)
{
    int i;
    if (n >= MDL_K)
        return n;
    for (i = 0; i < n; i++)
        if (out[i].tok == tok)
            return n;
    out[n].tok = tok;
    out[n].ng = sc;
    return n + 1;
}

int infer_candidates(const Model *m, uint16_t w1, uint16_t w2, Cand *out, uint8_t *tier)
{
    int n = 0;
    uint32_t key = ((uint32_t)w1 << 16) | w2;

    /* trigram: binary search the sorted key table */
    int lo = 0, hi = (int)m->tri_count - 1;
    int found = -1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        uint32_t k = m->tri_keys[mid];
        if (k == key) {
            found = mid;
            break;
        }
        if (k < key)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    if (found >= 0) {
        const uint8_t *idx = m->tri_idx + found * 4;
        uint16_t off = mdl_be16(idx);
        uint8_t len = idx[2];
        const uint8_t *e = m->tri_pool + (uint32_t)off * 4;
        int i;
        for (i = 0; i < len && i < MDL_TRI_TOP; i++, e += 4) {
            out[n].tok = mdl_be16(e);
            out[n].ng = (int8_t)e[2];
            n++;
        }
    }

    /* bigram top-8 (pool pre-truncated at pack time), deduped */
    {
        const uint8_t *idx = m->bi_idx + (uint32_t)w2 * 4;
        uint16_t off = mdl_be16(idx);
        uint8_t len = idx[2];
        const uint8_t *e = m->bi_pool + (uint32_t)off * 4;
        int i;
        *tier = (found >= 0) ? 0 : (len > 0 ? 1 : 2);
        for (i = 0; i < len && i < MDL_BI_TOP; i++, e += 4)
            n = push_unique(out, n, mdl_be16(e), (int8_t)e[2]);
    }

    /* unigram fill */
    {
        const uint8_t *e = m->uni;
        int i;
        for (i = 0; i < m->uni_count && n < MDL_K; i++, e += 4)
            n = push_unique(out, n, mdl_be16(e), (int8_t)e[2]);
    }
    return n;
}

void infer_context_vector(const Model *m, const uint16_t ctx[MDL_CTX], int qt,
                          int32_t cv[MDL_DIM])
{
    static const int32_t w[MDL_CTX] = { MDL_CTX_W0, MDL_CTX_W1, MDL_CTX_W2, MDL_CTX_W3 };
    int d, j;
    for (d = 0; d < MDL_DIM; d++)
        cv[d] = 0;
    for (j = 0; j < MDL_CTX; j++) {
        const int8_t *e = m->ctx_emb + (uint32_t)ctx[j] * MDL_DIM;
        for (d = 0; d < MDL_DIM; d++)
            cv[d] += w[j] * e[d];
    }
    {
        const int8_t *q = m->qtype_emb + (uint32_t)qt * MDL_DIM;
        for (d = 0; d < MDL_DIM; d++)
            cv[d] += MDL_QT_W * q[d];
    }
}

int32_t infer_score(const Model *m, const int32_t cv[MDL_DIM], uint16_t tok,
                    int8_t ng, const uint16_t *recent, int recent_len,
                    int emitted)
{
    if (tok == TOK_EOS && emitted < MDL_MIN_TOKENS)
        return MDL_SCORE_MIN;
    if (tok == TOK_PAD || tok == TOK_SEP || tok == TOK_UNK)
        return MDL_SCORE_MIN;

    {
        const int8_t *e = m->cand_emb + (uint32_t)tok * MDL_DIM;
        int32_t dot = 0;
        int d;
        for (d = 0; d < MDL_DIM; d++)
            dot += cv[d] * e[d];
        int32_t s = ((int32_t)ng << MDL_SH_NG) + (dot >> m->sh_dot)
                  + ((int32_t)m->bias[tok] << m->sh_b);
        int win = recent_len < MDL_REP_WINDOW ? recent_len : MDL_REP_WINDOW;
        int i;
        for (i = 0; i < win; i++) {
            if (recent[recent_len - 1 - i] == tok) {
                s -= MDL_REP_PEN;
                break;
            }
        }
        return s;
    }
}
