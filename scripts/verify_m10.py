#!/usr/bin/env python3
"""Milestone 10 verifier: the eval gauntlet.

The console runs every prompt from the embedded eval blob (422 prompts:
known/paraphrases/random/nonsense/technical) through the full multi-CPU
inference pipeline. For EVERY prompt this script asserts:
  - no crash / no hang (all prompts completed, zero error flags)
  - no blank response (>= MIN_TOKENS tokens)
  - max answer length respected
  - valid token IDs only (no PAD/UNK/SEP, within vocab)
  - console token IDs match the Rust reference token IDs exactly
  - console fallback level matches the reference fallback level
  - processors used as expected (both SH-2 scoring counters advanced)
Receipt includes the full spec field list (hashes, counters, modes).
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
FRAMES = 1200  # gauntlet finishes < 600; 2x margin
SLOT_STRIDE = 0x44
MIN_TOKENS, MAX_TOKENS = 4, 32
SPECIALS = {0, 1, 3}

SETS = [("known_questions", 121), ("paraphrases", 101),
        ("random_questions", 100), ("nonsense", 50), ("technical_questions", 50)]


def sha(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


def main():
    ref = json.load(open(f"{ROOT}/model/reference_outputs.json"))["outputs"]
    total = len(ref)
    dump_size = total * SLOT_STRIDE

    results_path = f"{ROOT}/build/m10_results.bin"
    cmd = [HARNESS, "--core", CORE, "--rom", ROM, "--frames", str(FRAMES), "--quiet",
           "--sample", "progress:workram:0x6058:2",
           "--sample", "total:workram:0x605A:2",
           "--sample", "m7err:workram:0x6056:2",
           "--sample", "m2err:workram:0x6052:2",
           "--sample", "jobs_m:sdram:0x18038:4",
           "--sample", "jobs_s:sdram:0x1803C:4",
           "--dump", f"workram:0x8000:{hex(dump_size)}:{results_path}"]
    out = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
    data = json.loads(out.stdout[out.stdout.index("{"):])
    s = data["samples"]
    buf = open(results_path, "rb").read()

    manifest = json.load(open(f"{ROOT}/model/manifest.json"))
    vocab_len = manifest["vocab_len"]

    per_prompt = []
    n_parity = n_blank = n_badlen = n_badid = n_fbmiss = 0
    fallback_hist = {0: 0, 1: 0, 2: 0, 3: 0}
    for i, entry in enumerate(ref):
        off = i * SLOT_STRIDE
        n = int.from_bytes(buf[off:off + 2], "big")
        fb = int.from_bytes(buf[off + 2:off + 4], "big")
        ids = [int.from_bytes(buf[off + 4 + j * 2:off + 6 + j * 2], "big")
               for j in range(min(n, 32))]
        parity = ids == entry["output_ids"]
        fb_ok = fb == entry["fallback"]
        blank = n < MIN_TOKENS
        badlen = n > MAX_TOKENS
        badid = any(not (0 <= t < vocab_len) or t in SPECIALS for t in ids)
        n_parity += parity
        n_blank += blank
        n_badlen += badlen
        n_badid += badid
        n_fbmiss += not fb_ok
        fallback_hist[min(fb, 3)] += 1
        per_prompt.append((parity and fb_ok and not blank and not badid, i))

    checks = {
        "all_prompts_completed": s["progress"][-1] == total == s["total"][-1],
        "no_hang_no_error": s["m7err"][-1] == 0 and s["m2err"][-1] == 0,
        "no_blank_responses": n_blank == 0,
        "max_len_respected": n_badlen == 0,
        "valid_token_ids_only": n_badid == 0,
        "token_parity_all": n_parity == total,
        "fallback_parity_all": n_fbmiss == 0,
        "both_sh2_scored": s["jobs_m"][-1] > 0 and s["jobs_s"][-1] > 0,
    }
    ok = all(checks.values())

    # per-set breakdown
    set_stats = []
    idx = 0
    for name, count in SETS:
        sl = per_prompt[idx:idx + count]
        set_stats.append({"set": name, "prompts": count,
                          "pass": sum(1 for p, _ in sl if p)})
        idx += count

    git_rev = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT,
                             capture_output=True, text=True).stdout.strip()
    receipt = {
        "milestone": 10,
        "title": "Eval gauntlet: 422 prompts, full console-vs-reference parity",
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "project_rev": git_rev,
        "emulator": "PicoDrive (libretro, local build w/ memory-access patch)",
        "rom_sha256": sha(ROM),
        "corpus_sha256": manifest["corpus_sha256"],
        "tokenizer_sha256": manifest["files"]["tokenizer.bin"]["sha256"],
        "candidate_table_sha256": manifest["files"]["candidates.bin"]["sha256"],
        "neural_weights_sha256": manifest["files"]["weights.bin"]["sha256"],
        "model_manifest_files": manifest["files"],
        "reference_outputs_sha256": sha(f"{ROOT}/model/reference_outputs.bin"),
        "scoring_mode": "split 8/8 across master+slave SH-2 (master-only when <=8 candidates)",
        "quantization_mode": manifest["quantization"],
        "prompts_total": total,
        "per_set": set_stats,
        "fallback_histogram": fallback_hist,
        "processor_role_counters": {
            "sh2_master_slots_scored": s["jobs_m"][-1],
            "sh2_slave_slots_scored": s["jobs_s"][-1],
        },
        "checks": checks,
        "pass": ok,
        "honesty_notes": [
            "CD image gate not applicable yet: model is cart-ROM-resident (M3/M9 blocked on user-supplied Sega CD BIOS); rom_sha256 stands in for cd_image_sha256.",
            "Emulator-only.",
            "Eval prompts hand-curated alongside the corpus; no synthetic teacher-model data anywhere.",
        ],
    }
    out_path = os.path.join(ROOT, "docs", "receipts",
                            f"m10-eval-{git_rev[:10] or 'nogit'}.json")
    json.dump(receipt, open(out_path, "w"), indent=2)

    for k, v in checks.items():
        print(f"  {'PASS' if v else 'FAIL'}  {k}")
    for st in set_stats:
        print(f"    {st['set']}: {st['pass']}/{st['prompts']}")
    print(f"    fallback levels: {fallback_hist}")
    print(f"receipt: {os.path.relpath(out_path, ROOT)}")
    print("VERIFY PASS (M10)" if ok else "VERIFY FAIL (M10)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
