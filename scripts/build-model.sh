#!/bin/bash
# make model — train, pack, and generate reference outputs (M4 + M5).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export CARGO_TARGET_DIR="${CARGO_TARGET_DIR:-/Volumes/Untitled/cargo-targets/nanocamelid-32XCD}"
T="$CARGO_TARGET_DIR/release"

cargo build --release --quiet --manifest-path "$ROOT/Cargo.toml"
"$T/nc-trainer" "$ROOT/assets/corpus/corpus.txt" "$ROOT/model/trained.ncm"
"$T/nc-packer" "$ROOT/assets/corpus/corpus.txt" "$ROOT/model/trained.ncm" "$ROOT/model"
"$T/nc-reference" "$ROOT/model" --eval "$ROOT/docs/eval/known_questions.txt" \
    "$ROOT/model/reference_outputs.json" "$ROOT/model/reference_outputs.bin"
