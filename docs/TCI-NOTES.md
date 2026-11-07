# ExpertSDR3 TCI: measured behaviour

The [TCI 2.1 spec](https://github.com/ExpertSDR3/TCI/blob/main/TCI%20Protocol%202.1.pdf)
leaves some things out. These were measured against ExpertSDR3 (`protocol:ExpertSDR3,2.1`)
on a ColibriDDC. `tests/MockExpert.cpp` reproduces them, so tests fail if the code stops
accounting for them.

## Muting does not affect the TCI stream

The RX audio stream level did not change under any local output control:

| Command | Stream level |
|---|---|
| Baseline | -47.1 dBFS |
| `VOLUME:-40` | -47.1 dBFS |
| `RX_VOLUME:0,0,-40` | -46.8 dBFS |
| `RX_MUTE:0,true` | -47.0 dBFS |
| `MUTE:false`, then `MUTE:true` | -47.0, -47.2 dBFS |

This is what makes the app possible: ExpertSDR3's speakers can be silenced without
touching the audio the app processes.

## VOLUME clears MUTE

Not in the spec. Any `VOLUME` set makes the server report `mute:false`, even from a
muted state. So:

- This app must never send `VOLUME`, or it would undo its own mute.
- Moving ExpertSDR3's volume control while the app holds the mute unmutes it. The app
  treats that as the operator's decision and leaves it alone until the option is re-armed.
- Scripts restoring both must set volume first, then mute.

## `length` and `AUDIO_STREAM_SAMPLES` count real samples

The spec's table gives the default at 48 kHz as 2048 samples. The real block has
`length=4096`, 2 channels and 16384 payload bytes: 2048 frames, 42.7 ms.

After `AUDIO_STREAM_SAMPLES:512` the block has `length=512`: 256 frames in stereo, 5.3 ms.
That is below the spec's own 10 ms recommendation, despite being its suggested minimum.
The app asks for 1024 (512 frames, 10.7 ms), and always derives frames as
`length / channels`.

## Radios cannot be discovered, only found

The spec defines no discovery, and ExpertSDR3 2.1 advertises nothing over mDNS, so `TciDiscovery`
tries each host and port and waits for the `protocol:` and `ready;` of the init burst. A server
sends that to anyone who connects, so a probe never has to send a command.

## Other details

- Sets that change nothing are still echoed, so a set can always be confirmed.
- `start;` is part of the init burst and arrives before `ready;`. A later one means the
  operator restarted the DSP, and audio has to be requested again.
- `AUDIO_*` are per-client, so requesting a block size or rate does not disturb other
  TCI clients.
- The Line Out stream (`LINE_OUT_START`) arrives as type 4, 48 kHz float32. Not used here.
