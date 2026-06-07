#!/usr/bin/env python3
"""mkrom.py — assemble the NanoCamelid 32X cart ROM (M6+: model embedded).

Layout (PicoDrive 32X HLE boot contract + model directory):
  0x00000-0x001FF  68K vectors + MD header     (68K shell)
  0x00200-0x003BF  68K startup
  0x003C0-0x003FF  32X header block (patched here)
  0x004C0-0x004FF  model directory: 8 x (u32 off, u32 size) BE
                   [vocab, tokenizer, candidates, weights, decode, font]
  0x00800          master SH-2 binary (0x22000800, len <= 0x1800)
  0x02000          slave SH-2 binary  (0x22002000, len <= 0x1000)
  0x03000          Main 68K C runtime (boot.s jumps here; len <= 0x5000)
  0x08000          font (2048B)
  0x08800          vocab.bin     (68K-readable: below 64K)
  0x09800          tokenizer.bin
  0x09900          decode.bin
  0x10000          candidates.bin (4-aligned, SH-2 only)
  next 4-aligned   weights.bin
ROM is padded to 256K.

Usage: mkrom.py SHELL MASTER SLAVE MAIN68K FONT MODELDIR OUT.32x
"""
import struct
import sys

MASTER_OFF, MASTER_MAX = 0x800, 0x1800
SLAVE_OFF, SLAVE_MAX = 0x2000, 0x1000
MAIN_OFF, MAIN_MAX = 0x3000, 0x5000
FONT_OFF = 0x8000
VOCAB_OFF = 0x8800
TOKENIZER_OFF = 0x9800
DECODE_OFF = 0x9900
CAND_OFF = 0x10000
DIR_OFF = 0x4C0
ROM_SIZE = 0x40000


def main():
    shell_p, master_p, slave_p, main_p, font_p, modeldir, out_p = sys.argv[1:8]
    rd = lambda p: open(p, "rb").read()
    shell, master, slave = rd(shell_p), rd(master_p), rd(slave_p)
    main68k, font = rd(main_p), rd(font_p)
    vocab = rd(f"{modeldir}/vocab.bin")
    tokenizer = rd(f"{modeldir}/tokenizer.bin")
    decode = rd(f"{modeldir}/decode.bin")
    candidates = rd(f"{modeldir}/candidates.bin")
    weights = rd(f"{modeldir}/weights.bin")

    assert len(shell) <= DIR_OFF, f"68K shell overlaps directory: {len(shell):#x}"
    assert len(master) <= MASTER_MAX, f"master SH-2 too big: {len(master):#x}"
    assert len(slave) <= SLAVE_MAX, f"slave SH-2 too big: {len(slave):#x}"
    assert len(main68k) <= MAIN_MAX, f"68K main too big: {len(main68k):#x}"
    assert len(font) <= VOCAB_OFF - FONT_OFF
    assert len(vocab) <= TOKENIZER_OFF - VOCAB_OFF, f"vocab too big: {len(vocab):#x}"
    assert len(tokenizer) <= DECODE_OFF - TOKENIZER_OFF
    assert len(decode) <= CAND_OFF - DECODE_OFF

    weights_off = (CAND_OFF + len(candidates) + 3) & ~3
    assert weights_off + len(weights) <= ROM_SIZE, "ROM overflow"

    rom = bytearray(ROM_SIZE)
    for off, data in [(0, shell), (MASTER_OFF, master), (SLAVE_OFF, slave),
                      (MAIN_OFF, main68k), (FONT_OFF, font), (VOCAB_OFF, vocab),
                      (TOKENIZER_OFF, tokenizer), (DECODE_OFF, decode),
                      (CAND_OFF, candidates), (weights_off, weights)]:
        rom[off:off + len(data)] = data

    def patch32(off, val):
        rom[off:off + 4] = struct.pack(">I", val)

    # 32X header (PicoDrive HLE boot contract)
    patch32(0x3D4, 0x000400)                 # IDL src (unused)
    patch32(0x3D8, 0x000000)                 # IDL dst
    patch32(0x3DC, 0)                        # IDL size: none
    patch32(0x3E0, 0x22000000 | MASTER_OFF)  # master entry (uncached ROM)
    patch32(0x3E4, 0x22000000 | SLAVE_OFF)   # slave entry
    patch32(0x3E8, 0)                        # master VBR -> HLE trap table
    patch32(0x3EC, 0)                        # slave VBR

    # model directory
    entries = [(VOCAB_OFF, len(vocab)), (TOKENIZER_OFF, len(tokenizer)),
               (CAND_OFF, len(candidates)), (weights_off, len(weights)),
               (DECODE_OFF, len(decode)), (FONT_OFF, len(font)),
               (0, 0), (0, 0)]
    for i, (off, size) in enumerate(entries):
        patch32(DIR_OFF + i * 8, off)
        patch32(DIR_OFF + i * 8 + 4, size)

    open(out_p, "wb").write(rom)
    print(f"{out_p}: {ROM_SIZE:#x} bytes (shell {len(shell):#x}, "
          f"master {len(master):#x}, slave {len(slave):#x}, main {len(main68k):#x}, "
          f"cand {len(candidates):#x}@{CAND_OFF:#x}, weights {len(weights):#x}@{weights_off:#x})")


if __name__ == "__main__":
    main()
