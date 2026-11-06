# ExpertSDR3 Audio

Plays ExpertSDR3's receive audio through the
[AetherSDR](https://github.com/aethersdr/AetherSDR) **AetherRX** chain, so you can use its
noise reduction with any radio ExpertSDR3 supports.

```
ExpertSDR3 ──TCI──▶ NR (one method) ──▶ Gate ▶ EQ ▶ Comp ▶ Tube ▶ AetherVoice ──▶ your sound device
```

It connects over [TCI](https://github.com/ExpertSDR3/TCI), lets you pick any audio output
on the machine, and mutes ExpertSDR3's own audio while it plays so you don't hear both.

![The main window, connected to a radio](docs/screenshot.png)

## Noise reduction methods

Pick one on the **NR** tab or with `--nr`. All are AetherSDR's own code, unmodified.

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

NR is bypassed automatically on CW and digital modes, because those signals are not speech
and a speech model treats a steady carrier as noise. You can turn this off.

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

## Use

1. In ExpertSDR3, enable TCI in its options, and start the radio.
2. Start the app and press **Connect**. If nothing answers on the port shown, the usual TCI
   ports (40001, 50001, 60001) are tried automatically.
3. Choose your **Device**, and a method on the **NR** tab.

To run without a display, for example on a Raspberry Pi:
```sh
ExpertSDR3Audio --list-devices
ExpertSDR3Audio --headless --host 192.168.1.20 --port 40001 --device "USB" --nr nr2
ExpertSDR3Audio --list-params   # everything --set can change
```

## How muting works

With **Mute ExpertSDR3's own audio** on (the default), the app sends `MUTE` only once audio
is really playing, and undoes it when you disconnect or quit.

- A radio you had already muted stays muted afterwards.
- Unmuting in ExpertSDR3 wins, and the app won't fight you. Moving ExpertSDR3's volume
  control counts too, because that also unmutes it.
- If the output device fails or the stream stops, ExpertSDR3 is unmuted at once, so you
  are never left with silence from both.
- If the app crashes, ExpertSDR3 stays muted until the next time the app connects, which
  then restores it. If you never run it again, unmute in ExpertSDR3.

[docs/TCI-NOTES.md](docs/TCI-NOTES.md) has the protocol behaviour behind this.

## Platform status

| Platform | Status |
|---|---|
| macOS (Apple Silicon) | Tested live against ExpertSDR3 2.1 on a ColibriDDC, including the Developer ID signed build from its DMG |
| Windows | **Not yet built or run.** The pipeline is written but has not run |
| Linux x86-64 | **Not yet built or run.** The pipeline is written but has not run |
| Raspberry Pi (64-bit) | **Not yet built or run.** The pipeline is written but has not run. It builds on arm64 Debian, not on a real Pi |

**Raspberry Pi.** Start with `nr2`. The neural methods need about 28% of an Apple Silicon
core here, and a Pi core is several times slower, so they may not keep up; the app warns if
so. Note that `--rate 24000` adds about 150 ms of delay from resampling and does not make
RN2 any cheaper, because RNNoise always runs at 48 kHz internally.

**Transceivers.** Testing so far is on a receive-only radio. While transmitting, the chain
is bypassed and restarts cleanly afterwards, but that has only been tested against a
simulated server.

## Adding a noise reduction method

Each method is an adapter plus one table entry, both in
[src/dsp/NrRegistry.cpp](src/dsp/NrRegistry.cpp):

```cpp
class MyProcessor final : public NrProcessor { /* process, reset, setParam */ };

methods.append({QStringLiteral("my"), QStringLiteral("My Method"), QStringLiteral("What it does."),
    {{QStringLiteral("my.amount"), QStringLiteral("Amount"), 0.0, 1.0, 0.5, 2}},
    [](int rate) { return makeIfValid<MyProcessor>(rate); }});
```

The GUI controls, saved settings, `--nr`, `--set` and the test suite all pick it up from
that entry, and `test_rx_chain` will hold it to the same bar as the others.

## Updating the AetherRX code

`vendor/aethersdr` is an unmodified subset of AetherSDR, at the commit in
`vendor/aethersdr/UPSTREAM_COMMIT`. To take upstream DSP improvements:

```sh
scripts/sync-aether.sh /path/to/AetherSDR
(cd vendor/aethersdr && bash scripts/setup/setup-deepfilter.sh)
cmake --build build && ctest --test-dir build
```

## Tests

```sh
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure
```

These need no radio or sound device. `test_engine_e2e` runs the real engine against a
simulated server built from measurements of the real one.

## License

GPL-3.0, inherited from AetherSDR. See [LICENSE](LICENSE), and
[vendor/aethersdr/THIRD_PARTY_LICENSES](vendor/aethersdr/THIRD_PARTY_LICENSES) for the
bundled components. If you distribute binaries, you must also offer the source.
