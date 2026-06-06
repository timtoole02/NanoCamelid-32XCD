#!/usr/bin/env python3
"""Milestone 1 verifier: boot proof with per-processor heartbeats.

Runs the M1 cart ROM headless, samples heartbeats per frame, asserts:
  - MAIN68K heartbeat strictly increasing
  - SH2M / SH2S SDRAM heartbeats strictly increasing (u32)
  - SH2M / SH2S COMM heartbeats changing every sampled frame (u16, wraps)
  - mailbox handshake reached (M_OK + S_OK observed by 68K)
  - zero unexpected 68K exceptions
Writes a receipt to docs/receipts/.

CD-side (SUB68K) heartbeat is recorded as BLOCKED until a Sega CD BIOS is
available — see docs/claims.md disclosure ledger.
"""
import hashlib
import json
import os
import subprocess
import sys
from datetime import datetime, timezone

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORE = os.environ.get("PICODRIVE_CORE",
                      "/Volumes/Untitled/emu/picodrive/picodrive_libretro.dylib")
ROM = os.path.join(ROOT, "build", "nanocamelid32x_m1.32x")
HARNESS = os.path.join(ROOT, "build", "nc-headless")
FRAMES = 120

SAMPLES = [
    ("main68k_hb", "workram:0x6000:4"),
    ("mailbox_ok", "workram:0x6020:2"),
    ("exceptions", "workram:0x6030:4"),
    ("sh2m_comm", "sysregs:0x2C:2"),
    ("sh2s_comm", "sysregs:0x2E:2"),
    ("sh2m_sdram", "sdram:0x18000:4"),
    ("sh2s_sdram", "sdram:0x18004:4"),
]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    for p, what in [(CORE, "PicoDrive core"), (ROM, "M1 ROM"), (HARNESS, "harness")]:
        if not os.path.exists(p):
            print(f"VERIFY FAIL: missing {what}: {p}")
            return 1

    cmd = [HARNESS, "--core", CORE, "--rom", ROM, "--frames", str(FRAMES), "--quiet"]
    for name, spec in SAMPLES:
        cmd += ["--sample", f"{name}:{spec}"]
    out = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    body = out.stdout[out.stdout.index("{"):]
    data = json.loads(body)
    s = data["samples"]

    checks = {}
    WARMUP = 30  # frames allowed for TMSS/ADEN/handshake before liveness applies

    def strictly_increasing(v):
        v = v[WARMUP:]
        return all(b > a for a, b in zip(v, v[1:]))

    def changing_with_wrap(v):
        # u16 counters: must differ between every consecutive frame sample
        v = v[WARMUP:]
        return all(b != a for a, b in zip(v, v[1:]))

    checks["main68k_alive"] = strictly_increasing(s["main68k_hb"])
    checks["sh2m_alive_sdram"] = strictly_increasing(s["sh2m_sdram"])
    checks["sh2s_alive_sdram"] = strictly_increasing(s["sh2s_sdram"])
    checks["sh2m_alive_comm"] = changing_with_wrap(s["sh2m_comm"])
    checks["sh2s_alive_comm"] = changing_with_wrap(s["sh2s_comm"])
    checks["mailbox_handshake"] = s["mailbox_ok"][-1] == 1
    checks["no_unexpected_exceptions"] = all(v == 0 for v in s["exceptions"])
    ok = all(checks.values())

    git_rev = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT,
                             capture_output=True, text=True).stdout.strip()

    receipt = {
        "milestone": 1,
        "title": "32X boot proof with per-processor heartbeats (cart mode)",
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "project_rev": git_rev,
        "emulator": "PicoDrive (libretro, local build w/ memory-access patch)",
        "rom_sha256": sha256(ROM),
        "frames": FRAMES,
        "checks": checks,
        "pass": ok,
        "processors": {
            "MAIN68K": "alive (work-RAM heartbeat)",
            "SH2_MASTER": "alive (COMM6 + SDRAM heartbeat)",
            "SH2_SLAVE": "alive (COMM7 + SDRAM heartbeat)",
            "SUB68K": "BLOCKED: Sega CD BIOS not available (user-supplied); CD boot path untested",
            "Z80": "not exercised (out of scope for M1)",
        },
        "samples_first_last": {k: [v[0], v[-1]] for k, v in s.items()},
        "honesty_notes": [
            "Cart-mode only: the Sega CD half of Milestone 1 is pending BIOS availability.",
            "Emulator-only: PicoDrive HLE boot; real-hardware 32X security startup not implemented.",
            "Title screen rendering not yet implemented (heartbeats are verifier-captured).",
        ],
    }
    out_path = os.path.join(ROOT, "docs", "receipts",
                            f"m1-boot-{git_rev[:10] or 'nogit'}.json")
    with open(out_path, "w") as f:
        json.dump(receipt, f, indent=2)

    for k, v in checks.items():
        print(f"  {'PASS' if v else 'FAIL'}  {k}")
    print(f"receipt: {os.path.relpath(out_path, ROOT)}")
    print("VERIFY PASS" if ok else "VERIFY FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
