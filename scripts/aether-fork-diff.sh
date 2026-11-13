#!/usr/bin/env bash
# Show what AetherSDR changed, since we forked them, in the upstream files we keep
# adapted copies of under src/gui/aether/fork/. A fork does not re-sync, so an upstream
# fix to one is only seen here. sync-aether.sh runs this after every sync.
#
# Usage: scripts/aether-fork-diff.sh /path/to/AetherSDR
#
# The forks are read from src/gui/aether/fork/README.md, one line each:
#   - Upstream: `src/gui/A.h` `src/gui/A.cpp` @ `<full commit hash>`
# Prints only; exits non-zero just for a bad argument.
set -euo pipefail

src="${1:?usage: aether-fork-diff.sh /path/to/AetherSDR}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readme="$root/src/gui/aether/fork/README.md"

git -C "$src" rev-parse --git-dir > /dev/null 2>&1 || { echo "error: $src is not a git checkout" >&2; exit 1; }
[ -f "$readme" ] || { echo "error: $readme is missing" >&2; exit 1; }

head="$(git -C "$src" rev-parse HEAD)"
found=0
while IFS= read -r line; do
  # The backticked words on the line: the upstream paths, then the commit last.
  words=()
  while IFS= read -r w; do words+=("$w"); done < <(printf '%s\n' "$line" | grep -o '`[^`]*`' | tr -d '`')
  [ "${#words[@]}" -ge 2 ] || { echo "warning: cannot read fork line: $line" >&2; continue; }
  found=$((found + 1))
  commit="${words[${#words[@]}-1]}"
  paths=("${words[@]:0:${#words[@]}-1}")

  echo "== fork of ${paths[*]} (forked at ${commit:0:8}, upstream now ${head:0:8})"
  if ! git -C "$src" cat-file -e "$commit^{commit}" 2> /dev/null; then
    echo "warning: $commit is not in $src (a shallow clone?); fetch it to compare" >&2
    continue
  fi
  log="$(git -C "$src" log --oneline "$commit..HEAD" -- "${paths[@]}")"
  if [ -z "$log" ]; then
    echo "   no upstream changes since the fork"
  else
    echo "$log" | sed 's/^/   /'
    git -C "$src" diff "$commit" HEAD -- "${paths[@]}"
    echo "   Review these against src/gui/aether/fork/ and carry over what applies."
  fi
done < <(grep -E '^- Upstream: ' "$readme")

[ "$found" -gt 0 ] || echo "warning: no '- Upstream:' lines in $readme" >&2
exit 0
