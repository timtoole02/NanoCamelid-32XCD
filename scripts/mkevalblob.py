#!/usr/bin/env python3
"""mkevalblob.py — build the console eval-prompt blob + the combined raw
question list for the reference runtime.

Canonical set order: known_questions, paraphrases, random_questions,
nonsense, technical_questions. The blob stores pre-normalized text
(lowercase [a-z0-9']+ words joined by spaces) — this MUST normalize
identically to nc-model::normalize for prompt-token parity, so commas and
periods are forbidden in eval questions (asserted here).

Blob format: u16 count BE, then per prompt: u8 len + bytes.

Usage: mkevalblob.py EVALDIR OUT.blob OUT_all.txt
"""
import re
import struct
import sys

SETS = ["known_questions.txt", "paraphrases.txt", "random_questions.txt",
        "nonsense.txt", "technical_questions.txt"]
MINIMUMS = [50, 100, 100, 50, 50]


def main():
    evaldir, out_blob, out_all = sys.argv[1:4]
    raw_lines = []
    set_sizes = []
    for fname, need in zip(SETS, MINIMUMS):
        lines = [l.strip() for l in open(f"{evaldir}/{fname}") if l.strip()]
        assert len(lines) >= need, f"{fname}: {len(lines)} < required {need}"
        for l in lines:
            assert "," not in l and "." not in l, f"comma/period breaks parity: {l!r}"
        raw_lines.extend(lines)
        set_sizes.append(len(lines))

    blob = struct.pack(">H", len(raw_lines))
    for l in raw_lines:
        words = re.findall(r"[a-z0-9']+", l.lower())
        assert words, f"no words: {l!r}"
        norm = " ".join(words)
        assert len(norm) <= 63, f"too long for console buffer: {l!r}"
        blob += struct.pack("B", len(norm)) + norm.encode()

    open(out_blob, "wb").write(blob)
    open(out_all, "w").write("\n".join(raw_lines) + "\n")
    print(f"{out_blob}: {len(raw_lines)} prompts ({len(blob)} bytes); "
          f"sets {dict(zip(SETS, set_sizes))}")


if __name__ == "__main__":
    main()
