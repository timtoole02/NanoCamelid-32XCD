/*
 * NanoCamelid 32XCD — Genesis Main 68000 runtime (Milestone 1: title +
 * debug overlay). Entered from boot.s after the 32X handshake (M_OK/S_OK
 * observed). Runs forever.
 *
 * No globals (.data/.bss stay empty): all state lives at the fixed work-RAM
 * addresses documented in docs/architecture/memory-map.md.
 *
 * Verifier contract (unchanged from boot.s era):
 *   0xFF6000 u32 MAIN68K heartbeat
 *   0xFF6008 u16 SH2M COMM6 snapshot     0xFF600A u16 SH2S COMM7 snapshot
 *   0xFF6020 u16 mailbox-ok flag (set by boot.s)
 *   0xFF6030 u32 unexpected-exception counter
 *   0xFF6040 u32[3] M2 mailbox round-trip results
 *   0xFF6050 u16 M2 completed count   0xFF6052 u16 M2 timeout-error seq
 */
#include <stdint.h>

#define NC32X_CPU_M68K 1
#include "../shared/mailbox.h"
#include "../shared/model.h"

#define REG16(a)   (*(volatile uint16_t *)(a))
#define REG32(a)   (*(volatile uint32_t *)(a))

#define VDP_DATA   0xC00000
#define VDP_CTRL   0xC00004

#define COMM6      0xA1512C
#define COMM7      0xA1512E

#define M2_RES     0xFF6040
#define M2_CNT     0xFF6050
#define M2_ERR     0xFF6052
#define M7_CNT     0xFF6054   /* u16 generated-token count */
#define M7_ERR     0xFF6056   /* u16 timeout/error code */
#define GEN_BUF    0xFF6100   /* u16 count + u16 ids[] — verifier contract */

#define HB68K      0xFF6000
#define SH2M_SNAP  0xFF6008
#define SH2S_SNAP  0xFF600A

#define FONT_ROM   0x008000  /* cart offset 0x8000, visible in the low 64K bank */
#define ROM68K     0x880000  /* full cart ROM window when the 32X is active */
#define FONT_TILES 64

#define NAMETBL_A  0xC000
#define PLANE_W    64

#define VRAM_W(a)  (0x40000000u | (((uint32_t)(a) & 0x3FFFu) << 16) | ((uint32_t)(a) >> 14))
#define CRAM_W(a)  (0xC0000000u | (((uint32_t)(a) & 0x3FFFu) << 16))

static void vdp_reg(uint16_t r, uint16_t v)
{
    REG16(VDP_CTRL) = 0x8000 | (r << 8) | v;
}

static void vdp_addr(uint32_t cmd)
{
    REG16(VDP_CTRL) = cmd >> 16;
    REG16(VDP_CTRL) = cmd & 0xFFFF;
}

static void text(uint16_t row, uint16_t col, const char *s)
{
    vdp_addr(VRAM_W(NAMETBL_A + (row * PLANE_W + col) * 2));
    while (*s) {
        char c = *s++;
        REG16(VDP_DATA) = (uint16_t)(c - 32); /* tile index = ASCII-32, pal 0 */
    }
}

static void hex(uint16_t row, uint16_t col, uint32_t v, int digits)
{
    vdp_addr(VRAM_W(NAMETBL_A + (row * PLANE_W + col) * 2));
    for (int i = digits - 1; i >= 0; i--) {
        uint16_t n = (v >> (i * 4)) & 0xF;
        REG16(VDP_DATA) = (uint16_t)((n < 10 ? '0' + n : 'A' + n - 10) - 32);
    }
}

static void wait_vblank(void)
{
    while (REG16(VDP_CTRL) & 0x0008) {}   /* in vblank? wait for it to end */
    while (!(REG16(VDP_CTRL) & 0x0008)) {} /* then wait for the next one */
}

/* M2: send one mailbox command to the SH-2 pair and wait for the reduced
 * result. Bounded wait — a timeout latches the seq into M2_ERR. */
static uint32_t m2_cmd(uint16_t seed, uint16_t seq)
{
    COMM(1) = seed;
    COMM(0) = CMD(OP_WORK, seq);
    for (uint32_t t = 0; t < 2000000; t++) {
        REG32(HB68K)++;
        if (COMM(0) == 0)
            return ((uint32_t)COMM(2) << 16) | COMM(3);
    }
    REG16(M2_ERR) = seq;
    return 0xDEADDEADu;
}

/* ---- M7/M8: console-side tokenizer + generation protocol ------------- */

/* Look up a lowercase word in vocab.bin (u16 count, then len-prefixed
 * words in id order). Linear scan; returns TOK_UNK when absent. */
static uint16_t vocab_id(const char *w, int wlen)
{
    const uint8_t *v = mdl_dir(ROM68K, MDL_DIR_VOCAB, 0);
    uint16_t count = mdl_be16(v);
    const uint8_t *p = v + 2;
    for (uint16_t id = 0; id < count; id++) {
        int len = *p++;
        if (len == wlen) {
            int i;
            for (i = 0; i < len && p[i] == (uint8_t)w[i]; i++) {}
            if (i == len)
                return id;
        }
        p += len;
    }
    return TOK_UNK;
}

/* Copy word `id` from vocab.bin into buf (uppercased for the font),
 * returns length. */
static int vocab_word(uint16_t id, char *buf)
{
    const uint8_t *v = mdl_dir(ROM68K, MDL_DIR_VOCAB, 0);
    uint16_t count = mdl_be16(v);
    const uint8_t *p = v + 2;
    for (uint16_t i = 0; i < count; i++) {
        int len = *p++;
        if (i == id) {
            for (int j = 0; j < len; j++) {
                char c = (char)p[j];
                buf[j] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
            }
            return len;
        }
        p += len;
    }
    buf[0] = '?';
    return 1;
}

/* qtype from the first prompt token id (tokenizer.bin: u16 count + ids
 * in QTYPE_WORDS order; index+1 on match, 0 otherwise). */
static uint16_t qtype_from(uint16_t first_id)
{
    const uint8_t *t = mdl_dir(ROM68K, MDL_DIR_TOKENIZER, 0);
    uint16_t count = mdl_be16(t);
    for (uint16_t i = 0; i < count; i++)
        if (mdl_be16(t + 2 + i * 2) == first_id)
            return i + 1;
    return 0;
}

/* Send one command to the master SH-2, wait for the ACK-by-clear. */
static int send_cmd(uint16_t op, uint16_t seq, uint16_t arg)
{
    COMM(1) = arg;
    COMM(0) = CMD(op, seq);
    for (uint32_t t = 0; t < 2000000; t++) {
        REG32(HB68K)++;
        if (COMM(0) == 0)
            return 0;
    }
    REG16(M7_ERR) = (uint16_t)(0x0100 | op);
    return -1;
}

/* Tokenize the prompt (lowercase words separated by spaces), stream the
 * ids to the master, start generation, and collect the streamed tokens.
 * Result slot layout: u16 count, u16 fallback, u16 ids[<=32].
 * Returns count or -1 on timeout. */
static int run_generation(const char *prompt, uint16_t *seq, uint32_t buf)
{
    uint16_t first_id = 0xFFFF;
    const char *p = prompt;
    while (*p) {
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        const char *start = p;
        while (*p && *p != ' ')
            p++;
        uint16_t id = vocab_id(start, (int)(p - start));
        if (first_id == 0xFFFF)
            first_id = id;
        if (send_cmd(OP_TOKEN, (*seq)++, id))
            return -1;
    }
    uint16_t qt = qtype_from(first_id);
    if (send_cmd(OP_GEN, (*seq)++, qt))
        return -1;

    uint16_t n = 0;
    for (uint32_t t = 0; t < 30000000; t++) {
        uint16_t c3 = COMM(3);
        REG32(HB68K)++;
        if (c3 == 0xFFFF) {
            uint16_t done = COMM(2); /* MSG_DONE | fallback<<6 | count */
            REG16(buf) = n;
            REG16(buf + 2) = (uint16_t)((done >> 6) & 3);
            REG16(M7_CNT) = n;
            return n;
        }
        if (c3 == (uint16_t)(n + 1)) {
            uint16_t tok = COMM(2) & 0x0FFF;
            REG16(buf + 4 + n * 2) = tok;
            n++;
            REG16(buf) = n;
            COMM(1) = (uint16_t)(0x4000 | n); /* ACK */
        }
    }
    REG16(M7_ERR) = 0x0200;
    return -1;
}

/* Render generated ids as text across rows (40-col wrap). */
static void render_answer(uint16_t row, uint32_t buf, int n);

/* ---- M12: interactive demo (controller + on-screen keyboard) --------- */

#define PAD_DATA   0xA10003
#define PAD_CTRL   0xA10009
#define PB_U 0x01
#define PB_D 0x02
#define PB_L 0x04
#define PB_R 0x08
#define PB_A 0x10
#define PB_B 0x20
#define PB_C 0x40
#define PB_S 0x80

#define D_CURSOR   0xFF6062  /* (unused; cursor lives in registers) */
#define D_INLEN    0xFF6064  /* u16 input length */
#define D_PADPREV  0xFF6066  /* u16 previous pad state */
#define D_INBUF    0xFF6070  /* char[40] typed input (uppercase) */

#define REG8(a)    (*(volatile uint8_t *)(a))

static uint16_t pad_read(void)
{
    uint16_t m = 0;
    REG8(PAD_CTRL) = 0x40;
    REG8(PAD_DATA) = 0x40;
    __asm__ volatile("nop\n\tnop\n\tnop\n\tnop");
    uint8_t th1 = REG8(PAD_DATA);
    REG8(PAD_DATA) = 0x00;
    __asm__ volatile("nop\n\tnop\n\tnop\n\tnop");
    uint8_t th0 = REG8(PAD_DATA);
    REG8(PAD_DATA) = 0x40;
    if (!(th1 & 0x01)) m |= PB_U;
    if (!(th1 & 0x02)) m |= PB_D;
    if (!(th1 & 0x04)) m |= PB_L;
    if (!(th1 & 0x08)) m |= PB_R;
    if (!(th1 & 0x10)) m |= PB_B;
    if (!(th1 & 0x20)) m |= PB_C;
    if (!(th0 & 0x10)) m |= PB_A;
    if (!(th0 & 0x20)) m |= PB_S;
    return m;
}

#define KB_COLS 13
#define KB_ROWS 3
#define KB_TOP  6
#define KB_LEFT 7

static char key_char(int kx, int ky)
{
    int idx = ky * KB_COLS + kx;
    if (idx < 26) return (char)('A' + idx);
    if (idx < 36) return (char)('0' + idx - 26);
    if (idx == 36) return '\'';
    if (idx == 37) return '_';  /* space */
    return '<';                 /* backspace */
}

static void draw_keyboard(int sel_x, int sel_y)
{
    for (int ky = 0; ky < KB_ROWS; ky++) {
        vdp_addr(VRAM_W(NAMETBL_A + ((KB_TOP + ky * 2) * PLANE_W + KB_LEFT) * 2));
        for (int kx = 0; kx < KB_COLS; kx++) {
            uint16_t tile = (uint16_t)(key_char(kx, ky) - 32);
            if (kx == sel_x && ky == sel_y)
                tile |= 0x2000; /* palette line 1: highlight */
            REG16(VDP_DATA) = tile;
            REG16(VDP_DATA) = 0; /* spacing */
        }
    }
}

static void draw_input(void)
{
    uint16_t len = REG16(D_INLEN);
    char line[37];
    int i;
    for (i = 0; i < len && i < 34; i++)
        line[i] = (char)REG8(D_INBUF + i);
    line[i] = '<'; /* caret */
    for (i++; i < 35; i++)
        line[i] = ' '; /* erase stale tail */
    line[35] = 0;
    text(3, 5, line);
}

__attribute__((noreturn)) static void demo_loop(uint16_t *cmd_seq)
{
    uint16_t kx = 0, ky = 0;
    REG16(D_PADPREV) = 0;
    REG16(D_INLEN) = 0;
    for (;;) {
        wait_vblank();
        REG32(HB68K)++;
        REG16(SH2M_SNAP) = REG16(COMM6);
        REG16(SH2S_SNAP) = REG16(COMM7);
        hex(24, 27, REG16(COMM6), 4);
        hex(24, 35, REG16(COMM7), 4);

        uint16_t pad = pad_read();
        uint16_t hit = pad & ~REG16(D_PADPREV);
        REG16(D_PADPREV) = pad;

        if (hit & PB_L) kx = kx ? kx - 1 : KB_COLS - 1;
        if (hit & PB_R) kx = (uint16_t)(kx + 1 < KB_COLS ? kx + 1 : 0);
        if (hit & PB_U) ky = ky ? ky - 1 : KB_ROWS - 1;
        if (hit & PB_D) ky = (uint16_t)(ky + 1 < KB_ROWS ? ky + 1 : 0);

        uint16_t len = REG16(D_INLEN);
        if (hit & PB_A) {
            char c = key_char(kx, ky);
            if (c == '<') {
                if (len) REG16(D_INLEN) = --len;
            } else if (len < 34) {
                REG8(D_INBUF + len) = (uint8_t)(c == '_' ? ' ' : c);
                REG16(D_INLEN) = ++len;
            }
        }
        if ((hit & PB_B) && len)
            REG16(D_INLEN) = --len;
        if ((hit & PB_C) && len < 34) {
            REG8(D_INBUF + len) = ' ';
            REG16(D_INLEN) = ++len;
        }

        if ((hit & PB_S) && len) {
            char q[36];
            for (uint16_t i = 0; i < len; i++) {
                char c = (char)REG8(D_INBUF + i);
                q[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
            }
            q[len] = 0;
            text(13, 4, "THINKING...        ");
            for (uint16_t r = 15; r < 19; r++)
                text(r, 0, "                                        ");
            int n = run_generation(q, cmd_seq, GEN_BUF);
            text(13, 4, "PRESS START TO ASK ");
            if (n > 0) {
                render_answer(15, GEN_BUF, n);
                text(20, 4, "TOKENS");
                hex(20, 11, (uint16_t)n, 2);
                text(20, 15, "FALLBACK");
                hex(20, 24, REG16(GEN_BUF + 2), 1);
            } else {
                text(15, 0, "GENERATION TIMEOUT");
            }
            REG16(D_INLEN) = 0;
        }

        draw_keyboard(kx, ky);
        draw_input();
    }
}

static void render_answer(uint16_t row, uint32_t buf, int n)
{
    char line[41];
    int col = 0;
    for (int i = 0; i < n; i++) {
        char w[40];
        int wl = vocab_word(REG16(buf + 4 + i * 2), w);
        int sep = (col > 0 && !(wl == 1 && (w[0] == ',' || w[0] == '.'))) ? 1 : 0;
        if (col + sep + wl > 40) {
            line[col] = 0;
            text(row++, 0, line);
            col = 0;
            sep = 0;
        }
        if (sep)
            line[col++] = ' ';
        for (int j = 0; j < wl; j++)
            line[col++] = w[j];
    }
    if (col) {
        line[col] = 0;
        text(row, 0, line);
    }
}

__attribute__((section(".text.entry"), noreturn)) void cmain(void)
{
    (void)REG16(VDP_CTRL); /* reset control-port latch */

    vdp_reg(0, 0x04);   /* no H-int */
    vdp_reg(1, 0x44);   /* mode 5, display on, no V-int */
    vdp_reg(2, 0x30);   /* plane A at 0xC000 */
    vdp_reg(3, 0x00);   /* window off */
    vdp_reg(4, 0x07);   /* plane B at 0xE000 */
    vdp_reg(5, 0x5E);   /* sprites at 0xBC00 (unused) */
    vdp_reg(7, 0x00);   /* backdrop: pal 0 color 0 */
    vdp_reg(10, 0xFF);
    vdp_reg(11, 0x00);
    vdp_reg(12, 0x81);  /* H40 */
    vdp_reg(13, 0x2E);  /* hscroll at 0xB800 (zeroed below) */
    vdp_reg(15, 0x02);  /* autoincrement 2 */
    vdp_reg(16, 0x01);  /* planes 64x32 */

    /* clear VRAM */
    vdp_addr(VRAM_W(0));
    for (uint32_t i = 0; i < 0x8000; i++)
        REG16(VDP_DATA) = 0;

    /* palette 0: black bg, white text, green status, amber warning */
    vdp_addr(CRAM_W(0));
    REG16(VDP_DATA) = 0x0000;
    REG16(VDP_DATA) = 0x0EEE;
    REG16(VDP_DATA) = 0x00E0;
    REG16(VDP_DATA) = 0x028E;

    /* font: 64 tiles from cart ROM into VRAM 0x0000 */
    vdp_addr(VRAM_W(0));
    const uint16_t *font = (const uint16_t *)FONT_ROM;
    for (uint32_t i = 0; i < FONT_TILES * 32 / 2; i++)
        REG16(VDP_DATA) = font[i];

    /* palette 1, color 1: green key highlight for the demo keyboard */
    vdp_addr(CRAM_W(0x22));
    REG16(VDP_DATA) = 0x00E0;

    /* demo build (no eval blob in ROM): interactive chat UI */
    {
        uint32_t blob_size = 0;
        mdl_dir(ROM68K, 6, &blob_size);
        if (blob_size <= 2) {
            uint16_t cmd_seq = 1;
            text(1, 11, "NANOCAMELID 32XCD");
            text(3, 0, "ASK:");
            text(13, 4, "PRESS START TO ASK ");
            text(22, 2, "DPAD MOVE  A TYPE  B DEL  C SPACE");
            text(24, 0, "TINY TRAINED LM - EMULATOR - SH2");
            demo_loop(&cmd_seq); /* never returns */
        }
    }

    text(2, 11, "NANOCAMELID 32XCD");
    text(4, 8, "TINY LM INFERENCE ENGINE");
    text(5, 8, "M1 BOOT PROOF (EMULATOR)");
    text(8, 4, "MAIN68K");
    text(9, 4, "SH2M");
    text(10, 4, "SH2S");
    text(11, 4, "SUB68K  BLOCKED: NO CD BIOS");
    text(12, 4, "MAILBOX OK");

    /* M2: three deterministic mailbox round-trips through both SH-2s */
    static const uint16_t seeds[3] = { 0x1234, 0xBEEF, 0x0042 };
    text(14, 4, "M2 MBOX");
    for (uint16_t i = 0; i < 3; i++) {
        uint32_t r = m2_cmd(seeds[i], i + 1);
        REG32(M2_RES + 4 * i) = r;
        REG16(M2_CNT) = i + 1;
        hex(14, 13 + i * 9, r, 8);  /* cols 13/22/31, last ends at 39 (H40) */
    }

    /* M7/M8: real inference — prompts streamed to the SH-2 pair, tokens
     * streamed back. Results land in GEN_BUF slots (0x80 apart) for the
     * verifier; the last answer is rendered on screen. */
    static const char *const prompts[4] = {
        "why do llamas hum",
        "what is the sega cd",
        "how does the model work",
        "hello",
    };
    uint16_t cmd_seq = 4; /* continues after the M2 seqs 1..3 */
    int n = 0;
    for (uint16_t i = 0; i < 4; i++)
        n = run_generation(prompts[i], &cmd_seq, GEN_BUF + (uint32_t)i * 0x80);
    text(16, 0, "ASK: HELLO?");
    if (n > 0)
        render_answer(18, GEN_BUF + 3 * 0x80, n);
    else
        text(18, 0, "GENERATION TIMEOUT");

    /* M10: eval gauntlet — run every prompt from the ROM eval blob (dir
     * entry 6: u16 count, then per prompt u8 len + normalized text) and
     * store results at 0xFF8000 (stride 0x44) for the verifier. */
    {
        uint32_t blob_size = 0;
        const uint8_t *blob = mdl_dir(ROM68K, 6, &blob_size);
        if (blob_size > 2) {
            uint16_t total = mdl_be16(blob);
            const uint8_t *p = blob + 2;
            REG16(0xFF605A) = total;
            text(21, 0, "EVAL");
            for (uint16_t i = 0; i < total; i++) {
                char qbuf[64];
                uint8_t len = *p++;
                for (uint8_t j = 0; j < len && j < 63; j++)
                    qbuf[j] = (char)p[j];
                qbuf[len < 63 ? len : 63] = 0;
                p += len;
                run_generation(qbuf, &cmd_seq, 0xFF8000 + (uint32_t)i * 0x44);
                REG16(0xFF6058) = i + 1;
                if ((i & 15) == 0 || i + 1 == total) {
                    hex(21, 5, i + 1, 4);
                    text(21, 9, "/");
                    hex(21, 10, total, 4);
                }
            }
            text(21, 15, "DONE");
        }
    }

    for (;;) {
        wait_vblank();
        REG32(HB68K)++;
        uint16_t m = REG16(COMM6), s = REG16(COMM7);
        REG16(SH2M_SNAP) = m;
        REG16(SH2S_SNAP) = s;
        hex(8, 14, REG32(HB68K), 8);
        hex(9, 14, m, 4);
        hex(10, 14, s, 4);
    }
}
