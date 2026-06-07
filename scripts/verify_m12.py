#!/usr/bin/env python3
"""Milestone 12 verifier: interactive demo mode.

Drives the demo ROM with scripted controller input — moves the on-screen
keyboard cursor, types "HI", presses START — then asserts the console
generated the same answer the reference gives for "hi", rendered it on
screen, and updated the stats line. This is the typed-prompt path end to
end: pad -> keyboard UI -> 68K tokenizer -> SH-2 inference -> render.
"""
import json
import hashlib
import os
import subprocess
import sys
from datetime import datetime, timezone

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORE = os.environ.get("PICODRIVE_CORE",
                      "/Volumes/Untitled/emu/picodrive/picodrive_libretro.dylib")
ROM = os.path.join(ROOT, "build", "nanocamelid32x_demo.32x")
HARNESS = os.path.join(ROOT, "build", "nc-headless")

# libretro joypad ids -> PicoDrive Genesis buttons:
#   RETRO B(0)->GEN B, RETRO Y(1)->GEN A, RETRO START(3)->GEN START,
#   dpad UP(4) DOWN(5) LEFT(6) RIGHT(7)
GEN_A = 1 << 1
GEN_START = 1 << 3
RIGHT = 1 << 7


def main():
    presses = []
    f = 60  # let the demo settle first

    def tap(mask, n=1):
        nonlocal f
        for _ in range(n):
            presses.append(f"{f}:{mask}")
            f += 3  # release gap for edge detection

    tap(RIGHT, 7)   # cursor A -> H
    tap(GEN_A)      # type H
    tap(RIGHT)      # -> I
    tap(GEN_A)      # type I
    tap(GEN_START)  # ask
    end_frame = f + 240  # generation + render headroom

    nt_path = f"{ROOT}/build/m12_nt.bin"
    cmd = [HARNESS, "--core", CORE, "--rom", ROM, "--frames", str(end_frame), "--quiet",
           "--dump", f"vram:0xC000:0x1000:{nt_path}",
           "--dump", f"workram:0x6100:0x80:{ROOT}/build/m12_genbuf.bin",
           "--screenshot", f"{ROOT}/build/m12_demo.ppm"]
    for p in presses:
        cmd += ["--press", p]
    out = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    assert out.returncode == 0, out.stderr

    ref = json.load(open(f"{ROOT}/model/reference_outputs.json"))["outputs"]
    entry = next(o for o in ref if o["normalized"] == "hi")

    buf = open(f"{ROOT}/build/m12_genbuf.bin", "rb").read()
    n = int.from_bytes(buf[:2], "big")
    fb = int.from_bytes(buf[2:4], "big")
    ids = [int.from_bytes(buf[4 + j * 2:6 + j * 2], "big") for j in range(min(n, 32))]

    nt = open(nt_path, "rb").read()

    def nt_row(r):
        cells = [int.from_bytes(nt[(r * 64 + c) * 2:(r * 64 + c) * 2 + 2], "big")
                 for c in range(40)]
        return "".join(chr((t & 0x7FF) + 32) for t in cells).rstrip()

    answer_rows = " ".join(nt_row(r).strip() for r in range(15, 19)).strip()
    expected = entry["decoded"].upper().replace("  ", " ")

    checks = {
        "typed_prompt_generated": n > 0,
        "ids_match_reference": ids == entry["output_ids"],
        "fallback_matches": fb == entry["fallback"],
        "answer_rendered": expected[:30] in answer_rows,
        "stats_line_updated": "TOKENS" in nt_row(20),
    }
    ok = all(checks.values())

    git_rev = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT,
                             capture_output=True, text=True).stdout.strip()
    receipt = {
        "milestone": 12,
        "title": "Interactive demo: controller-typed prompt, live inference, rendered answer",
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "project_rev": git_rev,
        "emulator": "PicoDrive (libretro, local build w/ memory-access patch + scripted input)",
        "demo_rom_sha256": hashlib.sha256(open(ROM, "rb").read()).hexdigest(),
        "typed": "HI",
        "console_ids": ids,
        "reference_ids": entry["output_ids"],
        "decoded": entry["decoded"],
        "rendered_rows": answer_rows,
        "checks": checks,
        "pass": ok,
        "honesty_notes": [
            "Input was scripted through the headless harness (--press); the same UI works with a human on a real pad in any libretro frontend.",
            "Emulator-only. Model cart-resident (CD streaming blocked on BIOS).",
        ],
    }
    out_path = os.path.join(ROOT, "docs", "receipts",
                            f"m12-demo-{git_rev[:10] or 'nogit'}.json")
    json.dump(receipt, open(out_path, "w"), indent=2)

    for k, v in checks.items():
        print(f"  {'PASS' if v else 'FAIL'}  {k}")
    print(f"    typed 'HI' -> {n} tokens: {entry['decoded']!r}")
    print(f"receipt: {os.path.relpath(out_path, ROOT)}")
    print("VERIFY PASS (M12)" if ok else "VERIFY FAIL (M12)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
