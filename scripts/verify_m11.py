#!/usr/bin/env python3
"""Milestone 11 verifier: performance measurement.

Measures aggregate inference throughput from the eval-ROM gauntlet run
(422 prompts, ~5.1k tokens) by sampling the progress counter per frame.
Honest scope: PicoDrive's timing model does not differentiate cached vs
uncached SH-2 access or model bus contention, so these are EMULATED-TIME
numbers; real-hardware figures would differ (and the SH-2s now run from
the cached ROM mirror, which is the right configuration for hardware).
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
ROM = os.path.join(ROOT, "build", "nanocamelid32x.32x")
HARNESS = os.path.join(ROOT, "build", "nc-headless")
FRAMES = 900


def main():
    ref = json.load(open(f"{ROOT}/model/reference_outputs.json"))["outputs"]
    total_prompts = len(ref)
    total_tokens = sum(len(o["output_ids"]) for o in ref)

    cmd = [HARNESS, "--core", CORE, "--rom", ROM, "--frames", str(FRAMES), "--quiet",
           "--sample", "progress:workram:0x6058:2",
           "--sample", "jobs_m:sdram:0x18038:4",
           "--sample", "jobs_s:sdram:0x1803C:4"]
    out = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    data = json.loads(out.stdout[out.stdout.index("{"):])
    s = data["samples"]
    pr = s["progress"]

    start = next((i for i, v in enumerate(pr) if v > 0), None)
    done = next((i for i, v in enumerate(pr) if v == total_prompts), None)
    checks = {"gauntlet_completed": done is not None}
    tok_per_sec = fr_per_tok = None
    if done is not None and start is not None:
        frames_used = done - start + 1
        tok_per_sec = total_tokens / (frames_used / 60.0)
        fr_per_tok = frames_used / total_tokens
        checks["throughput_over_100_tps"] = tok_per_sec > 100
        checks["interactive_feel"] = fr_per_tok < 6  # >=10 tokens/sec visible pace

    jm, js = s["jobs_m"][-1], s["jobs_s"][-1]
    checks["slave_share_meaningful"] = js > 0 and jm > 0 and js >= jm * 0.5

    ok = all(checks.values())
    git_rev = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT,
                             capture_output=True, text=True).stdout.strip()
    receipt = {
        "milestone": 11,
        "title": "Performance measurement (emulated time)",
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "project_rev": git_rev,
        "emulator": "PicoDrive (libretro, local build w/ memory-access patch)",
        "rom_sha256": hashlib.sha256(open(ROM, "rb").read()).hexdigest(),
        "config": {"vocab": 489, "dim": 16, "K": 16, "ctx": 4,
                   "model_packed_bytes": "~38K", "sh2_code": "cached ROM mirror (0x02000000)"},
        "measured": {
            "prompts": total_prompts,
            "tokens": total_tokens,
            "gauntlet_frames": (done - start + 1) if done is not None else None,
            "tokens_per_sec_emulated": round(tok_per_sec, 1) if tok_per_sec else None,
            "frames_per_token": round(fr_per_tok, 4) if fr_per_tok else None,
            "sh2_master_slots_scored": jm,
            "sh2_slave_slots_scored": js,
            "slave_share": round(js / max(jm + js, 1), 3),
        },
        "checks": checks,
        "pass": ok,
        "honesty_notes": [
            "Emulated-time numbers: PicoDrive does not model cached-vs-uncached SH-2 access or bus contention; real hardware would be slower and the cached-ROM configuration matters there.",
            "Larger model configs (vocab 1024 / dim 32 / K 32) not exercised: the corpus yields 489 words; config sweep deferred until the corpus grows.",
            "Steps with <=8 candidates score on the master only; the slave share reflects that.",
        ],
    }
    out_path = os.path.join(ROOT, "docs", "receipts",
                            f"m11-perf-{git_rev[:10] or 'nogit'}.json")
    json.dump(receipt, open(out_path, "w"), indent=2)

    for k, v in checks.items():
        print(f"  {'PASS' if v else 'FAIL'}  {k}")
    if tok_per_sec:
        print(f"    {total_tokens} tokens in {done - start + 1} frames -> "
              f"{tok_per_sec:.0f} tok/s emulated ({fr_per_tok:.3f} frames/token)")
    print(f"    slave share of split-scored slots: {js}/{jm + js}")
    print(f"receipt: {os.path.relpath(out_path, ROOT)}")
    print("VERIFY PASS (M11)" if ok else "VERIFY FAIL (M11)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
