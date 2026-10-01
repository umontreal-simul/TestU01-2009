#!/bin/bash
# Build this repository's TestU01 and the checkpointed Crush/BigCrush driver.
#
# Run from anywhere inside a clone of this repository:
#     bash checkpointing/build_driver.sh [BUILD_DIR]
# BUILD_DIR defaults to ~/testu01_ckpt_build. Nothing outside BUILD_DIR is touched.
#
# The two source patches (GCC 15 K&R declarations; six `static` keywords removed
# from bbattery.c) are already applied in the repository tree, so this script only
# configures, builds and installs TestU01 and then compiles the driver:
#     BUILD_DIR/pgprng_testu01_ckpt_driver
#
# Using the driver: it reads raw 64-bit words from stdin and takes three arguments:
#     <mode> <battery> <checkpoint file>
# for example   <your generator> | BUILD_DIR/pgprng_testu01_ckpt_driver hi bigcrush bc.ckpt
# where mode is hi, lo, hi_rev or lo_rev and battery is crush or bigcrush. If the
# run is interrupted, start it again with the same command and the same checkpoint
# file; the generator must continue at the word count recorded in that checkpoint
# (see README.md, "Checkpoint design").
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
BUILD="${1:-$HOME/testu01_ckpt_build}"
mkdir -p "$BUILD"
BUILD="$(cd "$BUILD" && pwd)"
case "$BUILD" in *" "*) echo "BUILD_DIR must not contain spaces: $BUILD" >&2; exit 1;; esac

[ -x "$REPO/configure" ] || { echo "No ./configure in $REPO -- run this inside a TestU01-2009 clone." >&2; exit 1; }

echo "=== [1/3] copying source to $BUILD/src ==="
rm -rf "$BUILD/src"
mkdir -p "$BUILD/src"
( cd "$REPO" && tar --exclude=.git --exclude=checkpointing -cf - . ) | tar -xf - -C "$BUILD/src"

echo "=== [2/3] configure + make + install (logs in $BUILD/*.log) ==="
cd "$BUILD/src"
./configure --prefix="$BUILD/install" > "$BUILD/configure.log" 2>&1 || { echo "configure failed -- see $BUILD/configure.log" >&2; exit 1; }
make -j"$(nproc 2>/dev/null || echo 2)" > "$BUILD/make.log" 2>&1 || { echo "make failed -- see $BUILD/make.log" >&2; exit 1; }
make install > "$BUILD/install.log" 2>&1 || { echo "make install failed -- see $BUILD/install.log" >&2; exit 1; }
cp "$HERE/src/bbattery_ckpt_internal.h" "$BUILD/install/include/"

echo "=== [3/3] compiling the checkpointed driver ==="
gcc -O2 -Wall -I"$BUILD/install/include" -I"$BUILD/src/include" \
    -o "$BUILD/pgprng_testu01_ckpt_driver" \
    "$HERE/src/pgprng_testu01_ckpt_driver.c" \
    "$BUILD/install/lib/libtestu01.a" "$BUILD/install/lib/libprobdist.a" "$BUILD/install/lib/libmylib.a" -lm
echo "Built: $BUILD/pgprng_testu01_ckpt_driver"
