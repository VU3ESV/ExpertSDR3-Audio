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

## Updating AetherSDR

`vendor/aethersdr` holds AetherSDR's files exactly as upstream has them, at the commit in
`vendor/aethersdr/UPSTREAM_COMMIT`. Only `scripts/sync-aether.sh` writes there. A file that has
to change to build here is forked into `src/gui/aether/fork/` instead, and
[its README](../src/gui/aether/fork/README.md) records where each fork came from and exactly
what differs. A sync is its own pull request, never mixed with other changes.

1. Check out the AetherSDR commit to take, then, from this repository:

   ```sh
   scripts/sync-aether.sh /path/to/AetherSDR
   ```

   It replaces `vendor/aethersdr` with the DSP files, the theme, Inter and the setup scripts it
   lists itself, and the GUI files listed in `scripts/aether-gui-files.txt`, and records the
   commit. Then it checks that every quoted `#include` in those GUI files and in the forks
   resolves to a vendored file, a shim in `src/gui/aether/shim/`, a fork or Qt, and runs the
   fork diff below.
2. If the include check names a file, upstream's pages need something new. Add it to
   `scripts/aether-gui-files.txt` (its header says how the list was made), or give it a shim,
   and sync again. Never edit a vendored file: one that will not build on Qt 6.4.2 is forked.
3. Read the fork diff. For each fork it prints either "no upstream changes since the fork", or
   upstream's commits and diff since the commit the fork was taken at. Carry over what applies,
   then update that fork's `- Upstream:` commit, its line ranges and its exact diff in the fork
   README. Each fork that takes only some of upstream's lines has `diff -u` commands to run in
   the AetherSDR checkout. NrGainStrip is a whole-file fork: compare upstream's two files with
   the fork's with a plain `diff -u`. Either way they must print exactly the diff the README
   records. To see the fork diff again:

   ```sh
   scripts/aether-fork-diff.sh /path/to/AetherSDR
   ```

4. If the sync says upstream moved DeepFilterNet, run
   `(cd vendor/aethersdr && bash scripts/setup/setup-deepfilter.sh)`.
5. Build and run every test:

   ```sh
   cmake --build build && ctest --test-dir build --output-on-failure
   ```

   Two tests are there for a sync. `test_aether_gui` is the sync-safety contract: it renders
   each vendored page against the shims, then checks in the real window every object the window
   finds by name, every setting each page edits, in both directions, that opening every page
   changes nothing, and each correction made from outside (the Volume knob, the makeup handle,
   wheel steps, value fields, the lit mode buttons, the AetherVoice wordmark, the NR tabs, the
   scope's note). A failure names what moved: fix the lookup, the page's list of settings or
   the correction in the same pull request. `test_look` compares every page with its golden
   picture, so a sync that changes how a page draws fails there, but only on a computer that
   has a set in `tests/golden`: every CI job, and macOS 27 on Apple Silicon with Qt 6.10.
   Check that ctest lists it as Passed, not Skipped. Where it is skipped, look at the renders
   in CI's `look-*` artifacts by eye. If the new look is upstream's intent,
   regenerate the goldens as [Look goldens](#look-goldens) says.
6. Do not merge until CI's Debian Bookworm jobs pass. Bookworm and Raspberry Pi OS ship Qt
   6.4.2 and GCC 12, and upstream targets a newer Qt. Before the next release, rehearse the Mac
   build as above, which is the only place DFNR is tested, and go through the hand checks below
   for every page whose vendored files changed.

## Checking by hand before a release

The tests run offscreen, against a simulated radio and outputs that play nothing. Some things
only a person at a real screen, with a radio, can check: how it sounds, PLAY's pacing, a real
drag of a rail grip, knobs on a Mac trackpad, the custom frame on each platform, the Documents
prompt on a signed Mac build, and a Raspberry Pi's CPU and touchscreen. Go through the
sections that cover what a release changes, on each platform, with a release build.

### Listening

Before a release that changes the chain, a stage page or the output path, with a radio on a
band with voices:

- Tick AetherNR and each stage in turn: each changes the sound as its page says, at once, with
  no click or dropout, and unticking it takes the change away.
- Turn a knob on each stage page while listening: the sound follows the knob as it turns, not
  only when it is let go.
- BYPASS gives the radio unprocessed at once, and a second click brings the chain back.
- Switching between two saved profiles: each sounds as it did when it was saved.
- With Mute ExpertSDR3's own audio on, press Disconnect while the app plays: the status chip
  says Disconnecting… for a moment (at most about a second and a half), then Not connected,
  and ExpertSDR3's own audio is back.
- On Windows: the button at the foot of the rail shows a gear (⚙), not an empty box. The
  offscreen tests draw an empty box there, having no symbol font to fall back on; a desktop's
  should come from Segoe UI Symbol. And Ctrl+Q quits.

### The rail and the knobs

A rail grip starts a drag that offscreen tests cannot run, and they have no trackpad. Before a
release that changes the rail or a page with knobs:

- Drag a stage by its grip above and below the others: it lands where it is dropped, the sound
  follows the new order, and the order is still there after restarting. Radio, AetherNR and
  Final Output neither move nor take a stage dropped on them.
- Every knob turns with a Mac trackpad, one notch per swipe, on each stage page and on Final
  Output, and with a mouse wheel.

### Checking the custom window frame by hand

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
- In Custom, drag the window to the top of the screen, then switch to System: the window moves
  down so that its whole title bar is on the screen, a few pixels below the top edge (a window
  as tall as the screen also gets a little shorter). Lower down, switching leaves it where it
  was.

Linux and the Raspberry Pi:

- On Wayland (GNOME and KDE) and on X11, with and without a compositor (the Pi's own desktop
  among them): the header moves the window, double-clicking it maximises, the edges resize,
  and the chips minimise and close.
- No black corners on X11 without a compositor: the window is opaque and square.
- Where the window comes back after switching the frame on Wayland: changing the frame shows
  the window again, and Wayland lets the compositor place a window it shows.
- On the Pi's touchscreen: dragging the header with a finger, and tapping both chips.

### Checking REC, PLAY and Recordings by hand

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
- By ear: record a minute of a voice, then PLAY it. It sounds as it did live, at the same speed
  and pitch, with no dropouts, and stops by itself at the end; the header's ▶ time keeps pace
  with a clock. The file also plays in another player.

### Checking the EQ page by hand

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

### Checking Final Output by hand

Tests use outputs that play nothing, so nobody has listened. Before a release that changes the
Final Output page or the output path, on each platform with a release build and a radio:

- By ear: the Volume knob turns from silence-quiet at -60 dB to +30 dB, with 0 dB at
  12 o'clock sounding as loud as the old 0 dB; BOOST lifts quiet audio without crackle on
  peaks; MUTE silences this app at once, and only this app: ExpertSDR3's own audio does what
  the Radio page's Mute ExpertSDR3's own audio setting says, and clicking MUTE again brings the
  sound back.
- The meter and the waveform follow what is heard: they rise with Volume and BOOST. Under
  MUTE they keep moving, as AetherSDR's do, showing what would play. With no radio the scope reads "Not connected", and with a radio sending no
  audio "No audio from radio", never "Enable PC Audio".
- The waveform's footer shows the radio's rate (48 kHz, or 24 kHz with `--rate 24000`).
- Plug in a USB headset, press Refresh: it is listed, choosing it moves the sound there, and
  unplugging it leaves it in the list as "(not found)" until it comes back.

### Measuring on a Raspberry Pi

Upstream's pages redraw every 16 ms while they are on screen, so the window's own CPU matters
on a Pi. This needs a Pi 4 or 5 running the 64-bit Raspberry Pi OS desktop, ideally on the
7-inch touchscreen, the release `.deb`, and a radio. Before a release that changes a page, and
before calling the Pi supported:

1. Install `pidstat` once: `sudo apt install sysstat`.
2. Start the app on the Pi's screen. It opens maximised on a small screen. Connect, choose the
   Pi's output, tick AetherNR (NR2) and every stage, and let audio play throughout. Note the
   underruns on Final Output's statistics line.
3. Show each page in turn, wait a few seconds, then from another computer over SSH, so no
   terminal is drawing on the Pi's screen, run:

   ```sh
   pidstat -t -p "$(pidof ExpertSDR3Audio)" 10 1
   ```

   It averages ten seconds. From its `Average:` lines write down two figures: the `%CPU` of the
   thread whose TID is the program's PID (the window's thread, listed as `|__ExpertSDR3Audio`),
   and the `%CPU` on the first line (TID `-`), which is the whole program. 100 % is one core.
4. Do the same on the EQ page while dragging a handle the whole time, as each step of a drag
   restyles the band readings.
5. At the end, on Final Output, write down the DSP figure from its statistics line, and whether
   the underruns rose.

| Page | Window thread %CPU | Whole program %CPU |
|---|---|---|
| Radio | | |
| AetherNR | | |
| Gate | | |
| EQ | | |
| EQ, dragging a handle | | |
| Compressor | | |
| Tube | | |
| Exciter | | |
| Final Output | | |

Underruns that rise while a page is on screen, or a window thread near 100 %, mean the Pi
cannot keep up with that page. The waveform on Final Output redraws at 30 Hz on Linux on ARM,
half upstream's rate, for this reason.

On the touchscreen:

- Each rail row opens its page with a tap, and each box ticks with a tap.
- A knob on each page turns with a finger dragged up or down, and an EQ handle drags.
- The gear menu, its submenus, REC, PLAY, BYPASS and the AetherNR tabs work with a tap.
- Every page is usable on the 800x480 screen. Final Output scrolls rather than cutting the
  scope off; note whether its scroll bar can be taken with a finger.
- With the custom frame, the touch checks in the frame section above.

## Regenerating the README screenshot

`docs/screenshot.png` is the window on its EQ page, with a saved profile, connected to the
test suite's simulated radio, which sends a tone, so the status chip is green and the analyser
shows the tone. `test_gui`'s `aConnectedWindowShowsTheRadioAndItsAudio` draws it offscreen at
720x750; it reaches no real radio and plays nothing. After a change to the look, from a build
with the tests:

```sh
cmake --build build && mkdir -p build/readme-shot
ESA_GUI_SHOT_DIR="$PWD/build/readme-shot" ctest --test-dir build -R test_gui --output-on-failure
cp build/readme-shot/readme.png docs/screenshot.png
```

The current one was drawn on a Mac. Look at it before committing: the text is drawn by each
platform's own font engine, so a picture from Linux or Windows differs a little.

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
- **Every package carries the licences** of what it is built from or ships with, from one list
  in `CMakeLists.txt` (Licences): inside the Mac app, beside the program on Windows, and in
  `/usr/share/doc/expertsdr3-audio/Licenses` on Linux. `test_licences` checks that each is
  there. When a sync or a new dependency brings in another library, add its licence to that
  list and to `tests/CheckLicences.cmake`. The libraries the deploy tools copy in beside Qt
  (macdeployqt, windeployqt, linuxdeploy) have not been gone through one by one.
- **A double-click on an Exciter knob lands on AetherSDR's default,** 6 dB for Body drive and 30 %
  for either mix, where this app's defaults are 0 dB and 50 %. The page is AetherSDR's own. Our
  defaults stay because a value left at its default is not stored: changing one would change the
  sound of everyone who never moved that knob.

## Look goldens

`test_look` holds the approved look still. It draws seven pages from the settings in
`tests/look/<page>.ini`: Gate, EQ, Compressor, Tube, Exciter and Final Output at 720x750, and
AetherNR at 720x480. It draws them as `--screenshot` does, with three things held fixed
(below), and compares each with `tests/golden/<key>/<page>.png`. A page passes when every pixel
is within 3/255 of the golden on every channel. That is strict on purpose: a computer draws
its own set again pixel for pixel, a new colour for the title text alone changes only 0.3 % of
the window, and the theme's ground and its raised and nested cards differ by only 4 to 12 per
channel, so a wider tolerance would let a card lose its surface unnoticed.

Nothing in the pictures moves: there is no radio and no audio, and every run paints the check
boxes and arrows afresh. The three things held fixed: the numbers are written the US way, Final
Output is given the same device list on every computer, and AetherNR shows the NR4, DFNR and
NNR tabs available even in a build without them or on a virtual Mac. So the app's own
`--screenshot` can differ from the golden in those places.

The key names the environment that drew the set, because text comes out a little different on
each operating system release, processor and Qt version. The test prints it, for example
`macos-27-arm64-qt6.10` or `debian-12-x86_64-qt6.4`. Where there is no folder for the key, the
test compares nothing and ctest lists it as skipped. Sets exist for each CI job
(`macos-15-arm64-qt6.10`, `windows-server2025-x86_64-qt6.10`, `debian-12-x86_64-qt6.4` and
`debian-12-arm64-qt6.4`) and for `macos-27-arm64-qt6.10`. When a runner image moves to a new
release its key changes, the test skips there, and its new set is committed from its artifact
as below. Plain ctest shows only "Skipped"; run
`ctest --test-dir build -R test_look -V` to see the key and the message. On CI the key is also
the name of the folder inside the `look-*` artifact.

When a page fails, the message names a folder holding the render and a diff: the golden, the
render, and the pixels past the tolerance in red, side by side. Set `ESA_LOOK_OUT=<folder>` to
choose the folder. After an operating system, font or Qt update the text may come out a little
different: if the diff shows only the edges of the letters in red, draw the set again as below.

**After the user approves a change to the look,** draw this computer's set again:

```sh
ESA_UPDATE_GOLDENS=1 ctest --test-dir build -R test_look -V
```

It writes `tests/golden/<key>/*.png` and compares nothing: each page says which file it wrote
(`-V` shows those lines), and ctest lists the test as skipped. Look at every picture before
committing them. Run ctest again; it now passes. Never set `ESA_UPDATE_GOLDENS` in CI; the test
fails if you do. The other environments' sets are now out of date too: replace them from CI as
below.

**To add or replace a CI environment's set:** every CI run that reaches `test_look` uploads the
renders as an artifact, kept for a week: `look-macos-arm64`, `look-windows-x64`,
`look-linux-x86_64` and `look-linux-arm64`. Each holds a folder named for its key, ready to
commit:

```sh
gh run download <run id> -n look-linux-x86_64 -D look
```

Look at every picture in `look/<key>/`, copy that folder into `tests/golden/`, and commit it.
From the next run on, that job compares against it.
