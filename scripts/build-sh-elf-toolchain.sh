#!/bin/bash
# Build sh-elf binutils + GCC (C only, bare metal, SH-2 big-endian) for NanoCamelid 32XCD.
# Prefix: /Volumes/Untitled/toolchains/sh-elf
set -euo pipefail

TC=/Volumes/Untitled/toolchains
PREFIX=$TC/sh-elf
BUILD=$TC/build
BREW=$(brew --prefix)
JOBS=10

BINUTILS_VER=2.43
GCC_VER=14.2.0

mkdir -p "$BUILD" "$PREFIX"
cd "$BUILD"

# --- fetch sources ---
[ -f binutils-$BINUTILS_VER.tar.xz ] || curl -fLO https://ftp.gnu.org/gnu/binutils/binutils-$BINUTILS_VER.tar.xz
[ -f gcc-$GCC_VER.tar.xz ]           || curl -fLO https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VER/gcc-$GCC_VER.tar.xz
[ -d binutils-$BINUTILS_VER ] || tar xf binutils-$BINUTILS_VER.tar.xz
[ -d gcc-$GCC_VER ]           || tar xf gcc-$GCC_VER.tar.xz

export PATH="$PREFIX/bin:$PATH"

# --- binutils ---
if [ ! -x "$PREFIX/bin/sh-elf-as" ]; then
  rm -rf b-binutils && mkdir b-binutils && cd b-binutils
  ../binutils-$BINUTILS_VER/configure --target=sh-elf --prefix="$PREFIX" \
    --disable-nls --disable-werror --with-system-zlib
  make -j$JOBS MAKEINFO=true
  make install MAKEINFO=true
  cd "$BUILD"
fi
echo "=== binutils done ==="

# --- gcc (C only, no headers/libc; libgcc for m2 multilib) ---
GCC_COMMON_OPTS="--target=sh-elf --prefix=$PREFIX --enable-languages=c \
  --without-headers --with-newlib --disable-nls --disable-shared \
  --disable-threads --disable-libssp --disable-libquadmath \
  --disable-decimal-float --disable-libgomp --disable-libatomic \
  --with-system-zlib --with-gmp=$BREW --with-mpfr=$BREW --with-mpc=$BREW"

rm -rf b-gcc && mkdir b-gcc && cd b-gcc
if ! ../gcc-$GCC_VER/configure $GCC_COMMON_OPTS --with-multilib-list=m2; then
  echo "--- multilib-list=m2 rejected, retrying with defaults ---"
  rm -rf ../b-gcc && mkdir ../b-gcc && cd ../b-gcc
  ../gcc-$GCC_VER/configure $GCC_COMMON_OPTS
fi
make -j$JOBS MAKEINFO=true all-gcc
make -j$JOBS MAKEINFO=true all-target-libgcc
make MAKEINFO=true install-gcc install-target-libgcc
cd "$BUILD"
echo "=== gcc done ==="

"$PREFIX/bin/sh-elf-gcc" --version | head -1
echo "SH-ELF TOOLCHAIN BUILD COMPLETE"
