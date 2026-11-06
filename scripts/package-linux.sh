#!/usr/bin/env bash
# Builds a .deb and an AppImage from a finished CMake build. Runs inside the Debian
# container CI builds in, so the oldest glibc we target sets the floor for both.
#
#   scripts/package-linux.sh <version> <build dir>
#
# arm64 output is labelled for the Raspberry Pi, though it runs on any arm64 Debian.
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="${1:?usage: package-linux.sh <version> <build dir>}"
BUILD="$(cd "${2:?usage: package-linux.sh <version> <build dir>}" && pwd)"
DIST="$PWD/dist"
mkdir -p "$DIST"

case "$(dpkg --print-architecture)" in
  amd64) LABEL=linux-x86_64; TOOL_ARCH=x86_64 ;;
  arm64) LABEL=raspberrypi-arm64; TOOL_ARCH=aarch64 ;;
  *) echo "ERROR: unsupported architecture $(dpkg --print-architecture)" >&2; exit 1 ;;
esac
BASE="ExpertSDR3Audio-$VERSION-$LABEL"

echo "==> Building .deb"
(cd "$BUILD" && cpack -G DEB)
mv "$BUILD"/expertsdr3-audio-*.deb "$DIST/$BASE.deb"
dpkg-deb --info "$DIST/$BASE.deb"

echo "==> Building AppImage"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
DESTDIR="$WORK/AppDir" cmake --install "$BUILD" --prefix /usr

for tool in linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-"$TOOL_ARCH" \
            linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-"$TOOL_ARCH"; do
  curl -fsSL --retry 3 -o "$WORK/$(basename "$tool").AppImage" "https://github.com/$tool.AppImage"
  chmod +x "$WORK/$(basename "$tool").AppImage"
done

# Extract-and-run because a container has no FUSE. The offscreen platform plugin is what
# lets the smoke test open the window with no display, and a headless Pi needs no other.
export APPIMAGE_EXTRACT_AND_RUN=1
QMAKE="$(command -v qmake6)"
export QMAKE
export EXTRA_PLATFORM_PLUGINS=libqoffscreen.so
export LDAI_OUTPUT="$DIST/$BASE.AppImage"
export ARCH="$TOOL_ARCH"
export PATH="$WORK:$PATH"
"$WORK/linuxdeploy-$TOOL_ARCH.AppImage" \
  --appdir "$WORK/AppDir" \
  --plugin qt \
  --desktop-file "$WORK/AppDir/usr/share/applications/expertsdr3-audio.desktop" \
  --icon-file "$WORK/AppDir/usr/share/icons/hicolor/256x256/apps/expertsdr3-audio.png" \
  --output appimage

ls -la "$DIST"
