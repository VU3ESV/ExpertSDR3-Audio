# ExpertSDR3 Audio

Plays ExpertSDR3's receive audio through the
[AetherSDR](https://github.com/aethersdr/AetherSDR) **AetherRX** chain, so you can use its
noise reduction and voice stages with any radio ExpertSDR3 supports. The window is
AetherRX's own, taken from AetherSDR.

```
ExpertSDR3 ──TCI──▶ AetherNR ▶ Gate ▶ EQ ▶ Compressor ▶ Tube ▶ Exciter ▶ Final Output ──▶ your sound device
```

It connects over [TCI](https://github.com/ExpertSDR3/TCI), plays through any audio output on
the computer, and mutes ExpertSDR3's own audio while it plays so you don't hear both. It can
also record what the chain makes of the radio's audio, and play it back.

![The main window on its EQ page, with a radio connected](docs/screenshot.png)

## Install

Download the latest build from [Releases](https://github.com/VU3ESV/ExpertSDR3-Audio/releases):

| Platform | File | Install |
|---|---|---|
| macOS 13+ (Apple Silicon) | `-macos-arm64.dmg` | Drag to Applications. Signed and notarized. |
| Windows 10+ (x64) | `-windows-x64-setup.exe` or `-portable.zip` | Not code-signed yet, so SmartScreen warns on first run: choose **More info**, then **Run anyway**. |
| Raspberry Pi (64-bit OS) | `-raspberrypi-arm64.deb` | `sudo apt install ./<file>.deb` |
| Debian or Ubuntu (x86-64) | `-linux-x86_64.deb` | `sudo apt install ./<file>.deb` |
| Other Linux (x86-64) | `-linux-x86_64.AppImage` | `chmod +x`, then run. Needs a desktop Linux with glibc 2.36 or newer. |

32-bit Raspberry Pi OS is not supported. [docs/RELEASING.md](docs/RELEASING.md) covers how
releases are made, signed and verified.

## Getting started

1. In ExpertSDR3, enable TCI in its options, and start the radio.
2. Start the app. The first time, it opens on the **Radio** page. Press **Connect**. If
   nothing answers on the port shown, the usual TCI ports (40001, 50001, 60001) are tried
   automatically.
3. On **Final Output**, choose your **Device**.
4. On a new install everything is switched off, so at first you hear the radio as it is. Tick
   **AetherNR** on the left to turn noise reduction on (NR2 the first time), and tick the
   stages you want.

## The window

**The rail** down the left lists the pages: Radio, AetherNR, the five stages (Gate, EQ,
Compressor, Tube and Exciter) and Final Output. Click a row to open its page.

- The box beside AetherNR and each stage turns it on or off.
- The stages run in the order the rail shows. Drag a stage by its grip (the dots at its left)
  to move it. Right-click a stage for **Move up**, **Move down**, **Reset stage order** and
  **Exact values...**. From the keyboard, Alt+Up and Alt+Down move the stage whose row has
  the focus.

At the foot of the rail:

- **REC** and **PLAY** record and play back, as described below.
- **BYPASS** lets you hear the radio unprocessed: AetherNR and every stage are passed by until
  you click it again. Nothing is changed or saved, and the boxes on the rail keep their ticks.
- **The gear** opens the menu: Profiles, Find Radios..., Exact values..., Reset stage order,
  Recordings, Window frame, About, Project Website, and Quit on Windows and Linux. On a Mac the
  menu bar also has Radio, Profiles and Help, and Quit is in the app menu.

**The header** across the top holds:

- the **profile list**, with **Modified** beside it when the sound differs from the saved
  profile;
- **● REC 01:23** while recording, and **▶ 00:12 / 01:23** while playing back;
- the **status chip**. When all is well it is green and shows the radio's frequency, mode and
  receiver. Otherwise it says what needs attention: Not connected in grey; in amber
  Connecting…, Disconnecting… (while ExpertSDR3 takes its mute back after you press
  Disconnect), No audio from radio, NR preparing…, NR bypassed or Recording dropped audio; in
  red a failing output, mute or NR, a computer that cannot keep up, or a recording that
  stopped. Hover over it for every message, and click it to open the page concerned, if there
  is one (a recording has none);
- **⋯**, which opens the stage's exact-values window (also **Exact values...** in the gear
  menu and a stage's right-click menu): every setting of that stage as an exact number you can
  type, including the few its page does not show (the Gate's attack and exact look-ahead, the
  EQ's output, the Compressor's drive and phase rotator, the Tube's attack). An amber dot on
  **⋯** means one of those is away from its default. It is greyed on Radio, AetherNR and Final
  Output, which have no such window.

## The pages

- **Radio.** Host, Port, Receiver, **Find Radios...** and **Connect**, with a line saying what
  the radio is doing. The **From radio** meter shows the level arriving from ExpertSDR3. **Mute
  ExpertSDR3's own audio while this app is playing** is here too (see
  [How muting works](#how-muting-works)).
- **AetherNR.** One tab per method. Click a tab to run that method, and click the lit tab
  again to turn NR off. Below are the method's settings and its **Reset**, the **Bypass NR
  automatically on CW and digital modes** box, and a strip with the method running and its
  **NR GAIN** trace, which shows how hard it is working.
- **Gate.** A gate or expander (the **Mode** buttons) that quietens the noise between
  overs. The plot shows the level over time (**Level**) or the gain curve (**Curve**). Six
  knobs: Thresh, Return, Hold, Release, Floor and Ratio. **Peek** looks a few milliseconds
  ahead, so the gate is open before a sound starts.
- **EQ.** Ten bands on a curve. Drag a handle to change its frequency and gain, or Shift-drag
  it to change its width (Q). Click a band's icon above the curve to change its type; a
  handle's right-click menu sets its type, and the slope of a high- or low-pass. The analyser
  behind the curve shows the audio, and the **OUT** meter its level. **Reset** puts every band
  back to its default.
- **Compressor.** The threshold fader, the gain curve, and Ratio, Attack, Release and Knee.
  **LIMIT** and **Ceiling** add a limiter at the end. Drag the handle on the **Out** meter for
  make-up gain.
- **Tube.** Valve-style saturation: Model A, B or C, the **Dry/Wet** mix, and Drive, Tone,
  Bias, Envelope, Release and Output.
- **Exciter**, shown as AetherVoice. Adds harmonics for presence: **Even** or **Odd**, then
  Drive, Tune and Mix for the low end (**Body**) and Tune, Air and Mix for the top (**Clarity**).
- **Final Output.** Volume, **MUTE** and **BOOST** (below), the level meter (peak, RMS and
  crest factor), the output **Device** with **Refresh**, the **Buffer**, a line of statistics
  (DSP load, clock correction, underruns, lost blocks, chain rate), and a waveform of what is
  playing. Click the waveform to pause it. On a short screen this page scrolls.

Every knob turns as you drag up or down (hold Shift for fine steps) or with the mouse wheel,
and a double-click resets it to its default. On the Exciter that is AetherSDR's own default,
which for Body drive (6 dB) and the two mixes (30 %) differs from this app's (0 dB, 50 %). To
type an exact value, click the value under the knob, type, and press Return.

## Volume, MUTE and BOOST

These are on Final Output. Volume and BOOST belong to this computer: they are saved with its
settings, not in profiles. MUTE is never saved.

- **Volume** sets how loud this app plays, from -60 to +30 dB, with 0 dB at the top of the
  knob. Double-click it for 0 dB.
- **MUTE** silences this app only, at once, until you click it again or restart the app.
  ExpertSDR3's own audio is not affected: that is the Radio page's **Mute ExpertSDR3's own
  audio** setting. The meter and the waveform keep showing what would play.
- **BOOST** lifts quiet audio, up to about twice as loud, without ever going past full scale.

## Recording and playback

**REC** records what the stages make of the radio's audio, before Volume, BOOST and MUTE, so a
recording does not depend on how loud you were listening. It works while connected to a radio.
Click it again to stop. While BYPASS is on, the recording holds the unprocessed audio.

- Files are WAV, 32-bit float, stereo, at the chain's rate (48 kHz, or 24 kHz with
  `--rate 24000`): about 1.4 GB an hour.
- Each is named from the time in UTC, the frequency and the mode, for example
  `2026-11-07_141503Z_14.200MHz_USB.wav`. A long recording carries on in a `_part2` file
  after about 2 hours 40 minutes at 48 kHz (3.5 GiB), and so does one where the radio
  changes its audio rate.
- They go to `ExpertSDR3 Audio/Recordings` in your Documents folder. **Recordings > Folder...**
  in the gear menu chooses another folder, and **Show in Finder** (Explorer, Files) opens it,
  with the last recording selected on a Mac. On a Mac, macOS may ask once for access to
  Documents the first time you record there.

**PLAY** plays the last recording made since the app started, through the same output, Volume,
BOOST and MUTE, in place of the radio's audio, which goes on being processed. It is greyed
until you have recorded something. Click it again to stop. It plays without a radio too, and
then leaves ExpertSDR3 alone.

Neither REC nor PLAY sends anything to the radio. To record without a window, see `--record`
below.

## Window frame

**Window frame** in the gear menu chooses the window's edge. **System**, the default
everywhere, is the frame and title bar your computer gives every window. **Custom** is
AetherRX's own look:

- On a Mac the window's dark ground runs right to the top, under the usual close, minimise and
  zoom buttons. This needs a build with Qt 6.9 or later, which the released app has.
- On Windows and Linux the system's frame goes. Minimise and close are at the right of the
  header. Drag the header to move the window, double-click it to maximise, and drag an edge
  to resize.

The change applies at once and is remembered.

## Noise reduction methods

Pick one from the **AetherNR** page's tabs or with `--nr`. All are AetherSDR's own code, unmodified.

| Method | What it is | SNR gain | Noise in pauses | Delay | CPU |
|---|---|---|---|---|---|
| `dfnr` DeepFilterNet3 | Neural | +10 dB | -32 dB | 41 ms | 28% |
| `rn2` RNNoise | Neural | +5 dB | -36 dB | 31 ms | 27% |
| `nnr` WDSP NNR | Neural | +5 dB | -29 dB | 51 ms | 29% |
| `nr4` libspecbleach | Spectral | +5 dB | -6 dB (at the 10 dB default) | 40 ms | 20% |
| `mnr` Apple vDSP | Spectral, macOS only | +5 dB | -24 dB | 21 ms | 1% |
| `nr2` WDSP-derived | Spectral | +2 dB | -19 dB | 43 ms | 2-7% |

SNR, pause noise and delay are from `test_rx_chain`: synthetic speech at +6 dB SNR in white
noise, at 48 kHz with default settings. They show the methods work and roughly how they
differ, but are not a listening test, and real band noise behaves differently. CPU is the
share of one Apple Silicon core, in stereo, on a live stream. Delay excludes the output
buffer (60 ms by default).

A method this computer cannot run has a greyed tab, and its tooltip says why. MNR needs
macOS. BNR, NVIDIA's method in AetherSDR, is not part of this app.

DFNR is not offered when macOS runs in a virtual machine: its library always uses an Apple
Silicon coprocessor that VMs do not expose, so it would crash.

NR is bypassed automatically on CW and digital modes, because those signals are not speech
and a speech model treats a steady carrier as noise. You can turn this off.

## Finding radios

**Find Radios...**, on the Radio page and in the gear menu, looks for computers on your
network running ExpertSDR3 with TCI enabled, and connects to the one you pick.
`ExpertSDR3Audio --discover` does the same from a terminal.

TCI has no way to announce itself, so this tries the usual TCI ports (40001, 50001, 60001, plus
whatever is in the Port box) on each address, which takes about 10 seconds. It only reads each
server's greeting and sends no commands, so it cannot disturb a radio in use. It stays on private
addresses, skips VPN links, and checks only the 254 addresses around this computer on each network
adapter. For anything further away, type the host and port on the Radio page.

On a Mac, the first scan asks for permission to use the local network. If you allow it, scan again.

## Profiles

A profile saves everything on the AetherNR and stage pages: the noise reduction method and its
settings, NR's automatic bypass, every stage with its on/off box, and the stage order. It
leaves out the radio, output device, Volume, BOOST and buffer, so it can move to another
computer without bringing that computer's hardware along.

Pick one from the profile list in the header, or **Factory settings** to start over.
**Modified** means the sound has changed since it was saved. The gear menu's **Profiles** has
**Save**, **Save As...**, **Import...**, **Export...**, **Export All...** and **Delete** (on a
Mac, so does the Profiles menu). **Export...** writes a `.json` file to share or back up, and
**Export All...** writes every profile to one file. **Import...** reads either kind, renames
anything whose name is taken rather than replacing it, and leaves the current sound alone until
you choose one.

A profile that uses a method this computer lacks, such as MNR away from a Mac, plays with NR off,
and keeps the setting in case it travels back.

## How muting works

With **Mute ExpertSDR3's own audio while this app is playing** on (the default, on the Radio
page), the app sends `MUTE` only once audio is really playing, and undoes it when you
disconnect or quit.

- A radio you had already muted stays muted afterwards.
- Unmuting in ExpertSDR3 wins, and the app won't fight you. Moving ExpertSDR3's volume
  control counts too, because that also unmutes it.
- If the output device fails or the stream stops, ExpertSDR3 is unmuted at once, so you
  are never left with silence from both.
- If the app crashes, ExpertSDR3 stays muted until the next time the app connects, which
  then restores it. If you never run it again, unmute in ExpertSDR3.

Final Output's **MUTE** is separate: it silences this app and leaves ExpertSDR3 as it is.
[docs/TCI-NOTES.md](docs/TCI-NOTES.md) has the protocol behaviour behind this.

## Command line

On a Mac the program is `/Applications/ExpertSDR3Audio.app/Contents/MacOS/ExpertSDR3Audio`.
On Windows it is `ExpertSDR3Audio.exe` in its install folder, and on Linux the `.deb` puts
`ExpertSDR3Audio` on the path.

To run without a display, for example on a Raspberry Pi:
```sh
ExpertSDR3Audio --list-devices
ExpertSDR3Audio --list-params   # NR methods, and everything --set can change
# These connect, so they need ExpertSDR3 running at that address:
ExpertSDR3Audio --headless --host 192.168.1.20 --port 40001 --device "USB" --nr nr2
ExpertSDR3Audio --headless --host 192.168.1.20 --device "USB" --record ~/Recordings --duration 3600
```

Profiles from a terminal (the last line connects, so it needs a radio too):
```sh
ExpertSDR3Audio --list-profiles
ExpertSDR3Audio --import-profile shared.json
ExpertSDR3Audio --export-profile "Weak SSB" > weak-ssb.json
ExpertSDR3Audio --headless --device "USB" --profile "Weak SSB"
```

Every option, as `ExpertSDR3Audio --help` prints them. Qt writes the first three lines itself,
and their wording differs a little between Qt versions and systems:
```
  -h, --help                  Displays help on commandline options.
  --help-all                  Displays help, including generic Qt options.
  -v, --version               Displays version information.
  --headless                  Run without a window.
  --list-devices              List audio output devices and exit.
  --list-params               List NR methods and chain parameters and exit.
  --discover                  Look for TCI radios on the local network and
                              exit.
  --list-profiles             List saved audio chain profiles and exit.
  --profile <name>            Use this saved audio chain profile. --nr and
                              --set still apply on top.
  --import-profile <file>     Import profiles from a file and exit.
  --export-profile <name>     Print a saved profile as JSON and exit. Redirect
                              it to a file.
  --host <host>               TCI server host.
  --port <port>               TCI server port.
  --rx <n>                    Receiver number.
  --device <device>           Output device: key, name or part of a name.
                              "null" discards audio.
  --nr <id>                   Noise-reduction method, or "off". See
                              --list-params.
  --set <key=value>           Set a chain parameter; repeatable. See
                              --list-params.
  --gain <dB>                 Output gain in dB.
  --buffer <ms>               Output buffer in ms.
  --device-rate <hz>          Force the output device's sample rate, converting
                              to it if needed.
  --rate <hz>                 Processing rate: 48000 (default) or 24000 for
                              low-power machines.
  --no-mute                   Leave ExpertSDR3's own audio alone.
  --capture <base>            Headless: record the radio's audio to
                              <base>.before.wav and the chain's output to
                              <base>.after.wav (or the next free names; a rate
                              change continues in _part2).
  --record <folder|file.wav>  Headless: record the chain's output from the
                              first audio until exit or --duration, into this
                              folder, named from the time, frequency and mode,
                              or into this .wav file. An existing file is never
                              replaced.
  --duration <s>              Headless: stop after this many seconds.
  --settings <file>           Use this ini file instead of the normal settings
                              store.
  --connect                   Connect immediately on startup.
  --screenshot <file>         Save a PNG of the window after 2 s and exit
                              (development).
  --page <id>                 With --screenshot: the page to show (radio, nr,
                              gate, eq, comp, tube, exciter, output).
  --size <WxH>                With --screenshot: the window's size, e.g.
                              724x752, instead of the saved one.
```

`--gain` is the Volume knob, and is held to its -60 to +30 dB. `--device null` plays into
nothing, and Final Output's Device list then shows **No output (null device)**. `--headless`
connects at once; the window connects only when you press Connect, or when started with
`--connect`.

The window keeps what you give it on the command line, as if you had set it in the window:
`--device`, `--gain` and `--no-mute` among the rest are still there the next time it starts.
So after trying `--device null`, choose your device on Final Output again, or you will hear
nothing, and after `--no-mute`, tick the Radio page's mute box again if you want it.

## Platform status

| Platform | Status |
|---|---|
| macOS (Apple Silicon) | The audio path tested live against ExpertSDR3 2.1 on a ColibriDDC, including the Developer ID signed build from its DMG; the new AetherRX window not yet. Built and tested in CI |
| Windows | Built, tested and run from both the installer and the zip in CI. **Not yet tried on a real PC, with a radio or sound device** |
| Linux x86-64 | Built, tested, and run from the `.deb` and AppImage on Debian 12 and 13 and Ubuntu 24.04 and 26.04 in CI. **Not yet tried with a radio or sound device** |
| Raspberry Pi (64-bit) | The same, on arm64. **Not yet tried on a real Pi** |

**Raspberry Pi.** Install the `.deb`, which also suits a headless Pi OS Lite; the AppImage needs
the desktop version's libraries. Start with `nr2`. The neural methods need about 28% of an Apple Silicon
core here, and a Pi core is several times slower, so they may not keep up; the app warns if
so. Note that `--rate 24000` adds about 150 ms of delay from resampling and does not make
RN2 any cheaper, because RNNoise always runs at 48 kHz internally. On a small screen, such as
the 7-inch display, the window opens maximised.

**Transceivers.** Testing so far is on a receive-only radio. While transmitting, the chain
is bypassed and restarts cleanly afterwards, but that has only been tested against a
simulated server.

## Build

You need CMake 3.25+, a C++20 compiler, Qt 6.4+ with WebSockets, FFTW3 and PortAudio.

**macOS**
```sh
brew install qt fftw portaudio ninja pkg-config
(cd vendor/aethersdr && bash scripts/setup/setup-deepfilter.sh)
cmake -S . -B build -G Ninja && cmake --build build
open build/ExpertSDR3Audio.app
```

**Linux and Raspberry Pi OS (64-bit)**
```sh
sudo apt install build-essential cmake ninja-build pkg-config qt6-base-dev \
  libqt6websockets6-dev libfftw3-dev portaudio19-dev libgl1-mesa-dev
(cd vendor/aethersdr && bash scripts/setup/setup-deepfilter.sh)
cmake -S . -B build -G Ninja && cmake --build build
./build/ExpertSDR3Audio
```

**Windows** (Visual Studio with the clang-cl component, and Qt 6 from the Qt installer)
```powershell
vcpkg install fftw3:x64-windows
cd vendor\aethersdr; .\scripts\setup\setup-deepfilter.ps1; cd ..\..
cmake -S . -B build -T ClangCL -DCMAKE_TOOLCHAIN_FILE=<vcpkg>\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release --parallel
```
PortAudio is built from source automatically on Windows.

The DeepFilterNet step downloads a checksum-verified prebuilt library, or builds it with
Cargo if there is none for your platform. Skip it to build without DFNR. Other options:
`-DENABLE_NNR=OFF`, `-DENABLE_NR4=OFF`, `-DBUILD_TESTS=OFF`.

## Adding a noise reduction method

Each method is an adapter plus one table entry, both in
[src/dsp/NrRegistry.cpp](src/dsp/NrRegistry.cpp):

```cpp
class MyProcessor final : public NrProcessor { /* process, reset, setParam */ };

methods.append({QStringLiteral("my"), QStringLiteral("My Method"), QStringLiteral("What it does."),
    {{QStringLiteral("my.amount"), QStringLiteral("Amount"), 0.0, 1.0, 0.5, 2}},
    [](int rate) { return makeIfValid<MyProcessor>(rate); }});
```

The AetherNR page (a tab of its own), saved settings, `--nr`, `--set` and the test suite all
pick it up from that entry, and `test_rx_chain` will hold it to the same bar as the others.

## Updating the AetherRX code

`vendor/aethersdr` is an unmodified subset of AetherSDR, at the commit in
`vendor/aethersdr/UPSTREAM_COMMIT`. `scripts/sync-aether.sh` takes a newer one; the few
files that had to change live in `src/gui/aether/fork/`. A sync is its own pull request:
[docs/RELEASING.md](docs/RELEASING.md#updating-aethersdr) says how to make one and what to
check after it.

## Tests

```sh
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure
```

These need no radio or sound device, and are safe to run on the computer your radio runs on:
every test runs in a sandbox where no real audio output opens and ExpertSDR3's TCI ports
(40001, 50001, 60001) are refused. `test_engine_e2e` runs the real engine against a
simulated server built from measurements of the real one. Its settings tests skip the
native store (a Mac's preferences, the Windows registry), which belongs to your user
account, unless `ESA_TEST_NATIVE_SETTINGS=1` is set, as CI sets it.

`test_look` draws seven pages and compares each with its approved picture in `tests/golden/`,
drawn earlier on the same kind of computer. Every pixel must be within 3/255 of the picture on
every channel, so a change to how a page looks fails there, down to a card losing its shade.
It compares only where `tests/golden` has a set for your computer, today only macOS 27 on
Apple Silicon with Qt 6.10; elsewhere it compares nothing and ctest lists it as skipped.
[docs/RELEASING.md](docs/RELEASING.md#look-goldens) says how to draw a set.

## Credits and licence

GPL-3.0, inherited from AetherSDR. See [LICENSE](LICENSE). If you distribute binaries, you
must also offer the source.

- **AetherSDR** (GPL-3.0): the AetherRX chain and its noise reduction, and the window's look
  and widgets: the rail, the stage pages, the knobs, meters, curves and waveform, and the
  theme. They are AetherSDR's own code, in `vendor/aethersdr`. The few pieces adapted to build
  here are listed, with their source and exact changes, in
  [src/gui/aether/fork/README.md](src/gui/aether/fork/README.md).
- **The Inter typeface** (SIL Open Font License 1.1,
  [vendor/aethersdr/third_party/inter/LICENSE.txt](vendor/aethersdr/third_party/inter/LICENSE.txt)),
  which the window is drawn in. It is built into the app.
- **WDSP** by NR0V (GPL-2.0+), **RNNoise** (BSD-3-Clause), **DeepFilterNet** (MIT or
  Apache-2.0), **libspecbleach** (LGPL-2.1+) and **r8brain** (MIT), bundled as AetherSDR
  bundles them. See
  [vendor/aethersdr/THIRD_PARTY_LICENSES](vendor/aethersdr/THIRD_PARTY_LICENSES) and the
  licence in each folder of `vendor/aethersdr/third_party`.
- **FFTW** (GPL-2.0+), **PortAudio** (MIT) and **Qt** (LGPL-3.0).

Every package carries the text of these licences, each in a file named for whose it is
(`GPL-3.0.txt`, `Inter-OFL.txt`, `RNNoise-BSD-3-Clause.txt`, `Qt-LGPL-3.0.txt` and so on):

- **macOS:** inside the app, in `Contents/Resources/Licenses` (in Finder, right-click the app
  and choose Show Package Contents).
- **Windows:** `Licenses` beside `ExpertSDR3Audio.exe`, in the zip and where the installer
  puts the program. `LICENSE.txt` beside it is the GPL again.
- **Linux:** `/usr/share/doc/expertsdr3-audio/Licenses` from the `.deb`, and
  `usr/share/doc/expertsdr3-audio/Licenses` inside the AppImage (`--appimage-extract` unpacks
  it).

AetherSDR, AetherRX, AetherNR and AetherVoice are names from the AetherSDR project. This app
is a separate project that uses its code under the GPL.
