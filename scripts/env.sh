# source scripts/env.sh — toolchain + build environment for NanoCamelid 32XCD
export NC32X_ROOT="/Volumes/Untitled/nanocamelid-32XCD"
export PATH="/Volumes/Untitled/toolchains/sh-elf/bin:$PATH"
# dedicated cargo target dir on the T7 (avoids global cargo-lock stall, spares internal disk)
export CARGO_TARGET_DIR="/Volumes/Untitled/cargo-targets/nanocamelid-32XCD"
export PICODRIVE_CORE="/Volumes/Untitled/emu/picodrive/picodrive_libretro.dylib"
export CD_BIOS="$NC32X_ROOT/assets/bios/bios_CD_U.bin"
