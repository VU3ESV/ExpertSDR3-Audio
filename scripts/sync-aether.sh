#!/usr/bin/env bash
# Copy the AetherRX chain subset from an AetherSDR checkout into vendor/aethersdr,
# unmodified and at upstream's relative paths, and record the commit it came from.
#
# Usage: scripts/sync-aether.sh /path/to/AetherSDR
#
# To take an upstream DSP update: pull AetherSDR, run this, rebuild, run ctest.
set -euo pipefail

src="${1:?usage: sync-aether.sh /path/to/AetherSDR}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dst="$root/vendor/aethersdr"

[ -f "$src/src/core/ClientEq.h" ] || { echo "error: $src is not an AetherSDR checkout" >&2; exit 1; }

core_files=(
  ClientEq.h ClientEq.cpp
  ClientGate.h ClientGate.cpp
  ClientComp.h ClientComp.cpp
  ClientPhaseRotator.h ClientPhaseRotator.cpp
  ClientTube.h ClientTube.cpp
  ClientPudu.h ClientPudu.cpp
  Resampler.h Resampler.cpp
  RNNoiseFilter.h RNNoiseFilter.cpp
  SpectralNR.h SpectralNR.cpp Nr2GammaTables.inc
  SpecbleachFilter.h SpecbleachFilter.cpp
  MacNRFilter.h MacNRFilter.cpp
  DeepFilterFilter.h DeepFilterFilter.cpp
  NnrFilter.h NnrFilter.cpp NnrControls.h
  dsp/FftwPlannerLock.h dsp/FftwPlannerLock.cpp
)
tp_dirs=(rnnoise r8brain libspecbleach wdsp)

rm -rf "$dst"
mkdir -p "$dst/src/core/dsp" "$dst/third_party/deepfilter" "$dst/scripts/setup"

for f in "${core_files[@]}"; do cp "$src/src/core/$f" "$dst/src/core/$f"; done
for d in "${tp_dirs[@]}"; do cp -R "$src/third_party/$d" "$dst/third_party/$d"; done

# DeepFilterNet: header, model and licences only. The static library is
# per-platform and large, so scripts/setup/setup-deepfilter.* fetches it.
cp -R "$src/third_party/deepfilter/include" "$dst/third_party/deepfilter/include"
cp -R "$src/third_party/deepfilter/models"  "$dst/third_party/deepfilter/models"
cp "$src/third_party/deepfilter/"{COMMIT,LICENSE,LICENSE-APACHE,LICENSE-MIT} "$dst/third_party/deepfilter/"
cp "$src/scripts/setup/"{setup-deepfilter.sh,setup-deepfilter.ps1,_verify_sha256.sh,_verify_sha256.ps1} "$dst/scripts/setup/"

cp "$src/LICENSE" "$src/THIRD_PARTY_LICENSES" "$dst/"
git -C "$src" rev-parse HEAD > "$dst/UPSTREAM_COMMIT"

echo "Vendored AetherSDR $(cat "$dst/UPSTREAM_COMMIT") -> $dst ($(du -sh "$dst" | cut -f1))"
