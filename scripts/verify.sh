#!/bin/bash
# make verify — run all milestone verifiers that are currently implemented.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# ensure harness + ROM are built
make -s -C "$ROOT/tools/emulator"
make -s -C "$ROOT/src"

python3 "$ROOT/scripts/verify_m1.py"
python3 "$ROOT/scripts/verify_m2.py"
python3 "$ROOT/scripts/verify_m45.py"
python3 "$ROOT/scripts/verify_m78.py"
python3 "$ROOT/scripts/verify_m10.py"
python3 "$ROOT/scripts/verify_m11.py"
python3 "$ROOT/scripts/verify_m12.py"
