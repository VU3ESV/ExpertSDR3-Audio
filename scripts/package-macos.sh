#!/usr/bin/env bash
# Turns a built ExpertSDR3Audio.app into a self-contained, Developer ID signed, Apple
# notarized DMG. Follows LP-700-App's scripts/package-signed.sh: the same secrets, the
# same throwaway keychain and the same retries around Apple's flakier services.
#
#   VERSION=0.2.0 scripts/package-macos.sh build/ExpertSDR3Audio.app
#
# Env, signing (from CI secrets):
#   MACOS_CERT_P12_BASE64   base64 of the Developer ID Application .p12 (cert + key)
#   MACOS_CERT_PASSWORD     the .p12 export password
#   KEYCHAIN_PASSWORD       password for the temp keychain (any value)
# Env, notarization (needs signing too):
#   NOTARY_APPLE_ID         Apple ID email
#   NOTARY_TEAM_ID          team id (Y6FT52BKDA)
#   NOTARY_PASSWORD         app-specific password for that Apple ID
# Env, options:
#   REQUIRE_SIGNING=1       fail rather than produce anything unsigned or unnotarized.
#                           Unlike LP-700's fallback: the release pipeline sets this, so a
#                           missing secret can never publish an ad-hoc build.
#   CODESIGN_IDENTITY       sign with this keychain identity instead of a .p12 (local tests)
set -euo pipefail
cd "$(dirname "$0")/.."

SRC_APP="${1:?usage: VERSION=x scripts/package-macos.sh <path to .app>}"
VERSION="${VERSION:-0.0.0-dev}"
NAME="ExpertSDR3Audio"
DIST="${DIST:-dist}"
APP="$DIST/$NAME.app"
DMG="$DIST/$NAME-$VERSION-macos-arm64.dmg"
TMP="$(mktemp -d)"
KC=""

cleanup() {
  [ -n "$KC" ] && security delete-keychain "$KC" 2>/dev/null || true
  rm -rf "$TMP"
}
trap cleanup EXIT

fail() { echo "ERROR: $*" >&2; exit 1; }

echo "==> Staging app (v$VERSION)"
mkdir -p "$DIST"
rm -rf "$APP"
ditto "$SRC_APP" "$APP"

BUILT_VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$APP/Contents/Info.plist")"
[ "$BUILT_VERSION" = "$VERSION" ] || fail "app reports version '$BUILT_VERSION', expected '$VERSION'"

echo "==> Bundling Qt"
macdeployqt "$APP" -verbose=1

# Stray extended attributes make codesign refuse with "resource fork, Finder information,
# or similar detritus not allowed".
xattr -cr "$APP"

mach_o_files() {
  find "$APP/Contents" -type f \( -perm -111 -o -name '*.dylib' \) -print0 |
    while IFS= read -r -d '' f; do
      if file -b "$f" | grep -q 'Mach-O'; then printf '%s\n' "$f"; fi
    done
}

# This release is for Apple Silicon. Qt's official frameworks are universal, so without this
# about half the app would be Intel code nothing runs.
echo "==> Removing Intel slices"
mach_o_files | while IFS= read -r f; do
  if lipo -archs "$f" | grep -q x86_64; then lipo -thin arm64 "$f" -output "$f"; fi
done

echo "==> Checking the bundle is self-contained"
LEAKS="$(mach_o_files | while IFS= read -r f; do
  # Indented lines only: the unindented ones just name the file being listed.
  otool -L "$f" | grep -E '^[[:space:]]' | awk '{print $1}' |
    grep -vE '^(@rpath|@executable_path|@loader_path|/usr/lib/|/System/Library/)' | sed "s|^|$f -> |" || true
done)"
[ -z "$LEAKS" ] || fail "the bundle links outside itself, so it would not run on another Mac:
$LEAKS"

# --- signing identity -------------------------------------------------------------------
IDENTITY="${CODESIGN_IDENTITY:-}"
if [ -z "$IDENTITY" ] && [ -n "${MACOS_CERT_P12_BASE64:-}" ]; then
  echo "==> Importing Developer ID certificate into a temporary keychain"
  KC="$TMP/signing.keychain-db"
  KCPW="${KEYCHAIN_PASSWORD:-ci-temp}"
  security create-keychain -p "$KCPW" "$KC"
  security set-keychain-settings -lut 21600 "$KC"
  security unlock-keychain -p "$KCPW" "$KC"
  echo "$MACOS_CERT_P12_BASE64" | base64 --decode > "$TMP/cert.p12"
  security import "$TMP/cert.p12" -k "$KC" -P "${MACOS_CERT_PASSWORD:-}" -T /usr/bin/codesign
  rm -f "$TMP/cert.p12"
  security set-key-partition-list -S apple-tool:,apple: -s -k "$KCPW" "$KC" >/dev/null
  # Make the temp keychain searchable (keep the existing ones too).
  # shellcheck disable=SC2046  # the list is deliberately word-split
  security list-keychains -d user -s "$KC" $(security list-keychains -d user | tr -d '"')
  # In the fresh keychain there is exactly one Developer ID identity, so selecting by name
  # is unambiguous.
  IDENTITY="$(security find-identity -v -p codesigning "$KC" |
    sed -n 's/.*"\(Developer ID Application: .*\)"/\1/p' | head -1)"
  [ -n "$IDENTITY" ] || fail "no Developer ID Application identity in MACOS_CERT_P12_BASE64"
fi

HAVE_NOTARY=0
if [ -n "$IDENTITY" ] && [ -n "${NOTARY_APPLE_ID:-}" ] && [ -n "${NOTARY_TEAM_ID:-}" ] && [ -n "${NOTARY_PASSWORD:-}" ]; then
  HAVE_NOTARY=1
fi

if [ "${REQUIRE_SIGNING:-0}" = 1 ]; then
  [ -n "$IDENTITY" ] || fail "REQUIRE_SIGNING is set, but no signing identity is available. See docs/RELEASING.md."
  [ "$HAVE_NOTARY" = 1 ] || fail "REQUIRE_SIGNING is set, but the NOTARY_* secrets are missing. See docs/RELEASING.md."
fi

# codesign with a few retries: Apple's secure-timestamp service is intermittently
# unavailable ("The timestamp service is not available.").
cs() {
  local n=1
  until codesign "$@"; do
    [ "$n" -ge 4 ] && return 1
    echo "  codesign attempt $n failed, retrying in 15s" >&2
    sleep 15
    n=$((n + 1))
  done
}

# --- sign inside-out: libraries and plugins, then frameworks, then the app -------------
if [ -n "$IDENTITY" ]; then
  echo "==> Signing with: $IDENTITY"
  SIGN=(--force --sign "$IDENTITY" --options runtime --timestamp)
  while IFS= read -r -d '' lib; do
    cs "${SIGN[@]}" "$lib"
  done < <(find "$APP/Contents/PlugIns" "$APP/Contents/Frameworks" -type f -name '*.dylib' -print0 2>/dev/null)
  while IFS= read -r -d '' framework; do
    cs "${SIGN[@]}" "$framework"
  done < <(find "$APP/Contents/Frameworks" -type d -name '*.framework' -prune -print0 2>/dev/null)
  cs "${SIGN[@]}" "$APP"
else
  echo "==> WARNING: no signing identity, ad-hoc signing. This build will NOT pass Gatekeeper."
  codesign --force --sign - --deep "$APP"
fi
codesign --verify --deep --strict --verbose=2 "$APP"

# --- notarization ----------------------------------------------------------------------
notarize() {  # $1 = path to a .zip or .dmg
  local result id status
  # Not relying on the exit status alone: when Apple rejects a build, the reason is only
  # in the submission's log, so fetch it into the CI output.
  result="$(xcrun notarytool submit "$1" \
    --apple-id "$NOTARY_APPLE_ID" --team-id "$NOTARY_TEAM_ID" --password "$NOTARY_PASSWORD" \
    --wait --output-format json)" || true
  echo "$result"
  id="$(/usr/bin/plutil -extract id raw -o - - <<<"$result" 2>/dev/null || true)"
  status="$(/usr/bin/plutil -extract status raw -o - - <<<"$result" 2>/dev/null || true)"
  if [ "$status" != "Accepted" ]; then
    if [ -n "$id" ]; then
      xcrun notarytool log "$id" \
        --apple-id "$NOTARY_APPLE_ID" --team-id "$NOTARY_TEAM_ID" --password "$NOTARY_PASSWORD" || true
    fi
    fail "notarization of $(basename "$1") ended with status '${status:-no response}'"
  fi
}

# staple with a few retries: fetching the fresh ticket from Apple's CloudKit can drop the
# connection (exit 68) or not see the ticket yet (exit 65), even after it was Accepted.
staple() {
  local n=1
  until xcrun stapler staple "$1"; do
    [ "$n" -ge 4 ] && return 1
    echo "  stapler attempt $n failed, retrying in 20s" >&2
    sleep 20
    n=$((n + 1))
  done
}

# The app first, so it carries its ticket offline and once copied out of the DMG.
if [ "$HAVE_NOTARY" = 1 ]; then
  echo "==> Notarizing the app + stapling (a few minutes)"
  ditto -c -k --keepParent "$APP" "$TMP/app.zip"
  notarize "$TMP/app.zip"
  staple "$APP"
fi

# --- DMG -------------------------------------------------------------------------------
echo "==> Building DMG"
STAGE="$TMP/stage"
mkdir -p "$STAGE"
ditto "$APP" "$STAGE/$NAME.app"
ln -s /Applications "$STAGE/Applications"
rm -f "$DMG"
# hdiutil occasionally fails on CI with "Resource busy".
n=1
until hdiutil create -volname "ExpertSDR3 Audio $VERSION" -srcfolder "$STAGE" -ov -format UDZO "$DMG" >/dev/null; do
  [ "$n" -ge 4 ] && fail "hdiutil could not create the DMG"
  echo "  hdiutil attempt $n failed, retrying in 15s" >&2
  sleep 15
  n=$((n + 1))
done

if [ -n "$IDENTITY" ]; then
  echo "==> Codesigning the DMG"
  cs --force --sign "$IDENTITY" --timestamp "$DMG"
fi
if [ "$HAVE_NOTARY" = 1 ]; then
  echo "==> Notarizing the DMG + stapling (a few minutes)"
  notarize "$DMG"
  staple "$DMG"

  # Ask Gatekeeper itself, as a user's Mac would. Anything but acceptance stops the release.
  echo "==> Asking Gatekeeper"
  spctl --assess --type execute --verbose=2 "$APP"
  spctl --assess --type open --context context:primary-signature --verbose=2 "$DMG"
  xcrun stapler validate "$APP"
  xcrun stapler validate "$DMG"
  echo "==> Notarized + stapled (app + DMG)."
else
  echo "==> Skipping notarization (no NOTARY_* secrets)."
fi
echo "==> Done: $DMG"
