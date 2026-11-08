#!/usr/bin/env bash
# Copy the AetherRX chain subset from an AetherSDR checkout into vendor/aethersdr,
# unmodified and at upstream's relative paths, and record the commit it came from.
#
# Usage: scripts/sync-aether.sh /path/to/AetherSDR
#
# To take an upstream update: pull AetherSDR, run this, rebuild, run ctest. A sync is
# its own pull request, and the Qt 6.4.2 (Debian Bookworm) build must pass before it
# merges: upstream targets newer Qt than Bookworm and the Raspberry Pi ship.
set -euo pipefail

src="${1:?usage: sync-aether.sh /path/to/AetherSDR}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dst="$root/vendor/aethersdr"
shim="$root/src/gui/aether/shim"
fork="$root/src/gui/aether/fork"

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
  ThemeManager.h ThemeManager.cpp ThemeSeedGenerated.cpp
)
tp_dirs=(rnnoise r8brain libspecbleach wdsp)

# The GUI page family, from the frozen list (comments and blank lines skipped).
gui_files=()
while IFS= read -r line || [ -n "$line" ]; do
  case "$line" in ''|'#'*) continue ;; esac
  gui_files+=("$line")
done < "$root/scripts/aether-gui-files.txt"
[ "${#gui_files[@]}" -gt 0 ] || { echo "error: scripts/aether-gui-files.txt lists no files" >&2; exit 1; }

# Keep what setup-deepfilter.sh made: its static library (git-ignored, per platform) and the
# header cbindgen wrote with it. That header is the tracked one, and upstream's committed copy
# differs from it, so copying upstream's over it would change a tracked file on every sync.
keep="$(mktemp -d)"
trap 'rm -rf "$keep"' EXIT
df="$dst/third_party/deepfilter"
[ -d "$df/lib" ] && cp -R "$df/lib" "$keep/lib"
[ -f "$df/include/deep_filter.h" ] && cp "$df/include/deep_filter.h" "$keep/deep_filter.h"
[ -f "$df/COMMIT" ] && cp "$df/COMMIT" "$keep/COMMIT"

rm -rf "$dst"
mkdir -p "$dst/src/core/dsp" "$dst/src/gui" "$dst/third_party/deepfilter" "$dst/third_party/inter" \
         "$dst/resources/themes" "$dst/scripts/setup"

for f in "${core_files[@]}"; do cp "$src/src/core/$f" "$dst/src/core/$f"; done
for f in "${gui_files[@]}"; do mkdir -p "$(dirname "$dst/src/gui/$f")"; cp "$src/src/gui/$f" "$dst/src/gui/$f"; done
for d in "${tp_dirs[@]}"; do cp -R "$src/third_party/$d" "$dst/third_party/$d"; done

# DeepFilterNet: header, model and licences only. The static library is
# per-platform and large, so scripts/setup/setup-deepfilter.* fetches it.
cp -R "$src/third_party/deepfilter/include" "$dst/third_party/deepfilter/include"
cp -R "$src/third_party/deepfilter/models"  "$dst/third_party/deepfilter/models"
cp "$src/third_party/deepfilter/"{COMMIT,LICENSE,LICENSE-APACHE,LICENSE-MIT} "$dst/third_party/deepfilter/"
cp "$src/scripts/setup/"{setup-deepfilter.sh,setup-deepfilter.ps1,_verify_sha256.sh,_verify_sha256.ps1} "$dst/scripts/setup/"
[ -f "$keep/deep_filter.h" ] && cp "$keep/deep_filter.h" "$df/include/deep_filter.h"
[ -d "$keep/lib" ] && cp -R "$keep/lib" "$df/lib"
if [ -f "$keep/COMMIT" ] && ! cmp -s "$keep/COMMIT" "$df/COMMIT"; then
  echo "note: upstream moved DeepFilterNet to $(cat "$df/COMMIT"). The library and header kept here" >&2
  echo "      are for the old one, so run setup-deepfilter.sh (see the reminder below)." >&2
fi

# The one theme the pages are drawn in, and the UI font it names (SIL OFL).
cp "$src/resources/themes/default-dark.json" "$dst/resources/themes/"
cp "$src/third_party/inter/"{Inter-Regular.ttf,Inter-Bold.ttf,LICENSE.txt} "$dst/third_party/inter/"

cp "$src/LICENSE" "$src/THIRD_PARTY_LICENSES" "$dst/"
git -C "$src" rev-parse HEAD > "$dst/UPSTREAM_COMMIT"

# Include check: every quoted #include in the GUI files, the theme files and our forks
# must resolve to a vendored file, a shim header, a fork or a Qt header, searched as
# CMake's aether_gui target searches. A new upstream dependency then fails here, by
# name, instead of as a compile error in CI.
resolves() {  # <including file> <included name>
  local from="$1" name="$2" dir
  case "$name" in *.moc|moc_*) return 0 ;; Qt*/*) return 0 ;; esac  # moc output, <QtCore/...>
  [[ "$name" == Q[A-Z]* && "$name" != *.* ]] && return 0                 # "QWidget" and the like
  for dir in "$(dirname "$from")" "$shim" "$shim/core" "$fork" "$dst/src" "$dst/src/gui" "$dst/src/core"; do
    [ -f "$dir/$name" ] && return 0
  done
  return 1
}
strays=0
while IFS= read -r f; do
  while IFS= read -r name; do
    if ! resolves "$f" "$name"; then
      echo "error: ${f#"$root"/} includes \"$name\", which is neither vendored nor shimmed" >&2
      strays=$((strays + 1))
    fi
  done < <(sed -n 's/^[[:space:]]*#[[:space:]]*include[[:space:]]*"\([^"]*\)".*/\1/p' "$f")
done < <(find "$dst/src/gui" "$fork" -type f \( -name '*.h' -o -name '*.cpp' \) | sort
         printf '%s\n' "$dst/src/core/ThemeManager.h" "$dst/src/core/ThemeManager.cpp" \
                       "$dst/src/core/ThemeSeedGenerated.cpp")
if [ "$strays" -gt 0 ]; then
  echo "error: $strays unresolved include(s). Add the file to scripts/aether-gui-files.txt," >&2
  echo "       or give it a shim in src/gui/aether/shim/, then sync again." >&2
  exit 1
fi

echo "Vendored AetherSDR $(cat "$dst/UPSTREAM_COMMIT") -> $dst ($(du -sh "$dst" | cut -f1))"

# What upstream changed in the files we forked, so those fixes are not missed.
"$root/scripts/aether-fork-diff.sh" "$src"

cat <<'EOF'

Reminder: a sync is its own pull request. If upstream moved DeepFilterNet (see any note
above), or this checkout never had the library, run
(cd vendor/aethersdr && bash scripts/setup/setup-deepfilter.sh). Rebuild, run ctest,
and do not merge until the Bookworm job (Qt 6.4.2, GCC 12) passes.
EOF
