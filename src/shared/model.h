/*
 * NanoCamelid 32XCD — packed model access (console side).
 *
 * The cart ROM carries the packed model files (built by nc-packer; formats
 * documented in tools/nc-model/src/lib.rs::pack). A directory at ROM offset
 * 0x4C0 locates them:
 *      8 entries x (u32 offset, u32 size), big-endian:
 *      [0] vocab.bin  [1] tokenizer.bin  [2] candidates.bin
 *      [3] weights.bin [4] decode.bin    [5] font  [6..7] reserved
 *
 * Both the 68K and the SH-2s are big-endian, so packed fields are read
 * with plain pointer access; every record is 4-byte aligned (the SH-2
 * faults on unaligned access).
 *
 * Decode constants mirror tools/nc-model/src/lib.rs — keep in sync (the
 * packer also emits decode.bin; the build asserts equality at pack time).
 */
#ifndef NC32X_MODEL_H
#define NC32X_MODEL_H

#include <stdint.h>

#define MDL_DIR_OFF   0x4C0
#define MDL_DIR_VOCAB      0
#define MDL_DIR_TOKENIZER  1
#define MDL_DIR_CANDIDATES 2
#define MDL_DIR_WEIGHTS    3
#define MDL_DIR_DECODE     4
#define MDL_DIR_FONT       5

/* decode-loop constants (== nc-model) */
#define MDL_DIM         16
#define MDL_CTX         4
#define MDL_K           16
#define MDL_TRI_TOP     8
#define MDL_BI_TOP      8
#define MDL_MAX_TOKENS  32
#define MDL_MIN_TOKENS  4
#define MDL_REP_WINDOW  8
#define MDL_REP_PEN     192
#define MDL_SH_NG       4
#define MDL_QT_W        2
#define MDL_SCORE_MIN   (-0x20000000)

#define TOK_PAD 0
#define TOK_UNK 1
#define TOK_EOS 2
#define TOK_SEP 3

/* context weights, most recent last (== nc-model CTX_W) */
#define MDL_CTX_W0 1
#define MDL_CTX_W1 1
#define MDL_CTX_W2 2
#define MDL_CTX_W3 4

typedef struct {
    /* candidates.bin */
    uint16_t vocab_len;
    uint16_t tri_count;
    uint16_t uni_count;
    const uint32_t *tri_keys;       /* [tri_count] */
    const uint8_t *tri_idx;         /* [tri_count] x 4B (u16 off, u8 len, pad) */
    const uint8_t *tri_pool;        /* entries x 4B (u16 tok, i8 score, pad) */
    const uint8_t *bi_idx;          /* [vocab_len] x 4B */
    const uint8_t *bi_pool;
    const uint8_t *uni;             /* [uni_count] x 4B */
    /* weights.bin */
    uint8_t sh_dot, sh_b;
    const int8_t *ctx_emb;          /* [vocab_len][DIM] */
    const int8_t *cand_emb;
    const int8_t *bias;             /* [vocab_len] */
    const int8_t *qtype_emb;        /* [12][DIM] */
} Model;

static inline uint32_t mdl_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8) | p[3];
}

static inline uint16_t mdl_be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

/* Locate a directory entry. rom_base: 0x22000000 on SH-2 (uncached cart),
 * 0x000000 on the 68K low mirror (entries below 64K only). */
static inline const uint8_t *mdl_dir(uint32_t rom_base, int entry, uint32_t *size_out)
{
    const uint8_t *dir = (const uint8_t *)(rom_base + MDL_DIR_OFF);
    uint32_t off = mdl_be32(dir + entry * 8);
    if (size_out)
        *size_out = mdl_be32(dir + entry * 8 + 4);
    return (const uint8_t *)(rom_base + off);
}

/* Bind Model pointers into the packed files (SH-2 side). */
static inline void mdl_init(Model *m, uint32_t rom_base)
{
    const uint8_t *c = mdl_dir(rom_base, MDL_DIR_CANDIDATES, 0);
    m->vocab_len = mdl_be16(c);
    m->tri_count = mdl_be16(c + 2);
    m->uni_count = mdl_be16(c + 4);
    const uint8_t *p = c + 8;
    m->tri_keys = (const uint32_t *)p;
    p += (uint32_t)m->tri_count * 4;
    m->tri_idx = p;
    p += (uint32_t)m->tri_count * 4;
    /* tri pool length = sum of idx lens; compute end via bi_idx start:
     * we walk the idx table once (tri_count <= ~2000, boot-time only). */
    {
        uint32_t pool_len = 0;
        uint16_t i;
        for (i = 0; i < m->tri_count; i++)
            pool_len += m->tri_idx[i * 4 + 2];
        m->tri_pool = p;
        p += pool_len * 4;
    }
    m->bi_idx = p;
    p += (uint32_t)m->vocab_len * 4;
    {
        uint32_t pool_len = 0;
        uint16_t i;
        for (i = 0; i < m->vocab_len; i++)
            pool_len += m->bi_idx[i * 4 + 2];
        m->bi_pool = p;
        p += pool_len * 4;
    }
    m->uni = p;

    const uint8_t *w = mdl_dir(rom_base, MDL_DIR_WEIGHTS, 0);
    uint16_t vlen = mdl_be16(w);
    (void)vlen;
    m->sh_dot = w[4];
    m->sh_b = w[5];
    const int8_t *q = (const int8_t *)(w + 8);
    m->ctx_emb = q;
    q += (uint32_t)m->vocab_len * MDL_DIM;
    m->cand_emb = q;
    q += (uint32_t)m->vocab_len * MDL_DIM;
    m->bias = q;
    q += m->vocab_len;
    m->qtype_emb = q;
}

#endif /* NC32X_MODEL_H */
