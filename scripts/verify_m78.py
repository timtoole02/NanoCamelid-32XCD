#!/usr/bin/env python3
"""Milestone 6+7+8 verifier: console inference parity vs the Rust reference.

The console runs four prompts through the full pipeline (68K tokenize ->
mailbox -> SH-2 candidate merge + split scoring -> greedy decode -> token
stream back). This script:
  - dumps the four GEN_BUF slots and compares token IDs against
    reference_outputs.json entries (matched by normalized prompt text)
  - runs nc-verifier on slot 0 for the canonical parity receipt
  - asserts both SH-2s actually scored (role counters)
  - asserts the last answer is rendered on screen (nametable text)
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
T = os.path.join(os.environ.get("CARGO_TARGET_DIR",
                                "/Volumes/Untitled/cargo-targets/nanocamelid-32XCD"), "release")
FRAMES = 600

PROMPTS = ["why do llamas hum", "what is the sega cd",
           "how does the model work", "hello"]


def sha(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


def main():
    dumps = []
    cmd = [HARNESS, "--core", CORE, "--rom", ROM, "--frames", str(FRAMES), "--quiet",
           "--sample", "m7err:workram:0x6056:2",
           "--sample", "jobs_m:sdram:0x18038:4",
           "--sample", "jobs_s:sdram:0x1803C:4",
           "--dump", f"vram:0xC000:0x1000:{ROOT}/build/m78_nt.bin"]
    for i in range(4):
        p = f"{ROOT}/build/m78_slot{i}.bin"
        dumps.append(p)
        cmd += ["--dump", f"workram:{hex(0x6100 + i * 0x80)}:0x80:{p}"]
    out = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    data = json.loads(out.stdout[out.stdout.index("{"):])
    s = data["samples"]

    ref = json.load(open(f"{ROOT}/model/reference_outputs.json"))["outputs"]
    by_norm = {o["normalized"]: (i, o) for i, o in enumerate(ref)}

    checks = {"no_timeout_error": s["m7err"][-1] == 0}
    slot_results = []
    for i, prompt in enumerate(PROMPTS):
        ridx, entry = by_norm[prompt]
        buf = open(dumps[i], "rb").read()
        # slot layout: u16 count, u16 fallback, u16 ids[]
        n = int.from_bytes(buf[:2], "big")
        fb = int.from_bytes(buf[2:4], "big")
        ids = [int.from_bytes(buf[4 + j * 2:6 + j * 2], "big") for j in range(min(n, 60))]
        match = ids == entry["output_ids"] and fb == entry["fallback"]
        checks[f"slot{i}_parity"] = match
        slot_results.append({
            "prompt": prompt, "reference_index": ridx,
            "console_ids": ids, "reference_ids": entry["output_ids"],
            "console_fallback": fb, "reference_fallback": entry["fallback"],
            "decoded": entry["decoded"], "match": match,
        })

    # canonical parity check through the Rust verifier (slot 0, repacked
    # into the console.bin format: u16 count + ids)
    import struct as _struct
    s0 = slot_results[0]["console_ids"]
    repack = f"{ROOT}/build/m78_slot0_ids.bin"
    open(repack, "wb").write(_struct.pack(">H", len(s0))
                             + b"".join(_struct.pack(">H", t) for t in s0))
    v = subprocess.run([f"{T}/nc-verifier", f"{ROOT}/model/reference_outputs.bin",
                        str(by_norm[PROMPTS[0]][0]), repack],
                       capture_output=True, text=True)
    checks["nc_verifier_pass"] = v.returncode == 0 and "PARITY PASS" in v.stdout

    # both SH-2s actually scored candidates (slave participation proof)
    checks["master_scored"] = s["jobs_m"][-1] > 0
    checks["slave_scored"] = s["jobs_s"][-1] > 0

    # the answer is rendered on screen
    nt = open(f"{ROOT}/build/m78_nt.bin", "rb").read()

    def nt_row(r):
        cells = [int.from_bytes(nt[(r * 64 + c) * 2:(r * 64 + c) * 2 + 2], "big")
                 for c in range(40)]
        return "".join(chr((t & 0x7FF) + 32) for t in cells).strip()

    rendered = nt_row(18)
    expected_render = by_norm["hello"][1]["decoded"].upper()[:40].strip()
    checks["answer_rendered"] = rendered.startswith(expected_render[:20])

    ok = all(checks.values())
    git_rev = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT,
                             capture_output=True, text=True).stdout.strip()
    manifest = json.load(open(f"{ROOT}/model/manifest.json"))
    receipt = {
        "milestone": "6+7+8",
        "title": "SH-2 scoring kernel + multi-token generation, token parity vs reference",
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "project_rev": git_rev,
        "emulator": "PicoDrive (libretro, local build w/ memory-access patch)",
        "rom_sha256": sha(ROM),
        "model_manifest": manifest,
        "reference_outputs_sha256": sha(f"{ROOT}/model/reference_outputs.bin"),
        "prompts": slot_results,
        "score_slot_counters": {"master": s["jobs_m"][-1], "slave": s["jobs_s"][-1]},
        "checks": checks,
        "pass": ok,
        "processor_roles": {
            "MAIN68K": "tokenize (vocab from ROM), mailbox protocol, render",
            "SH2_MASTER": "candidate merge, context vector, low-slot scoring, reduce, decode loop",
            "SH2_SLAVE": "high-slot scoring (counters prove participation)",
            "SUB68K": "BLOCKED: Sega CD BIOS unavailable; model is cart-ROM-resident until M3/M9",
        },
        "honesty_notes": [
            "Model data is read from cart ROM, not streamed from CD (that is M3/M9, blocked on BIOS).",
            "Emulator-only.",
            "Candidate scoring is split 8/8 across the SH-2s when K=16 candidates exist; steps with <=8 candidates run master-only.",
        ],
    }
    out_path = os.path.join(ROOT, "docs", "receipts",
                            f"m78-inference-{git_rev[:10] or 'nogit'}.json")
    json.dump(receipt, open(out_path, "w"), indent=2)

    for k, v2 in checks.items():
        print(f"  {'PASS' if v2 else 'FAIL'}  {k}")
    for r in slot_results:
        print(f"    {r['prompt']!r}: {len(r['console_ids'])} tokens -> {r['decoded']!r}")
    print(f"receipt: {os.path.relpath(out_path, ROOT)}")
    print("VERIFY PASS (M6+M7+M8)" if ok else "VERIFY FAIL (M6+M7+M8)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
