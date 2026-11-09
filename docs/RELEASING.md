# Releasing

[`release.yml`](../.github/workflows/release.yml) builds every platform, signs what can be
signed, and publishes a GitHub Release. It publishes only if every platform built, passed its
tests and smoke tests, and the Mac build was signed and notarized. A partial release, or an
unsigned Mac build, never goes out.

## Starting a release

The triggers match LP-700-App:

| Trigger | Version |
|---|---|
| Merge to `main` | The latest tag plus one patch. Skipped if only docs changed. |
| Push a `vX.Y.Z` tag | That tag |
| `gh workflow run release.yml -f version=X.Y.Z` | That version, tagging the current commit. Add `-f prerelease=true` to keep it from becoming "latest". |

Pull requests run [`ci.yml`](../.github/workflows/ci.yml), which does the same builds and tests
through the same [`build.yml`](../.github/workflows/build.yml), but unsigned and with nothing
published. It can also be started by hand, for just the platforms you name.

## What gets published

| Platform | Files | Signing |
|---|---|---|
| macOS 13+ (Apple Silicon) | `-macos-arm64.dmg` | Developer ID, hardened runtime, notarized and stapled, on both the app and the DMG |
| Windows 10+ (x64) | `-windows-x64-setup.exe`, `-windows-x64-portable.zip` | **Not Authenticode signed**, see below. Sigstore provenance |
| Raspberry Pi (64-bit OS) | `-raspberrypi-arm64.deb`, `.AppImage` | Sigstore provenance |
| Linux x86-64 | `-linux-x86_64.deb`, `.AppImage` | Sigstore provenance |

Plus `SHA256SUMS`.

Linux has no equivalent of notarization, so a signature tying each file to this repository and
workflow run is what signing can mean there. It needs no key to manage, because GitHub's
identity signs it through Sigstore, and anyone can check it:

```sh
gh attestation verify <file> --repo VU3ESV/ExpertSDR3-Audio
```

GitHub only offers this on public repositories, unless on its Enterprise plan, so the step skips
itself on a private one. v0.1.0 was built while this repository was still private and has
checksums only; releases after it carry these signatures.

To fix one platform without waiting for the others:

```sh
gh workflow run ci.yml --ref <branch> -f platforms="linux windows"
```

## macOS signing secrets

These are the same six secrets as LP-700-App (its `docs/SIGNING-SECRETS.md`), with the same
meaning. Each has to be set on this repository too, since GitHub never lets a secret be read
back:

| Secret | What it is |
|---|---|
| `MACOS_CERT_P12_BASE64` | The Developer ID Application cert and key, exported as a `.p12` and base64-encoded |
| `MACOS_CERT_PASSWORD` | The password set when exporting that `.p12` |
| `KEYCHAIN_PASSWORD` | Any value: the password for the throwaway CI keychain |
| `NOTARY_APPLE_ID` | Apple ID email used for notarization |
| `NOTARY_TEAM_ID` | The team id, `Y6FT52BKDA` |
| `NOTARY_PASSWORD` | An app-specific password for that Apple ID |

To set them, run this on the Mac holding the certificate. It walks through the export, checks
the `.p12` and the Apple credentials before uploading anything, and never writes a secret to
disk:

```sh
scripts/setup-signing-secrets.sh
```

Unlike LP-700-App, a release with missing secrets fails rather than publishing an ad-hoc build.
The one exception is an automatic release from a merge to `main`, which skips with a notice
instead, so that merging before signing is set up does not turn the branch red.

This shares LP-700's certificate. Revoking that certificate would affect this app as well.

## Windows code signing

The Windows builds are not Authenticode signed, so SmartScreen warns on first run. Nothing here
can fix that without a Windows certificate: an Apple certificate cannot sign for Windows, and no
Windows certificate exists in this or any sibling project.

The usual routes, for when one is chosen:

- **[SignPath Foundation](https://signpath.org/):** free for open source. It needs an application
  and a review, and the certificate names the Foundation as publisher rather than you.
- **An OV certificate from a commercial CA:** roughly $150 to $300 a year. Since the 2023 CA/B
  Forum rules, its private key reportedly has to live in hardware, so CI signs through the
  vendor's cloud service rather than from an exported `.pfx`.
- **[Azure Artifact Signing](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/code-signing-options):**
  cheap, but limited to individuals in the USA and Canada.

There is no placeholder for this in the pipeline, because each route works differently and an
unused path could not be tested.

## Rehearsing the Mac build locally

Everything except Apple's notary service can be run on a Mac with the identity in its keychain,
using official Qt rather than Homebrew's, as CI does:

```sh
MACOSX_DEPLOYMENT_TARGET=13.0 scripts/build-fftw.sh "$PWD/.deps/fftw"
PKG_CONFIG_PATH="$PWD/.deps/fftw/lib/pkgconfig" cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DESA_STATIC_PORTAUDIO=ON -DESA_VERSION=0.1.0 "-DCMAKE_PREFIX_PATH=<Qt>/macos;$PWD/.deps/fftw"
cmake --build build-release

VERSION=0.1.0 CODESIGN_IDENTITY=<SHA-1 from security find-identity> \
  scripts/package-macos.sh build-release/ExpertSDR3Audio.app
SMOKE_METHODS="nr2 nr4 mnr dfnr rn2 nnr" scripts/smoke-test.sh \
  dist/ExpertSDR3Audio.app/Contents/MacOS/ExpertSDR3Audio 0.1.0 native
```

## Checking the custom window frame by hand

Gear > Window frame chooses between the system's frame, the default on every platform, and
AetherRX's (Custom). Tests run offscreen, where windows have no frame at all, so they check the
window flags, the chips and the switching, but not how a frame looks or behaves on a desktop.
Before a release that changes `src/gui/shell/WindowChrome.*`, and before making Custom any
platform's default, go through that platform's list on a real screen with a release build.

Every platform:

- Switching System to Custom and back from the gear applies at once, with no restart. The
  window comes back on the same page, with its content where it was and the same size;
  maximised, it stays maximised. The choice is still there after quitting and starting again.
- Closing from the custom frame (its close chip, or a Mac's red button) restores ExpertSDR3's
  mute, as every other way out does.

macOS (Custom needs Qt 6.9 or later; release builds use 6.10):

- The traffic lights sit over the ground at the top left, with no title text and no title bar
  background behind them, and the header starts below them, clear of them.
- Dragging the strip the traffic lights are in moves the window, and double-clicking it zooms
  or minimises as System Settings says. Every edge resizes.
- Full screen (the green button, or Control-Command-F): the strip goes and the header moves up,
  then comes back down on leaving. On a MacBook with a notch, nothing sits under the notch.
  Switching the frame while full screen leaves the window full screen.
- It is still the main window: Stage Manager and Mission Control show it and switch to it,
  and the menu bar's commands (Find Radios, Profiles, About) work while it is in front, as
  they do with the system frame.
- On a Retina and a non-Retina screen, and moved from one to the other.
- Switching keeps the window's content area where it was, and in Custom the strip under the
  traffic lights is part of it: judge whether the window's top edge moving by the title bar's
  height, and the pages losing that height, is acceptable.

Windows 10 and 11:

- Dragging the header's empty part (the title, or the space before the status chip) moves the
  window. Dragged to a screen's edge or corner it snaps there, and to the top it maximises.
  Win+Arrow keys snap, maximise and minimise it.
- Double-clicking the header's empty part maximises it, and again restores it. Maximised,
  however it got there, it stops at the taskbar rather than covering it.
- Every edge and corner resizes it, with the resize cursor, from a 6 px band, and nothing in
  the header, the rail or a page is lost under that band.
- The minimise chip minimises; the taskbar button restores and minimises; the close chip
  quits as Alt+F4 does.
- Windows 11 rounds the corners; Windows 10 leaves them square. Note whether the window
  casts a shadow on each.
- At 100 %, 150 % and 200 % display scaling, and moved between screens scaled differently:
  the header, its chips and the resize band keep their size and stay sharp.

Linux and the Raspberry Pi:

- On Wayland (GNOME and KDE) and on X11, with and without a compositor (the Pi's own desktop
  among them): the header moves the window, double-clicking it maximises, the edges resize,
  and the chips minimise and close.
- No black corners on X11 without a compositor: the window is opaque and square.
- Where the window comes back after switching the frame on Wayland: changing the frame shows
  the window again, and Wayland lets the compositor place a window it shows.
- On the Pi's touchscreen: dragging the header with a finger, and tapping both chips.

## Checking REC, PLAY and Recordings by hand

Tests replace the file manager and the folder dialog, so neither has run for real. Before a
release that changes recording or the Recordings menu, on each platform with a release build:

- Gear > Recordings > Folder... opens the system's folder dialog at the folder, or at the
  nearest folder above it that exists, and creates nothing. The folder chosen is where the
  next REC records.
- Show in Finder (Explorer, Files) shows the last recording, selected where the platform can
  select it, or the folder when there is no recording yet.
- macOS, signed build, a fresh user: starting the app never asks for access to Documents. The
  first time REC records into the default folder, the Recordings menu opens, or Folder...
  opens its dialog, macOS may ask once. Refuse it once, and check that REC says why it could
  not record, that its warning's Choose Folder... button opens the folder dialog, that REC
  then records into the folder chosen there, and that the window carries on.

## Checking the EQ page by hand

Tests drag the curve with synthetic mouse events and feed the analyser MockExpert's tone, so
neither a hand on a mouse or trackpad nor a radio's audio has gone through them. Before a release
that changes `src/gui/pages/EqPage.cpp`, `src/gui/binding/EqFeed.*` or the vendored EQ panel, with
a release build and a radio:

- Dragging a band's handle changes the sound while the drag goes on, smoothly, not only when the
  button is let go; so does Shift-dragging it for its Q.
- The analyser follows the radio's audio while the EQ page shows, and the OUT strip shows its
  level. Both fall away within a moment when the stream stops or BYPASS is on.
- A type icon, a handle's right-click menu (its type, and the slope of a high- or low-pass) and
  Reset each change the sound at once, and Modified follows.
- On a Raspberry Pi, the window's CPU with the EQ page showing, against the Gate page, and
  again while dragging a handle: each step of a drag restyles the band readings.

## Checking Final Output by hand

Tests use outputs that play nothing, so nobody has listened. Before a release that changes the
Final Output page or the output path, on each platform with a release build and a radio:

- By ear: the Volume knob turns from silence-quiet at -60 dB to +30 dB, with 0 dB at
  12 o'clock sounding as loud as the old 0 dB; BOOST lifts quiet audio without crackle on
  peaks; MUTE silences this app at once, and only this app: ExpertSDR3's own audio does what
  the Radio page's Mute ExpertSDR3 says, and clicking MUTE again brings the sound back.
- The meter and the waveform follow what is heard: they rise with Volume and BOOST. Under
  MUTE they keep moving, as AetherSDR's do, showing what would play. With no radio the scope reads "Not connected", and with a radio sending no
  audio "No audio from radio", never "Enable PC Audio".
- The knob turns with a Mac trackpad, one notch per swipe, and the waveform's footer shows the
  radio's rate (48 kHz, or 24 kHz with `--rate 24000`).
- Plug in a USB headset, press Refresh: it is listed, choosing it moves the sound there, and
  unplugging it leaves it in the list as "(not found)" until it comes back.
- On the Pi: GUI CPU with Final Output on screen while audio plays (the waveform redraws at
  30 Hz on Linux on ARM), and the page scrolls rather than cutting off the scope on its 480-line screen.

## Choices worth knowing about

- **CI cannot test DFNR on macOS, so a release does not either.** GitHub's Mac runners are virtual
  machines, where DFNR's library crashes for want of Apple's AMX coprocessor, so the app does not
  offer it there. The local rehearsal above runs on real hardware and does cover it. Repeat it
  whenever `vendor/aethersdr` changes.

- **Qt 6.10, not the 6.8 LTS:** Qt 6.8.3 links Apple's AGL framework, which current macOS SDKs
  have removed, and later 6.8 patches are commercial-only.
- **Debian Bookworm builds the Linux packages:** the oldest supported system sets the glibc floor
  (2.36), and CI installs the results on Bookworm, Trixie, Ubuntu 24.04 and Ubuntu 26.04 to prove
  they work. Ubuntu 22.04 is too old.
- **The `.deb` lists its dependencies by hand:** generating them adds Debian's strict
  `qt6-websockets-abi (= 6.4.2)`, and the package then refuses to install on anything newer.
- **No JACK on Linux:** PortAudio is linked statically without it, because an AppImage linking
  `libjack` will not start on most desktops. Audio reaches PipeWire and PulseAudio through ALSA.
- **The AppImage needs a desktop's libraries:** `libOpenGL`, `libharfbuzz`, `libX11` and others
  are left to the host on purpose. The list CI tests against is in `build.yml`.
- **linuxdeploy comes from its rolling `continuous` release,** as it publishes no stable ones, so
  an AppImage build can break through no change here.
- **Disconnecting and quitting wait for every recording to be written out,** however long the
  disk takes, so no file is left with a stale header. ExpertSDR3's mute is restored first. A
  folder on a disk that stops answering (a network share, a USB disk pulled mid-write) would
  hang the disconnect, and the window with it on Quit; no test can stall a real disk, so check
  it by hand before a release that changes recording: record to a network folder, cut the
  network, and quit.
- **The DeepFilterNet library is AetherSDR's prebuilt one,** checked against the SHA-256 pinned in
  its setup script, and built from source if unavailable.
- **A double-click on an Exciter knob lands on AetherSDR's default,** 6 dB for Body drive and 30 %
  for either mix, where this app's defaults are 0 dB and 50 %. The page is AetherSDR's own. Our
  defaults stay because a value left at its default is not stored: changing one would change the
  sound of everyone who never moved that knob.
