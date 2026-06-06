#!/usr/bin/env python3
"""Milestone 2 verifier: processor mailbox proof.

The 68K sends three seeded OP_WORK commands; the master SH-2 computes the
low half of a deterministic table-sum, the slave SH-2 the high half; the
master reduces and returns the result over COMM2/3 with an ACK-by-clear on
COMM0. This script recomputes the expected sums on the host (the function
mirrors src/shared/mailbox.h — keep in sync) and asserts:
  - all 3 round-trips completed, no timeout
  - all 3 results match host-computed values exactly
  - role counters: master jobs == slave jobs == 3 (slave participation proof)
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
ROM = os.path.join(ROOT, "build", "nanocamelid32x.32x")
HARNESS = os.path.join(ROOT, "build", "nc-headless")
FRAMES = 120
SEEDS = [0x1234, 0xBEEF, 0x0042]


def m2_tab(i):
    return (i * 0x9E37 + 0x4242) & 0xFFFF


def m2_full(seed):
    return sum(m2_tab(i) * ((seed + i) & 0xFFFF) for i in range(64)) % 2**32


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    cmd = [HARNESS, "--core", CORE, "--rom", ROM, "--frames", str(FRAMES), "--quiet",
           "--sample", "m2cnt:workram:0x6050:2",
           "--sample", "m2err:workram:0x6052:2",
           "--sample", "r1:workram:0x6040:4",
           "--sample", "r2:workram:0x6044:4",
           "--sample", "r3:workram:0x6048:4",
           "--sample", "jobs_m:sdram:0x18010:4",
           "--sample", "jobs_s:sdram:0x18014:4"]
    out = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    data = json.loads(out.stdout[out.stdout.index("{"):])
    s = data["samples"]

    expected = [m2_full(x) for x in SEEDS]
    got = [s["r1"][-1], s["r2"][-1], s["r3"][-1]]

    checks = {
        "all_roundtrips_completed": s["m2cnt"][-1] == 3,
        "no_timeout": s["m2err"][-1] == 0,
        "results_match_host": got == expected,
        "master_jobs_3": s["jobs_m"][-1] == 3,
        "slave_jobs_3": s["jobs_s"][-1] == 3,
    }
    ok = all(checks.values())

    git_rev = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT,
                             capture_output=True, text=True).stdout.strip()
    receipt = {
        "milestone": 2,
        "title": "Processor mailbox proof: 68K -> SH2M -> SH2S -> reduced result -> 68K",
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "project_rev": git_rev,
        "emulator": "PicoDrive (libretro, local build w/ memory-access patch)",
        "rom_sha256": sha256(ROM),
        "seeds": [hex(x) for x in SEEDS],
        "expected": [hex(x) for x in expected],
        "got": [hex(x) for x in got],
        "checks": checks,
        "pass": ok,
        "processor_roles": {
            "MAIN68K": "command issue (COMM0/1), result render, ACK-by-clear protocol",
            "SH2_MASTER": f"low-half compute + reduce + reply ({s['jobs_m'][-1]} jobs)",
            "SH2_SLAVE": f"high-half compute ({s['jobs_s'][-1]} jobs)",
            "SUB68K": "BLOCKED: Sega CD BIOS not available; CD block-load deferred to M3",
        },
        "honesty_notes": [
            "Work function is a synthetic deterministic table-sum, not model inference (that is M6+).",
            "Emulator-only.",
        ],
    }
    out_path = os.path.join(ROOT, "docs", "receipts",
                            f"m2-mailbox-{git_rev[:10] or 'nogit'}.json")
    with open(out_path, "w") as f:
        json.dump(receipt, f, indent=2)

    for k, v in checks.items():
        print(f"  {'PASS' if v else 'FAIL'}  {k}")
    print(f"receipt: {os.path.relpath(out_path, ROOT)}")
    print("VERIFY PASS (M2)" if ok else "VERIFY FAIL (M2)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
