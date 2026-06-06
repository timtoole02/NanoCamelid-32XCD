#!/usr/bin/env python3
"""mkrom.py — assemble a NanoCamelid 32X cart ROM.

Layout (matches PicoDrive's 32X HLE boot contract, see docs/hardware-notes.md):
  0x000-0x1FF  68K vectors + MD header   (from the 68K shell binary)
  0x200-0x3BF  68K startup + main code   (must stay below 0x10000)
  0x3C0-0x3FF  32X header block — patched here:
     0x3D4 u32  IDL source (ROM offset)      0x3D8 u32  IDL dest (SDRAM)
     0x3DC u32  IDL size                     0x3E0 u32  master entry pointer
     0x3E4 u32  slave entry pointer          0x3E8 u32  master VBR
     0x3EC u32  slave VBR
  0x800        master SH-2 binary (runs from uncached ROM, 0x22000800)
  0x1000       slave SH-2 binary  (0x22001000)

Usage: mkrom.py SHELL.BIN MASTER.BIN SLAVE.BIN OUT.32x
"""
import struct
import sys

MASTER_OFF = 0x800
SLAVE_OFF = 0x1000
ROM_SIZE = 0x10000  # pad to 64K (the post-ADEN 68K ROM bank size)


def main():
    shell_p, master_p, slave_p, out_p = sys.argv[1:5]
    shell = open(shell_p, "rb").read()
    master = open(master_p, "rb").read()
    slave = open(slave_p, "rb").read()

    assert len(shell) <= MASTER_OFF, f"68K shell too big: {len(shell):#x}"
    assert len(master) <= SLAVE_OFF - MASTER_OFF, f"master SH-2 too big: {len(master):#x}"
    assert len(slave) <= ROM_SIZE - SLAVE_OFF, f"slave SH-2 too big: {len(slave):#x}"

    rom = bytearray(ROM_SIZE)
    rom[: len(shell)] = shell
    rom[MASTER_OFF : MASTER_OFF + len(master)] = master
    rom[SLAVE_OFF : SLAVE_OFF + len(slave)] = slave

    def patch32(off, val):
        rom[off : off + 4] = struct.pack(">I", val)

    patch32(0x3D4, 0x000400)            # IDL src (unused)
    patch32(0x3D8, 0x000000)            # IDL dst
    patch32(0x3DC, 0)                   # IDL size: no initial data load
    patch32(0x3E0, 0x22000000 | MASTER_OFF)  # master entry (uncached ROM)
    patch32(0x3E4, 0x22000000 | SLAVE_OFF)   # slave entry
    patch32(0x3E8, 0)                   # master VBR -> HLE trap table
    patch32(0x3EC, 0)                   # slave VBR

    open(out_p, "wb").write(rom)
    print(f"{out_p}: {ROM_SIZE:#x} bytes (shell {len(shell):#x}, "
          f"master {len(master):#x} @ {MASTER_OFF:#x}, slave {len(slave):#x} @ {SLAVE_OFF:#x})")


if __name__ == "__main__":
    main()
