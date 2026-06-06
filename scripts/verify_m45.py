#!/usr/bin/env python3
"""Milestone 4+5 verifier: trainer determinism, packed-model integrity,
reference-runtime determinism, verifier round-trip, output sanity gates.

Everything here is host-side (no console). Console parity lands at M7.
"""
import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile
from datetime import datetime, timezone

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
T = os.path.join(os.environ.get("CARGO_TARGET_DIR",
                                "/Volumes/Untitled/cargo-targets/nanocamelid-32XCD"), "release")
CORPUS = os.path.join(ROOT, "assets", "corpus", "corpus.txt")
MODEL = os.path.join(ROOT, "model")
EVAL = os.path.join(ROOT, "docs", "eval", "known_questions.txt")

MIN_TOKENS, MAX_TOKENS = 4, 32
SPECIALS = {0, 1, 3}  # PAD, UNK, SEP must never be emitted (EOS=2 is stripped)


def sha(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


def run(*cmd):
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    if r.returncode != 0:
        print(r.stdout, r.stderr)
        raise RuntimeError(f"command failed: {cmd}")
    return r


def main():
    checks = {}
    with tempfile.TemporaryDirectory() as tmp:
        # 1. trainer determinism: retrain -> byte-identical trained.ncm
        run(f"{T}/nc-trainer", CORPUS, f"{tmp}/trained.ncm")
        checks["trainer_deterministic"] = sha(f"{tmp}/trained.ncm") == sha(f"{MODEL}/trained.ncm")

        # 2. packer determinism: repack -> byte-identical console bins
        run(f"{T}/nc-packer", CORPUS, f"{tmp}/trained.ncm", tmp)
        bins = ["vocab.bin", "tokenizer.bin", "candidates.bin", "weights.bin", "decode.bin"]
        checks["packer_deterministic"] = all(
            sha(f"{tmp}/{b}") == sha(f"{MODEL}/{b}") for b in bins)

        # 3. manifest hashes describe the actual files
        manifest = json.load(open(f"{MODEL}/manifest.json"))
        checks["manifest_hashes_valid"] = all(
            manifest["files"][b]["sha256"] == sha(f"{MODEL}/{b}") for b in bins)
        checks["manifest_corpus_hash"] = manifest["corpus_sha256"] == sha(CORPUS)

        # 4. reference determinism: re-evaluate -> byte-identical outputs
        run(f"{T}/nc-reference", MODEL, "--eval", EVAL,
            f"{tmp}/ref.json", f"{tmp}/ref.bin")
        checks["reference_deterministic"] = (
            sha(f"{tmp}/ref.bin") == sha(f"{MODEL}/reference_outputs.bin"))

        # 5. output sanity gates over every eval prompt
        ref = json.load(open(f"{MODEL}/reference_outputs.json"))["outputs"]
        vocab_len = manifest["vocab_len"]
        checks["no_blank_outputs"] = all(len(o["output_ids"]) >= MIN_TOKENS for o in ref)
        checks["max_len_respected"] = all(len(o["output_ids"]) <= MAX_TOKENS for o in ref)
        checks["valid_token_ids"] = all(
            0 <= t < vocab_len and t not in SPECIALS
            for o in ref for t in o["output_ids"])
        checks["eval_count"] = len(ref) >= 100

        # 6. verifier round-trip: reference vs itself -> PASS; corrupted -> FAIL
        ids = ref[0]["output_ids"]
        con = struct.pack(">H", len(ids)) + b"".join(struct.pack(">H", t) for t in ids)
        open(f"{tmp}/con.bin", "wb").write(con)
        ok_run = subprocess.run([f"{T}/nc-verifier", f"{MODEL}/reference_outputs.bin",
                                 "0", f"{tmp}/con.bin"], capture_output=True, text=True)
        bad = bytearray(con)
        bad[3] ^= 0xFF
        open(f"{tmp}/bad.bin", "wb").write(bytes(bad))
        bad_run = subprocess.run([f"{T}/nc-verifier", f"{MODEL}/reference_outputs.bin",
                                  "0", f"{tmp}/bad.bin"], capture_output=True, text=True)
        checks["verifier_detects_parity"] = ok_run.returncode == 0 and "PARITY PASS" in ok_run.stdout
        checks["verifier_detects_divergence"] = bad_run.returncode == 1 and "PARITY FAIL" in bad_run.stdout

    ok = all(checks.values())
    git_rev = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT,
                             capture_output=True, text=True).stdout.strip()
    receipt = {
        "milestone": "4+5",
        "title": "Host trainer + packer + Rust reference runtime",
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "project_rev": git_rev,
        "corpus_sha256": sha(CORPUS),
        "model_manifest": manifest,
        "reference_outputs_sha256": sha(f"{MODEL}/reference_outputs.bin"),
        "eval_prompts": len(ref),
        "checks": checks,
        "pass": ok,
        "honesty_notes": [
            "Reference runtime is host-side; console parity is Milestone 7.",
            "Corpus is hand-curated (no synthetic teacher-model data used).",
            "Topic-bias conditioning deferred to v2; qtype conditioning only.",
            "Quality is corpus-memorization-with-blending by design; this gate checks structure/determinism, not usefulness.",
        ],
    }
    out_path = os.path.join(ROOT, "docs", "receipts",
                            f"m45-model-{git_rev[:10] or 'nogit'}.json")
    json.dump(receipt, open(out_path, "w"), indent=2)

    for k, v in checks.items():
        print(f"  {'PASS' if v else 'FAIL'}  {k}")
    print(f"receipt: {os.path.relpath(out_path, ROOT)}")
    print("VERIFY PASS (M4+M5)" if ok else "VERIFY FAIL (M4+M5)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
