#!/usr/bin/env bash
# Builds static FFTW (double and single precision) into a prefix, so the macOS app has
# no dylib to bundle and does not inherit the runner OS as its minimum macOS version,
# which is what linking Homebrew's bottles would do.
#
# Usage: MACOSX_DEPLOYMENT_TARGET=13.0 scripts/build-fftw.sh <prefix>
set -euo pipefail

PREFIX="${1:?usage: build-fftw.sh <prefix>}"
VERSION=3.3.10
SHA256=56c932549852cddcfafdab3820b0200c7742675be92179e59e6215b340e26467

if [ -f "$PREFIX/lib/libfftw3.a" ] && [ -f "$PREFIX/lib/libfftw3f.a" ]; then
  echo "FFTW already built in $PREFIX"
  exit 0
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
curl -fsSL --retry 3 -o "$WORK/fftw.tar.gz" "https://www.fftw.org/fftw-$VERSION.tar.gz"
echo "$SHA256  $WORK/fftw.tar.gz" | shasum -a 256 -c -
tar xzf "$WORK/fftw.tar.gz" -C "$WORK"

JOBS="$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
SIMD=""
if [ "$(uname -m)" = "arm64" ]; then
  # config.guess reports plain "arm" on macOS, and configure then refuses NEON for double
  # precision, which NR2 and NNR use. Naming the real architecture enables it.
  SIMD="--enable-neon --build=aarch64-apple-darwin"
fi

cd "$WORK/fftw-$VERSION"
for precision in "" "--enable-float"; do
  # shellcheck disable=SC2086  # the flags are deliberately word-split
  ./configure --prefix="$PREFIX" --disable-shared --enable-static --disable-fortran \
    --disable-doc $SIMD $precision >/dev/null
  make -j"$JOBS" >/dev/null
  make install >/dev/null
  make distclean >/dev/null
done
echo "FFTW $VERSION installed in $PREFIX"
