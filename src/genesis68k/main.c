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

#define REG16(a)   (*(volatile uint16_t *)(a))
#define REG32(a)   (*(volatile uint32_t *)(a))

#define VDP_DATA   0xC00000
#define VDP_CTRL   0xC00004

#define COMM6      0xA1512C
#define COMM7      0xA1512E

#define M2_RES     0xFF6040
#define M2_CNT     0xFF6050
#define M2_ERR     0xFF6052

#define HB68K      0xFF6000
#define SH2M_SNAP  0xFF6008
#define SH2S_SNAP  0xFF600A

#define FONT_ROM   0x008000  /* cart offset 0x8000, visible in the low 64K bank */
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
        if (COMM(0) == 0)
            return ((uint32_t)COMM(2) << 16) | COMM(3);
    }
    REG16(M2_ERR) = seq;
    return 0xDEADDEADu;
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
