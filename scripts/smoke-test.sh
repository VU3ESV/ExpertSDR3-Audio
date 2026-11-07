#!/usr/bin/env bash
# Checks a packaged binary really works, the way the unit tests cannot: run from its
# package with its bundled or declared libraries, with no build tree to lean on.
#
#   scripts/smoke-test.sh <command that starts the app> <expected version> [gui platform]
#
# The gui platform is `offscreen`, `native`, or `skip` where no window system exists.
# Needs no radio or sound device: the port is closed, and audio goes to the null output.
set -uo pipefail

APP="${1:?usage: smoke-test.sh <app> <version> [offscreen|native|skip]}"
VERSION="${2:?usage: smoke-test.sh <app> <version> [offscreen|native|skip]}"
GUI="${3:-offscreen}"
METHODS_EXPECTED="${SMOKE_METHODS:-nr2 nr4 dfnr rn2 nnr}"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
failures=0

# Fails this script at the end rather than at once, so one log shows every problem.
check() {
  if "$@"; then echo "  ok: $CHECK"; else echo "  FAILED: $CHECK"; failures=$((failures + 1)); fi
}
contains() { printf '%s' "$OUT" | grep -qF -- "$1"; }

run() { OUT="$("$APP" "$@" 2>&1)"; RC=$?; }
status_ok() { [ "$RC" -eq 0 ]; }

echo "Smoke testing: $APP"

run --version
CHECK="--version reports $VERSION (got: $OUT)"; check contains "$VERSION"

run --list-params
CHECK="--list-params exits cleanly"; check status_ok
CHECK="--list-params lists the chain"; check contains "comp.threshold"

run --list-profiles --settings "$WORK/profiles.ini"
CHECK="--list-profiles exits cleanly"; check status_ok

run --list-devices
CHECK="--list-devices exits cleanly (PortAudio loads)"; check status_ok

# Every method constructs in the packaged build. This is what catches a missing model or a
# library that did not get bundled: DFNR once failed in the real app with tests still green.
for method in $METHODS_EXPECTED; do
  run --headless --host 127.0.0.1 --port 1 --device null --duration 4 \
      --settings "$WORK/$method.ini" --nr "$method"
  CHECK="$method starts"
  check contains '"nrFailed":false'
  CHECK="$method finished preparing"
  check contains '"nrPreparing":false'
done

if [ "$GUI" != skip ]; then
  SHOT="$WORK/window.png"
  if [ "$GUI" = offscreen ]; then
    QT_QPA_PLATFORM=offscreen "$APP" --settings "$WORK/gui.ini" --screenshot "$SHOT" >/dev/null 2>&1
  else
    "$APP" --settings "$WORK/gui.ini" --screenshot "$SHOT" >/dev/null 2>&1
  fi
  check_shot() { [ -s "$SHOT" ]; }
  CHECK="the window opens and renders ($GUI)"; check check_shot
fi

if [ "$failures" -ne 0 ]; then
  echo "$failures smoke check(s) failed"
  exit 1
fi
echo "All smoke checks passed"
