# Multi-radio plan

Status: proposal, 2026-11-07. Since 2026-11-22, phases 1, 3, 4 and 5 are being built for one model, the Kenwood TS-990S, on branch `feature/multi-radio`. Section 12 records how that differs from this plan. No radio was contacted while researching it.
Code references are to commit `5455f6f` and will drift. Source tags in square brackets, such as [I1], are listed in section 11.

## 1. Summary

**The rule.** A radio is supported only if its receive audio reaches the computer by one of three paths:

- **USB.** The radio itself shows up on the computer as a USB audio input.
- **TCI.** The radio's software speaks TCI and streams receive audio over it. This is what the app does today.
- **FlexAPI.** A FlexRadio, with audio streamed over the SmartSDR / Flex API.

Nothing else qualifies. Vendor network audio is excluded, and so are external interfaces such as a SignaLink. So Kenwood, Icom and Yaesu are reached only through USB. A USB radio still needs a control link, normally over the same cable, so the app can silence the radio's speaker and read its mode.

**The recommendation.** Build the USB path first. It is the only route to Kenwood, Icom and Yaesu under the rule, and on paper 29 models qualify. Order of work:

1. Put a radio-neutral seam under `Engine`, with TCI as the only backend and no change in behaviour.
2. Fix today's TCI path: misread two-argument mute reports, unmuting during a transmission, and output loss not being noticed.
3. USB audio capture without CAT. The operator turns the radio's AF down by hand.
4. CAT state reads (identity, frequency, mode, TX) through Hamlib, linked in-process. Hamlib itself writes a few settings when it opens the port (section 4.3).
5. Silence the radio's speaker by setting AF to zero, enabled one model at a time and only after a bench test.
6. Share the radio's CAT with other programs.
7. Widen TCI claims beyond ExpertSDR3.
8. FlexAPI, optional and last.

Phase 1 gives no user anything by itself. Every non-docs merge to `main` publishes a release, so phase 1 merges only together with phase 2, whose fixes help today's users. The first release with value for Kenwood, Icom and Yaesu owners is the one that ships phase 3. The risky step, writing to radios we cannot test, comes late and is gated per model. TCI users keep a working app throughout.

**The main risk is hardware.** The developer has one confirmed radio, an ExpertSDR3 ColibriDDC. Every USB and Flex fact in this plan comes from manuals, schematics or other projects' source code. None of it has been tested with this app. Phase 3 can ship without a radio tester because it writes nothing to the radio. From phase 4 on, testers are a gate: CAT, mute and Flex are switched on only for what a tester has verified.

**Rename (the user's decision, section 10, item 1).** Recommendation: use "AetherAudio" only if the AetherSDR maintainer agrees in public first. Otherwise pick a neutral name. Ranked by the collisions found so far, the candidates are RigHush, BandClear, then QuietRig. Rename the display name only, and keep every stored identifier unchanged so settings, profiles and owed mute records survive (section 9).

**Adopting Hamlib is also the user's decision** (section 10, item 0). This plan recommends it, on the conditions in section 4.3.

**Terms used below.**

- **AF**: the radio's audio (volume) level. **CAT** and **CI-V**: the serial command protocols of Kenwood/Yaesu and Icom.
- **Owed record** (also "debt"): a persisted note that this app muted a radio and must restore it. **Prior**: the state or level the radio had before this app muted it.
- **Mute phases**, from `MuteController`: Idle, Engaging (set sent, not yet confirmed), Holding (confirmed muted), Restoring, OperatorOverride (the operator changed it, so the app stops), Failed.
- **Switch** and **Level** speaker control: a mute flag (TCI, Flex) or an AF level written to 0 (USB radios).
- **`catVerified`** and **`muteVerified`**: per-model flags in the model table, set only from a bench record (sections 6 and 7).
- **Wave**: the order for recruiting testers, a judgement rather than a fact.
- **Standard** and **Enhanced**: the two virtual COM ports of a CP210x bridge. **Slice** and **DAX**: a Flex receiver and Flex's digital audio stream.

## 2. Who is supported

### 2.1 The rule, and how this plan applies it

Receive audio must arrive over USB audio built into the radio, over TCI, or over the FlexAPI. Consequences:

- A radio that has both USB and LAN is supported through USB only.
- The rule limits what is claimed, listed and supported. It is not a technical gate. The app shows no "generic sound card" radio: the operator picks a model from the supported table.
- The app can refuse a radio only when CAT identifies a model with no built-in USB audio. For example, an FTDX1200 with an external SCU-17 shows "USB Audio CODEC" and a CP2105, and only `ID;` reveals that it is an FTDX1200 [Y22]. In audio-only mode the app cannot tell an external interface from the radio's own codec. Even with CAT, `ID;` proves which radio is on the CAT port, not where the audio comes from.
- The device name "USB Audio CODEC" is a TI chip string, printed by TI with **two** spaces ("USB Audio&nbsp;&nbsp;CODEC"). TI warns that copying and pasting loses one, so name hints must match whitespace-tolerantly. The name proves nothing about which radio, or whether any radio, is attached [Y14][I42].

### 2.2 USB: models that qualify on paper

The 29 models in the 27 rows below have built-in USB audio according to the manufacturer's own documents, except where a row says **provisional**. CAT over the same cable is documented for all of them except the TH-D74 and TH-D75, which have no published Kenwood command reference. **None has been tested with this app.**

"Hamlib 4.7.2" gives the backend's self-declared status, and whether it declares AF get and set and a PTT (transmit-state) read [H7][H25]. "Evidence USB ignores AF" is the evidence that turning the radio's AF to zero leaves the USB stream alone. The mute design depends on it, and no row has a bench test. "Wave" is defined in section 1, "Terms used below".

**Icom**

| Model | Built-in USB audio and CAT on one cable | USB codec | Hamlib 4.7.2 | Evidence USB ignores AF | Wave | Sources |
|---|---|---|---|---|---|---|
| IC-7300 | Yes. [USB] carries CI-V and received audio | PCM2901E | STABLE, AF yes, PTT read yes | Strong. The manual says the same signal goes to [USB] and ACC pin 12, "Fixed AF detector". A reading of the schematic found USB on its own D/A channel, separate from the speaker amp; that reading was not re-checked (**unverified**) | 1 | [I1][I24] |
| IC-7300MK2 | Yes. Type-C, two virtual COM ports, USB audio in and out | unknown | BETA, AF yes, PTT read yes | Likely. "Fixed AF" wording for ACC, and USB has its own level setting | 1 | [I2][I3] |
| IC-7610 | Yes, on [USB 1] (Type B). [USB 2] is I/Q, not audio | PCM2901E | STABLE, AF yes, PTT read yes | Likely. "Fixed AF" wording for ACC, and USB has its own level setting | 1 | [I4][I31] |
| IC-705 | Yes. microUSB, USB audio and two serial ports | unknown | STABLE, AF yes, PTT read yes | Inference only. The manual has no "Fixed AF" text | 1 | [I6][I7] |
| IC-9700 | Yes. USB type B, two COM ports | PCM2901E | STABLE, AF yes, PTT read yes | Likely. ACC is "Fixed AF detector". Strong if a schematic reading holds: separate D/A channels (nets UAFA, UAFB) feed the USB codec apart from the speaker amps. The nets exist in the service manual, but the trace was not re-checked (**unverified**) | 1 | [I8][I25] |
| IC-7760 | Yes, on [USB B] of the controller. The RF deck's [USB] is I/Q | unknown | ALPHA, AF yes, PTT read yes | Likely ("Fixed AF" wording) | 2 | [I10][I11] |
| IC-7851, IC-7850 | Yes. One shared manual: [USB B], USB AF output, CI-V over USB | PCM2901E (**unverified**) | STABLE (one entry), AF yes, PTT read yes | Not researched (**unverified**) | 2 | [I22][I32] |
| IC-905 | Yes. Type-C, audio in and out, CI-V | unknown | STABLE, AF yes, PTT read yes | Inference only | 2 | [I12][I13] |
| IC-R8600 (receiver) | Yes, front mini-B and rear B, each with its own settings | 2 × PCM2901E | STABLE, AF yes, no PTT (receive-only) | Strong. Output "fixed, regardless of the volume control position", "also output from [USB]" | 2 | [I14][I30] |
| IC-7100 | Yes. Two COM ports, USB1 for CI-V | PCM2901E (service manual) | STABLE, AF yes, PTT read yes | Strong. Advanced Manual, USB Audio SQL: "The same audio signals are output from the [USB] connector and the [ACC] socket", and "The received audio output level cannot be adjusted with the [AF] control" | 2 | [I26] |
| IC-7200 | Yes. Icom: the [USB] jack is for "transceiver modulation input, the transceiver operation with a PC, and the received audio sent to the PC" (Advanced Manual p. 1-15) | PCM2901 (**unverified**) | STABLE, AF yes, PTT read yes | ACC pin is fixed. USB is not stated | 2 | [I27][I33] |
| IC-7410 | Yes | PCM2901E (**unverified**) | BETA, AF yes, PTT read yes | Strong. "The received audio output level cannot be adjusted with the [AF] control" | 2 | [I28][I33] |
| IC-9100 | Yes. Two COM ports, USB1 for CI-V | PCM2901E (service manual) | STABLE, AF yes, PTT read yes | Strong. Set item 55, USB Audio SQL: "The same audio signals are sent from the [USB] connector and the [ACC] sockets", and "The received audio output level cannot be adjusted with the [AF] control" | 2 | [I29] |
| IC-7600 | Yes, on [USB] (B) | PCM2901E (**unverified**) | STABLE, AF yes, PTT read yes | Strong. "cannot be adjusted with the [AF] control" | 2 | [I23][I33] |

**Kenwood**

| Model | Built-in USB audio and CAT on one cable | USB codec | Hamlib 4.7.2 | Evidence USB ignores AF | Wave | Sources |
|---|---|---|---|---|---|---|
| TS-890S | Yes. USB audio, virtual COM (Standard) for PC commands | PCM2903C, with 2 × CP2102N | STABLE, AF yes, PTT read yes | Implied, not stated. ACC2 independence is stated | 1 | [K8][K22][K2] |
| TS-590SG | Yes | PCM2903B (seen on two radios) | STABLE, AF yes, PTT read yes | Likely. "The audio level for Digital mode operation is also independent of the AF control setting" | 1 | [K12][K13][K16][K4] |
| TS-990S | Yes. Set "2 channel, 16-bit" | unknown | STABLE, AF yes, **no PTT read** | Implied, not stated | 2 | [K9][K3] |
| TS-590S | Yes | unconfirmed | STABLE, AF yes, PTT read yes | Likely. The same note as the TS-590SG: "The audio level for Digital mode operation is also independent of the AF control setting" (instruction manual p. 10) | 2 | [K15][K5] |
| TH-D74 (handheld) | Yes. USB Audio Device Class 1.0, plus virtual COM | unknown | STABLE, **no AF** | Not established | 3 (audio only) | [K21] |
| TH-D75 (handheld) | Yes, output only: 48 kHz, 16-bit, mono | unknown | **not in Hamlib 4.7.2** | Against: USB "outputs the same audio sound as the speaker output" | 3 (audio only) | [K20] |

**Yaesu**

| Model | Built-in USB audio and CAT on one cable | USB codec | Hamlib 4.7.2 | Evidence USB ignores AF | Wave | Sources |
|---|---|---|---|---|---|---|
| FTDX101D, FTDX101MP | Yes. One shared manual and one shared service manual | PCM2903C, with CP2105 | STABLE (two entries), AF yes, PTT read yes | Strong. Schematic: codec L = M-AF and R = S-AF, the same nets as AF-OUT, "not affected" by the AF knobs | 1 | [Y1][Y2] |
| FT-991A | Yes | PCM2903B, with CP2105 | Covered by the FT-991 entry, which recognises ID 670 (newcat.c:60). STABLE, AF yes, PTT read yes | Strong. Block diagram: USB branches off before the AF volume, but passes through DATA OUTG | 1 | [Y4][Y6][H25] |
| FT-710 | Yes. Has its own USB OUT LEVEL menu | unknown. A third-party wiki says CP2105 plus a third device, an FT4222H | STABLE, AF yes, PTT read yes | Not stated | 1 | [Y8][Y18] |
| FTX-1 | Yes, Type-C on the field head | unknown | BETA, AF yes, PTT read yes | Not stated | 1 | [Y9] |
| FTDX10 | Yes for a USB codec: the menu REAR SELECT is "DATA / USB", "USB: Inputs from the USB jack", and the CAT EX table has REAR SELECT "0: DATA 1: USB". The FT-891, which has no codec, offers only DATA IN SELECT MIC/REAR. Receive audio over USB is not stated outright (**provisional**; owner to confirm) | unknown | STABLE, AF yes, PTT read yes | Owner reports only (likely) | 1 | [Y10][Y13][Y19] |
| FT-991 | Yes | PCM2903B, with CP2105 | STABLE, AF yes, PTT read yes | AF path not traced (**unverified**) | 2 | [Y7] |
| FTDX3000 | Yes for a USB codec: the menu has "USB: Inputs the transmission data from the USB Jack", and third-party ioreg shows "USB Audio CODEC" and a CP2105 under one hub. Receive audio over USB is not stated outright (**provisional**; owner to confirm) | unknown | STABLE, AF yes, PTT read yes | The LINE jack is unaffected by AF. The USB path is undocumented | 2 | [Y11][Y16] |

Notes on the USB table:

- PCM2901, PCM2903B and PCM2903C are 16-bit codecs with ADC rates up to 48 kHz. Where those chips were found, USB audio is at most 16-bit, 48 kHz. That is a chip limit, not a manual statement. Only the TH-D75 manual states a rate [I35][K18][Y14][K20].
- ID-52 is **unverified**. Its manual shows only Charge, Serialport and SD Card USB modes, while Icom's product page mentions "audio". Do not list it until an owner checks [I34].

### 2.3 TCI: servers that speak it

**Tested with this app: only ExpertSDR3 2.1 on a ColibriDDC** (README.md:159, docs/TCI-NOTES.md:3). Every other row says what the server's own source or manual states. None of these can be claimed as supported until the section 5.2 changes are made and the server has been tested.

The shipped README already claims more than this: "use its noise reduction with any radio ExpertSDR3 supports" (README.md:5). Correct that sentence now, in a docs-only PR. Changes to `*.md` and `docs/` publish no release (release.yml:85-95).

| Server software | Radios | Streams RX audio over TCI | Mute behaviour that matters here | Tested here | Sources |
|---|---|---|---|---|---|
| ExpertSDR3 TCI 2.1 | ColibriDDC | Yes | Measured: MUTE does not change the stream. VOLUME clears MUTE | **Yes** | docs/TCI-NOTES.md |
| ExpertSDR3 (same program) | SunSDR2 DX, PRO, QRP, MB1, ColibriNANO | Yes, by specification | Not measured on these radios | No | [T1][T2][T3] |
| ExpertSDR2 | SunSDR2 family, MB1, Colibri | Yes, from 1.3.0 | Uses the TCI 1.8-or-earlier header, which the app rejects safely today | No | [T4][T5] |
| Thetis (ramdor) 2.10.3.13 or later, and the mi0bot fork | ANAN, Hermes, HPSDR, Red Pitaya, Hermes-Lite 2 | Yes | Normally leaves the stream alone. With one non-default option ticked, MUTE zeroes it. Stream level follows the AF slider | No | [T7][T10] |
| piHPSDR | HPSDR class | Yes, only at 48 kHz float32. Anything else is silently ignored | Taps audio before mute and compensates for AF | No | [T11] |
| deskHPSDR | HPSDR class | Yes | **MUTE zeroes the stream while blocks keep arriving** | No | [T12] |
| sdr-for-linux | HPSDR, ANAN, Hermes-Lite | Yes | Taps audio before mute (per its design note) | No | [T16] |
| Quisk | HPSDR class | Yes | No MUTE handling and no `mute:` report | No | [T14] |
| AetherSDR | FlexRadio, Hermes-Lite 2, networked Icoms. The networked-Icom case is excluded: its audio arrives over Icom's LAN protocol, the same side door as wfview (section 2.6) | Yes | Reports `mute:<trx>,<bool>` for each receiver and ignores `mute:true;`. Today's parser misreads that report (section 5.2, item 2) | No | [T15][A27] |
| Lyra, NereusSDR, QK4 (Elecraft K4), Zeus, OpenX6X00 (Xiegu X6100/X6200), zBitx | various | **Unverified** (read at grep level only). For QK4 the source is a design document, not an implementation | Zeus and OpenX6X00 always report not muted. zBitx mutes by setting volume to 0 | No | [T17][T18] |

Thetis by default, piHPSDR and deskHPSDR all greet with `protocol:ExpertSDR3,2.0`. With its EmulateExpertSDR3Protocol option off, Thetis sends `protocol:Thetis,2.0;`. So the greeting cannot tell servers apart [T7][T11][T12].

### 2.4 FlexAPI: radios that speak it

**Tested with this app: none.**

| Model | Max slices | DAX audio channels | Sold now | Sources |
|---|---|---|---|---|
| FLEX-6300 | 2 | 2 | No | [F20][F3] |
| FLEX-6400, 6400M | 2 | 2 | No | [F20][F3] |
| FLEX-6500 | 4 | 4 | No | [F20][F3] |
| FLEX-6600, 6600M | 4 | 4 | No | [F20][F3] |
| FLEX-6700 | 8 | 8 | No | [F20][F3] |
| FLEX-6700R (no transmitter) | 8 | 8 | No | [F20][F3] |
| FLEX-8400, 8400M | 2 | not documented | Yes | [F20][F1] |
| FLEX-8600, 8600M | 4 | not documented | Yes | [F20][F1] |
| AU-510, AU-510M | 2 | not documented | Deposits only | [F20][F1] |
| AU-520, AU-520M | 4 | not documented | Deposits only | [F20][F1] |

FlexLib 4.3.16 also lists ML-9600/W/X, MLS-9601, CL-9300, CLS-9301 and RT-2122 [F20]. Their capabilities were not researched.

### 2.5 Notable radios that are not supported, and why

| Radio or software | Why not | Confidence | Sources |
|---|---|---|---|
| IC-7700 | Its USB port is a Type-A port for memory sticks and keyboards. Control is RS-232C | confirmed | [I46] |
| IC-7800 | USB is for a keyboard only | confirmed | [I46] |
| IC-7000, IC-718, IC-R9500, ID-5100 | No PC-facing USB audio (CT-17 / RS-232C, memory-only USB, or the PC's own USB through a cable) | confirmed; ID-5100 likely | [I47] |
| IC-R30 | USB modes are Charge, Serialport and SD Card | likely | [I47] |
| TS-2000, TS-480, TS-570D/S | RS-232C and analog audio only | confirmed | [K23][I47] |
| TH-D72 | USB is described only for COM, TNC and GPS | likely | [I47] |
| FT-891 | The Advance Manual wires audio through the RTTY/DATA jack and uses USB for serial only. FreeDV and Digirig agree. One Operating Manual sentence says otherwise | likely | [Y13] |
| FTDX1200 | Its "USB Audio CODEC" belongs to the external SCU-17 | confirmed | [Y22] |
| FT-818, FT-817ND, FT-857D, FT-897D, FT-450D, FT-950, FTDX5000, FT-2000 | No built-in USB audio (CT-62, SCU-17 or RS-232C) | confirmed | [Y22][Y12] |
| ID-52, ID-52 PLUS, ID-5200, TM-D750, FT5DR, FT3DR, FTM-300DR, FTM-500DR | Not established either way. No claim | unverified | [I34][I48] |
| Icom "X-026" concept | No name or specification yet | unverified | [I49] |
| SparkSDR | Has no TCI | confirmed | [T13] |
| TAPR OpenHPSDR-Thetis 2.10.0.0, and ramdor Thetis before 2.10.3.13 | No RX audio over TCI | confirmed | [T10][T7] |
| wfview's TCI server | Its header leaves the channels word unset, so the app would reject the blocks. Its mute is mapped to nothing. Carrying an Icom over LAN, it would also bring a vendor network protocol in by the side door | likely | [T19][H21] |

### 2.6 Not doing

- **Vendor network audio.** Icom LAN / RS-BA1 (UDP 50001 to 50003) [A3][I2], Kenwood KNS and the TS-890S VoIP stream (TCP 60000, RTP on UDP 60001) [K30], and Yaesu SCU-LAN10 (UDP 50000 to 50003) [Y24] are all excluded by the rule. A radio with both USB and LAN uses USB. So this plan does not vendor AetherSDR's Icom transport (IcomProtocol, IcomSession, IcomStream, IcomAudio), stores no network credentials, and needs no kappanhang attribution [A14].
- **TCI that carries a vendor network protocol.** wfview's TCI server, and AetherSDR's TCI server when its radio is a networked Icom, would bring Icom LAN audio in by the side door. No claim and no work. Those radios are reached directly by USB where they have it [T19][A27].
- **External audio interfaces** such as SignaLink, SCU-17 or Digirig, and RS-232 radios with a separate sound card.
- **Transmit of any kind.** No PTT, no TX audio, no keying, no tuning. The app stays receive-only.
- **Writing radio menus.** The app's own code reads menus at most and never writes them. Its only write to a USB radio is the AF level. Hamlib's own writes at open are listed in section 4.3 as a decision.
- **Dedicated speaker-mute commands in v1:** Icom 1A 09, Kenwood QS and MU, Yaesu FR. Their effect on the USB stream is undocumented [I5][K2][Y3].
- **Automatic serial-port scanning.** Opening a port can pulse DTR and RTS, and on some radios those lines key the transmitter [Y21][I16][P13].
- **Flex** `remote_audio_rx`, Opus, SmartLink (WAN login and TLS), and spectrum or waterfall data.
- **A rigctld sidecar, OmniRig, wfview as a dependency, and CAT-to-TCI bridges** (section 4).

### 2.7 The three paths compared

| Path | Audio transport | Control path | How the operator's other audio is silenced | Does that leave our stream intact? |
|---|---|---|---|---|
| USB: Icom | USB Audio Class codec; PCM2901E where known, 16-bit, up to 48 kHz [I35] | CI-V over the USB virtual COM port, through Hamlib | Read AF with CI-V `14 01`, save it, write `14 01 00 00`, restore the saved value [I5] | Strong for IC-7300, IC-7100, IC-9100, IC-7410, IC-7600, IC-R8600. Likely for the others, including IC-9700 until its schematic reading is re-checked. Inference only for IC-705 and IC-905. **Untested on all** |
| USB: Kenwood | USB codec; PCM2903B (TS-590SG) or PCM2903C (TS-890S) [K16][K22] | Kenwood CAT over virtual COM. On the TS-890S this is the **Standard** port [K8] | `AG` to 000, with per-model syntax; restore the saved value [K2][K4][K3] | Implied, never stated for USB. ACC2 independence is stated. **Untested** |
| USB: Yaesu | USB codec; PCM2903C (FTDX101) or PCM2903B (FT-991, FT-991A) [Y1][Y4] | Yaesu CAT over the CP2105 **Enhanced** port [Y3] | `AG0000;`, plus `AG1000;` for SUB where the radio has one; restore the saved value [Y3] | Shown on paper by the schematics for FTDX101 and FT-991A. Inference for FTDX10, FT-710, FTX-1, FTDX3000. **Untested** |
| TCI | WebSocket binary audio blocks [T2] | The same TCI socket | `mute:true;`, restored to its prior value | Measured on ExpertSDR3 2.1 with a ColibriDDC only. deskHPSDR zeroes the stream, and Thetis does with one option set [T12][T7] |
| FlexAPI | VITA-49 over UDP, as a DAX RX stream: 24 kHz stereo float32 or mono int16 [F20] | TCP 4992 text API, as a non-GUI client [F5][F6] | `slice set N audio_mute=1` on the operator's slice, restored afterwards [F7] | AetherSDR states DAX is unaffected by slice mute; its active test radio is a FLEX-8600. No FlexRadio document says so. **Unverified on FLEX-6000** [A16] |

## 3. Architecture

### 3.1 Where the seam goes

Today `Engine` owns a concrete `TciClient`. Outside `src/tci`, `grep -rE 'Tci|tci::|m_tci|tci/' src` matches 61 lines in 7 files: Engine.h 6, Engine.cpp 31, MainWindow.h 1, MainWindow.cpp 5, RadioDiscoveryDialog.h 4, RadioDiscoveryDialog.cpp 4, main.cpp 10. Everything below `Engine::onAudioBlock` is already source-neutral: `RxChain`, `PlaybackBuffer`, `DriftController` and `SpscStereoRing`.

The seam is a new `src/radio/RadioBackend.h`. Engine holds one backend, created per connect by a `BackendFactory` that sits beside the existing `SinkFactory` (Engine.h:65-71). Before the first connect, Engine holds an idle backend that reports `Disconnected` and no receivers. `emitStatus` and housekeeping run from construction and would otherwise dereference a missing backend.

```
                     Engine (engine thread)
   RxChain -> PlaybackBuffer -> DriftController -> AudioSink (unchanged)
   MuteController (generalised)    liveness and silence guard (new)
                          ^
                          |  RadioBackend  (src/radio/RadioBackend.h)
        +-----------------+------------------+---------------------+
        |                                    |                     |
   TciBackend                           UsbBackend            FlexBackend
   TciClient (QWebSocket)               AudioSource            FlexClient   (TCP 4992)
   audio, state and mute                  (PortAudio capture)  VitaDecoder  (UDP, DAX RX)
   on one socket                        RigControl             FlexDiscovery (UDP 4992,
                                          (Hamlib, own thread)                listen only)
                                        UsbModels table
```

Interface sketch. The names are proposals, and the namespace follows the project's `esa`.

```cpp
// src/radio/RadioBackend.h
namespace esa::radio {
enum class Link { Disconnected, Connecting, Initializing, Ready };   // same four as today
enum class ModeClass { Unknown, Voice, Cw, Digital };
enum class SpeakerControl { None, Switch, Level };                    // TCI and Flex: Switch. USB: Level or None

struct Capabilities {
    SpeakerControl speaker = SpeakerControl::None;
    bool audioProvesRadio = false;   // true only for TCI: frames arriving prove the radio is reaching us
    bool reportsMode = false, reportsFrequency = false, reportsTransmit = false;
    int  closeWaitMs = 1500;         // today's kShutdownWaitMs
    int  muteRetryMs = 300;          // today's kRetryMs, which is TCI-specific
};

struct ReceiverState { int index = 0; bool enabled = true;          // "switched off" drives a GUI notice
                       double frequencyHz = 0; QString modeLabel; ModeClass mode = ModeClass::Unknown;
                       bool transmitting = false; };

struct RadioIdentity { QString kind;                  // "tci", "usb", "flex"
                       QString serverName, serverVersion, device;   // TCI fields today, model name for USB
                       bool receiveOnly = false;
                       QList<ReceiverState> receivers; };           // count and per-receiver state

class RadioBackend : public QObject {
    Q_OBJECT
public:
    virtual Capabilities capabilities() const = 0;
    virtual void open(const RadioEndpoint&) = 0;         // keeps retrying until close()
    virtual void close() = 0;                             // nothing is emitted after Disconnected
    virtual void flush() = 0;                             // today's TciClient::flush() before close
    virtual Link state() const = 0;                       // synchronous: disconnect and housekeeping ask it
    virtual RadioIdentity identity() const = 0;
    virtual QString lastError() const = 0;                // e.g. "(is TCI enabled?)"
    virtual quint64 malformedBlocks() const = 0;
    virtual void requestAudio(int receiver, int chainRate) = 0;   // re-applied across reconnects;
                                                          // re-emits that receiver's current ReceiverState
    virtual void stopAudio() = 0;
    virtual void setSpeakerMuted(bool muted, int restoreLevel) = 0;  // restoreLevel -1 for Switch
    virtual void releaseAudioDevices() = 0;               // PortAudio rescans only when every holder lets go
    virtual void reacquireAudioDevices() = 0;
    virtual QString muteKey() const = 0;                  // empty until resolved. TCI: the host string,
                                                          // byte-identical to today
signals:
    void linkChanged(esa::radio::Link);
    void identityChanged();                               // replaces radioChanged()
    void receiverChanged(const esa::radio::ReceiverState&);
    void muteKeyResolved(const QString& key);             // Flex learns client_id late (section 5.3)
    void endpointResolved(int port);                      // TCI: the fallback port that worked, persisted
    void speakerReported(bool muted, int level);          // level -1 when not a level
    void audioBlock(const float* stereo, int frames, int sampleRate);  // interleaved, engine thread
    void audioLost(quint64 blocks);
    void configurationWarning(const QString& text);
};
}

// src/radio/RigControl.h  (USB backend only; every call runs on one worker thread)
class RigControl {
public:
    virtual bool open(const RigProfile&) = 0;
    virtual void close() = 0;
    virtual std::optional<RigIdentity> identify() = 0;
    virtual std::optional<double> frequencyHz() = 0;
    virtual std::optional<ModeReading> mode() = 0;          // mode plus DATA flag
    virtual std::optional<bool> transmitting() = 0;         // read only
    virtual std::optional<int> afLevel(Receiver) = 0;       // 0..255
    virtual bool setAfLevel(Receiver, int level) = 0;       // the only write
    // Deliberately absent: PTT, raw send, power, tuning, menus.
};
```

Threading. Adopt the rules from AetherSDR's `IRadioBackend` contract, not its code [A10]:

- The backend lives on the engine thread, and every signal comes from it.
- Workers are private to the backend.
- Teardown is bounded.
- Nothing is emitted after `Disconnected`.
- The PortAudio capture callback never touches a QObject.
- PortAudio device listing runs on the engine thread only. `listOutputDevices` "must not be called while a PortAudioSink is open", and PortAudio snapshots its device list at first initialisation (PortAudioSink.h:16-18, PortAudioSink.cpp:16-32).
- Hamlib calls are blocking serial transactions, so they run on `RigControl`'s own thread with one request outstanding. That model holds only if Hamlib's own threads are switched off (section 4.3, "Hamlib's own threads and cache").

**What the GUI needs to know.** `EngineStatus` is TCI-shaped today: `connection` is a `TciClient::State`, and it carries `serverName`, `device`, `trxCount` and `connectedPort` (Engine.h:23-56). The headless reporter and the SUMMARY JSON print those too (main.cpp:185-241). Phase 1 changes it as follows:

- `connection` becomes `Link`. The existing fields stay, filled from `identity()`, so headless output and SUMMARY JSON keep their keys. New keys are only added.
- Add the backend kind, the `Capabilities`, and the receiver list.
- Phase 3 adds the capture device name, the capture error and capture liveness (the mirror of `outputError` and `outputOpen`), plus the latest `configurationWarning` text.
- Phase 5 adds the AF debt state: an owed record with the radio's speaker at zero.
- Capabilities drive the GUI. With `SpeakerControl::None`, the mute checkbox is hidden.

**What "receiver" means per backend.** Today `setReceiver` only resets the chain and asks for audio again (Engine.cpp:304-314). That is correct only because TCI's `mute:` is radio-global.

- TCI: the `trx` index, as today. The mute stays radio-global.
- USB: the radio's Main or Sub, on models with a dual-receiver flag. In v1 only Main is offered (section 5.1).
- Flex: a slice, and the mute is per slice.
- Where the mute is per receiver, `setReceiver` while holding restores the old key, then engages the new one. Until that exists, refuse to switch while holding. The receiver list comes from `identity().receivers`, not `trxCount`.

**When the mute key is known.** Engine creates the `MuteController` in `connectRadio`, before the backend opens, keyed by `m_config.host` (Engine.cpp:91-104, 253). A Flex key needs the GUI `client_id`, which arrives only after `sub client all`. So Engine creates or re-keys the controller on `muteKeyResolved`, before the first `speakerReported` and before `onConnected` marks it live. TCI resolves its key at once, so today's order is unchanged.

**What moves out of Engine into TciBackend:**

- `decodeToStereo` and the malformed-block check (Engine.cpp:452-458)
- the receiver filter (:450)
- packet-counter gap logic (:475-481)
- block-size choice (:321-322)
- fallback ports (:257)
- DIGL/DIGU I/Q handling (:483-486), together with `modeIsComplexStereo` (NrProcessor.h:52-54)

`modeBypassesNr` (NrRegistry.cpp:327) becomes TCI's translation into `ModeClass`, and `updateNrBypass` asks for the `ModeClass`. Without CAT the mode is Unknown, so auto-bypass never triggers. Neural NR would then run on CW and digital signals, the "28 dB hole" the code warns about (NrProcessor.h:47-50). So audio-only USB gets a manual "not speech" selector, and a warning when NR is on with the mode unknown (phase 3).

**What stays in Engine:**

- rate following (Engine.cpp:463-473), now driven by the block's rate
- connect, close and retry sequencing, using `Link`
- everything from :488 down

Swap the old backend only in `finishClosing`, and delete it with `deleteLater`. The mute action must go through `m_backend`, not a captured pointer.

The chain accepts only 24000 or 48000 Hz (Engine.cpp:464, Settings.cpp:94, main.cpp:380, NrProcessor.h:39). Each backend therefore delivers 24 or 48 kHz. USB capture resamples from the device rate, and Flex decimates 96 kHz or refuses it.

### 3.2 What is reused from AetherSDR, and what is not

All of it is GPL-3.0, from commit 141231e4, the same commit as `vendor/aethersdr`.

| Item | Use | Reason |
|---|---|---|
| AetherRX chain, already vendored | Unchanged | |
| `Resampler` (r8brain), already vendored | Reuse for capture rate conversion on the engine thread | Already in `vendor/aethersdr` |
| `CommandParser.{h,cpp}` (216 lines) | Vendor in phase 8 | Needs only QtCore and `src/core/RadioMessageTypes.h` [A15] |
| `RadioDiscovery.{h,cpp}` (486 lines) | Not used. Write `FlexDiscovery` from scratch | It parses the whole datagram as text, whereas FlexLib skips the 28-byte header. `parseDiscoveryPacket` is private and `onReadyRead` feeds it the whole datagram, so fixing that means editing the vendored file. Its header also pulls in `RadioSettingsIdentity.h` [A15][F20] |
| VITA audio decode (PanadapterStream.cpp:993-1036) | Rewrite, about 60 lines | Its file drags in AudioEngine, AppSettings and the FFT code [A15] |
| `IcomCivScheduler` "a write supersedes older observations" | Pattern for `RigControl`'s queue | Prevents stale AF reads being taken as operator overrides [A9] |
| PC Audio `m_dataOffModRestore` | Pattern: latch the radio's own value just before the first write | Matches MuteController's prior-state rule [A6] |
| `CivCodec` | Not in v1. Fallback if a native Icom driver is ever needed | Qt-free and transport-free: "over USB the same bytes go straight to a serial port" [A8] |
| PR #4921 `Ft991Cat` | Not in v1. Pattern for a native Yaesu driver if one is needed | Unmerged, with changes requested. Radio facts are the PR's own [A12] |
| `tests/gui_client_registration_recovery_test.cpp` | Pattern for a FakeFlexRadio (QTcpServer playing the radio) | [A21] |
| `packaging/macos/AetherSDR.entitlements` | Precedent: audio-input only | [A23] |
| Icom LAN stack, `IRadioBackend`, `RadioModel`, `FlexBackend`, `PanadapterStream`, `RadioCapabilities.h`, `SmartCatProtocol`, `RigctlProtocol`, the sim backend | Not used | Out of scope, too coupled to the app, or server-side. The sim backend has no sockets [A10][A15][A22] |

Vendored files stay an unmodified subset. `scripts/sync-aether.sh` gains `CommandParser` in phase 8, and nothing before that. The script deletes the vendor tree and copies only from `src/core/<file>` (scripts/sync-aether.sh:16-38). `CommandParser` lives in `src/core/backends/flex/` and needs `src/core/RadioMessageTypes.h`, so the script needs a second source path.

### 3.3 Mute

Keep the state machine. Move `src/tci/MuteController.*` to `src/radio/` (phase 2) and extend it for levels (phase 5):

- `Ownership` gains `int restoreLevel{-1}`. The early-return comparison in `setOwnership` (MuteController.cpp:17) must include it.
- `onServerMute(bool muted, qint64 now, int level = -1)` and `Actions.sendMute(bool muted, int restoreLevel)`. TCI passes -1 and never sends VOLUME, so `neverSendsVolume` (tests/test_engine_e2e.cpp:293) keeps passing.
- `kRetryMs` and `kMaxAttempts` become per-backend. The 300 ms value is justified by ExpertSDR3's 200 ms monopoly window (MuteController.h:51).
- The retry rule is for Switch backends only. In Engaging, a contradicting report counts as a dropped set and `tick()` resends, up to 10 times (MuteController.cpp:121-124, 145-159). That fits a TCI server that silently ignores sets. Hamlib sets are acknowledged, not dropped. So for Level backends, an AF read taken after an acknowledged set that differs from the written value means the operator moved a knob: go to OperatorOverride, never resend. Test it with `MockRig`.
- "Muted" means level == 0 exactly. An AF already at 0 goes straight to Holding without ownership and is never "restored", the same rule as today.
- Persist before the first set, as already done (MuteController.cpp:69-71).
- `beginEngage` keeps an existing prior (MuteController.cpp:67-69). It must update the level when the radio reports audible again before re-engaging, or a stale level would overwrite one the operator just set.

Duties the controller cannot enforce, which fall on the USB backend:

- Report only AF readings taken after the latest set. A reply already in flight when the mute landed would otherwise read as an operator unmute (MuteController.cpp:125-129).
- Poll. Icom does not push AF changes [I5]. Hamlib turns Kenwood and Yaesu Auto Information off at open (section 4.3), so pushes are not available there either.
- Put restores at the head of the queue so they fit inside `closeWaitMs`.
- A restore refused during transmit keeps ownership. The GUI must say plainly that the radio's speaker is still at zero.
- On startup with an owed record: read AF. If it is still 0, restore. If it is anything else, the operator or a power cycle has changed it, so clear the record and leave the radio alone. Whether AF survives a power cycle is **unverified** on every model.
- A non-zero AF read while holding is an operator override. On the IC-7300MK2 and IC-7760 at their default setting, moving any of the AF, RF or SQL dials overwrites AF [I3][I11].
- Kenwood: no AF write until an AF read has succeeded in the current open, restore paths included. Hamlib learns the `AG` format only in `kenwood_get_level`. A set before that sends an uninitialised buffer to the radio (section 4.3).

**Liveness and the silence guard** (new, safety-critical):

- `audioProvesRadio` is true for TCI and false for capture. For capture, the first frame must not set `m_audioConfirmed` (Engine.cpp:508-511). Require sustained non-zero signal, plus a Ready link where CAT exists.
- After engaging any mute, watch the input level. If it collapses within a short window, release the mute, stop muting that radio for the session, and raise `configurationWarning`. This catches four cases: deskHPSDR, Thetis's option [T12][T7], a USB radio whose stream follows AF, and macOS delivering zeros after a microphone denial [P8].
- "Collapses" must cover a fall to a non-zero noise floor, not only exact zeros. A USB codec whose stream follows AF falls to its own noise floor. The thresholds must be measured on at least one real USB radio before any mute opt-in is offered. They are not set here.
- **A squelch-gated USB stream cannot prove liveness.** Some USB radios rightly send silence: Icom with USB AF SQL on, FM with the squelch closed, the TS-890S in FM with CTCSS active (section 5.1). In those states audio never confirms, and a squelch closing just after engaging would trip the guard. Read squelch state over CAT, read-only, where Hamlib offers it, and pause the guard while it is closed. Otherwise exempt squelched modes from the guard and say so in the UI.
- **Transmit-aware dead stream, for every backend.** Where `reportsTransmit` is set and the radio reports transmitting, do not release the mute because the stream went quiet, and pause the silence guard. Otherwise an over longer than 3 s trips the dead-stream rule (Engine.cpp:532-535) and unmutes mid-transmission. For TCI this means no `mute:false` while `trx` is true [T5]. In Flex simplex, all slices are muted while transmitting [F3]. USB TX state comes from Hamlib, which can answer from a cache unless caching is switched off (section 4.3).
- **Output liveness by callback progress.** The existing sink's `alive()` is `Pa_IsStreamActive` (PortAudioSink.cpp:165-168), and Engine relies on it to hand the radio its audio back when the output dies (Engine.cpp:441, 519-526). PortAudio keeps reporting a stream active after its device is lost [P2]. Watch `SinkStats.callbacks` instead, as for capture.

### 3.4 Drift control and capture

`PlaybackBuffer` and `DriftController` need no code change. Only the comment at DriftController.h:13-14 needs updating. Drift runs in the same direction as with TCI: producer clock, then ring, then the drift controller at the output callback.

A throwaway simulation of the real `DriftController` header, with a capture device on its own clock, held the buffer for callback sizes up to 1024 frames. 4096-frame callbacks underran. The controller held at ±1900 ppm and diverged past its ±2000 ppm clamp. The simulation modelled the controller only: no PortAudio, no prebuffering, no underrun bump and no overrun skip. Phase 3 turns it into a committed test.

New code:

- `src/audio/AudioSource.h`, the mirror of `AudioSink`.
- `src/audio/CaptureBuffer.*`, Qt-free like `PlaybackBuffer`.
- `src/audio/PortAudioSource.*`, a thin wrapper:
  - The capture callback writes into an `SpscStereoRing` with channel mapping.
  - It counts ring overflow and `paInputOverflow`.
  - A timer of about 5 ms on the engine thread drains fixed blocks: 512 frames at 48 kHz, 256 at 24 kHz. Use `Qt::PreciseTimer`; the default `Qt::CoarseTimer` may resolve near 15.6 ms on Windows (general knowledge, **unverified** here).
  - Resampling happens on the engine thread, never in the callback.
  - The stream opens with `defaultLowInputLatency`, and `framesPerBuffer` is pinned at 1024 or less.
  - Measure the callback size at open. Above 1024 frames, raise the buffer target and show a warning. Test it.
  - Size the capture ring for the worst engine-thread stall. Stalls that exist today: a QSettings `sync()` on every ownership change (Settings.cpp:168), and a chain rebuild plus sink reopen inside `onAudioBlock` when the stream's rate changes (Engine.cpp:463-473).

Capture rules:

- **Never fall back to a default device.** `PortAudioSink::findDevice` falls back to the system default (PortAudioSink.cpp:43-50). Copied to capture, that would make the Mac's microphone "the radio".
- **Watch callback progress, not `Pa_IsStreamActive`.** PortAudio keeps reporting a stream active after its device is lost [P2]. The same applies to the existing sink (section 3.3).
- **List input devices on the engine thread.** Extend `refreshDevices` and `devicesChanged` to inputs (Engine.cpp:418-426). `refreshDevices` releases the capture stream as well as the sink, then reacquires both. The picker shows that list. A listing from the GUI thread while the engine holds streams would return a stale snapshot, missing a radio just plugged in, and would call `Pa_Initialize` and `Pa_Terminate` concurrently with the engine (whether that is safe is **unverified**).
- **Never offer the radio's own codec as an output.** Exclude the selected capture device from the output list, and refuse it at open, including when "System default" resolves to it. A radio with DATA VOX on may transmit computer sound sent to it [K13][K9].
  - Device keys are `<host api>|<name>` (PortAudioSink.cpp:34-39). Equal keys identify one device on macOS and ALSA, where one name covers both directions.
  - On Windows, the input and output endpoints of one codec usually have different names, such as "Microphone (USB Audio CODEC)" and "Speakers (USB Audio CODEC)", and appear under several host APIs (general knowledge, **unverified** here). Key equality would not exclude the radio's output.
  - So define a physical-device match per host API: the Windows endpoint container ID, or a name heuristic with a warning. Test it on Windows before claiming the rule.
- **Share the audio device.** Operators often run WSJT-X or fldigi on the same codec. A raw ALSA `hw:` device is normally single-open, which would lock one program out (general ALSA behaviour, untested here). PortAudio lists every host API's variant, exclusive ones included (PortAudioSink.cpp:64-88). Prefer shared devices: PipeWire or Pulse, `dsnoop`, WASAPI shared mode. Detect "device busy" and tell the operator another program holds the device, naming it where the OS reports it.
- **Open the device as stereo and choose a channel per model:**
  - IC-7300 receive audio is on the left (community source, likely) [I36].
  - FTDX101: L is MAIN, R is SUB (schematic) [Y1].
  - TS-990S: L is main, R is sub in Normal mode, set by menu 7-16 [K11].
  - IC-7610: Main and Sub on L and R (community sources only, **unverified**) [I37].
  - IC-9700: separate D/A channels; the L/R assignment is unconfirmed [I25][I38].
  - TS-890S needs "2 channel" capture [K8]. TH-D75 is mono [K20].
  - Unknown models default to left, with a selector.

### 3.5 Settings

- Rename `radioHost` to an opaque `radioKey` in `loadMuteOwnership` and `saveMuteOwnership` (Settings.cpp:142-169). TCI's key stays the host string exactly: `muteOwnership/<percent-encoded lowercase host>`. Records owed by v0.1.1 and `adoptSingleMuteRecord` keep working, and so does a downgrade to v0.1.x.
- Two TCI servers on one host (for example ExpertSDR3 and Thetis on different ports) would share that record, and a restore could unmute the wrong one. So add a `/port` value under the record when it is written. Restore only when the connected port matches. A record with no `/port`, written by v0.1.x, matches any port, as today. This adds a value and renames nothing. Test both cases.
- USB keys are `usb:` plus the serial-bridge USB serial number. The fallback is VID:PID plus port name, never an enumeration index. Flex keys combine radio serial, GUI `client_id` and slice letter. The Flex key is known only after `sub client all`, so it arrives through `muteKeyResolved` (section 3.1).
- Add `/priorLevel` only when it is set.
- Keep `radio/host`, `radio/port` and `radio/receiver` unchanged. Add:
  - `radio/backend` (absent means tci)
  - `radio/serial/{port,baud,model}`
  - `radio/capture/{device,channel}`
  - `radio/flex/{serial,station,slice}`
- Switching radio type keeps the TCI host.
- Profiles already exclude station settings (tests/test_gui.cpp:258), so a radio choice stays out of profiles.
- Add `pendingMuteDebts()`. A radio left with AF at 0 stays silent, so a debt on a radio that is not currently selected gets a visible notice.

### 3.6 Discovery

The discovery dialog becomes a radio picker with three sections:

- **On the network (TCI).** Today's scan, unchanged. It still sends nothing (`aProbeSendsTheRadioNothingAtAll`, tests/test_discovery.cpp:150).
- **FlexRadio.** Listen for the radio's broadcast on UDP 4992, sending nothing. The socket must allow sharing, because SmartSDR, DAX or AetherSDR on the same host bind 4992 too [F4][F20].
- **USB.** List `QSerialPortInfo` metadata (serial number, VID, PID) and the engine's input-device list (section 3.4), **without opening any port or stream**. The operator picks model, port and audio input. A separate **Test** button opens only the chosen port and sends one read-only identify. The picker shows which of a radio's ports carries CAT, and links the manufacturer's driver page when no port appears.

`connectToRadio` and `selectedRadio` move from `DiscoveredRadio` to `RadioEndpoint`.

CLI changes:

- `--discover` stays TCI.
- Add `--list-inputs` and `--list-serial` to the console-only list (main.cpp:255-256).
- Add flags for serial port, model, capture device and Flex slice.

## 4. Control path decision

### 4.1 Recommendation (the user's decision, section 10, item 0)

**Link libhamlib in-process, behind the narrow `RigControl` interface above, on its own worker thread.** Pin it to a commit on the `Hamlib-4.7` branch that carries the October and November 2026 fixes, or to 4.7.3 once that is tagged. Re-check the advisories and the branch head when pinning. The conditions are in 4.3 and in section 10, items 4 to 6.

Native per-family drivers are the fallback, and only for a family where Hamlib proves wrong at the bench. Qt SerialPort is used only to list ports, because Hamlib opens them itself.

### 4.2 Reasons

1. **We have no radios to test native drivers against.** Native drivers written from manuals could only be checked against mocks written from the same manuals. Hamlib's backends already encode field quirks:
   - Kenwood's AG format is probed as `AG`, `AG1`, then `AG0` [H9]. The probe runs only on a read, which creates a rule of its own (4.3).
   - Yaesu `?;` busy replies are retried, and FTDX3000 reads taken just after a set can be stale [Y23].
2. **Coverage of the in-scope list.** Every USB model in 2.2 except TH-D74 (no AF) and TH-D75 (absent) has a Hamlib 4.7.2 entry that declares AF get and set plus frequency and mode reads [H25]. TS-990S and IC-R8600 lack a PTT read.
3. **One driver also reaches the operator's own sharing setup** (phase 6): rigctld or wfview's rigctld emulator as model 2, and flrig as model 4 [H18].
4. **Precedent.** QLog (GPL-3, Qt, C++, Windows, macOS, Linux x86-64 and aarch64) links Hamlib 4.7.2 this way and documents its MSVC recipe [H14].
5. **Receive-only by interface and by configuration.** `RigControl` has no PTT or raw-send verb, so the app cannot call `rig_set_ptt` through it. In-process Hamlib opens no listening socket, but only because `rig_init` defaults its multicast publisher and command-server addresses to `0.0.0.0`, which means off (Hamlib: src/rig.c:812-825, src/network.c:1612, 1813). An `#if 0` block there shows a 224.0.0.2 command server was once the default. So the app sets those addresses itself (4.3).

### 4.3 Costs accepted, each with its mitigation

- **Hamlib writes to the radio at open.** From reading the 4.7.2 source [H25]:
  - **Kenwood:** reads Auto Information, turns it off if on, and restores it at close (kenwood.c:1156-1168, 1228-1231). The restore goes through `kenwood_set_trn`, which sends `AI1` for most models. The TS-890S guide marks AI value 1 "Not used" [K24][K2]. What a TS-890S receives on restore is **unverified**.
  - **Yaesu:** the same AI off-and-restore (newcat.c:812-825, 938-942).
  - **Yaesu:** on FT-991, FT-991A, FTDX10 and FTDX3000, Hamlib also **writes an EX menu item at open**, raising the CAT time-out timer to 100 ms (value 1; the radios' default is 10 ms) (newcat.c:833-884). The exact strings, which the setup text must disclose per model:
    - FT-991 and FT-991A: `EX0321;EX032;` sets EX 032 CAT TOT to 1, then reads it back [Y5][Y7].
    - FTDX10: `EX0301091;EX030109;` sets EX 03 01 09 CAT TIME OUT TIMER to 1, then reads it back [Y10].
    - FTDX3000: `EX0391;`, set only, for EX 039 CAT TIME OUT TIMER [Y11].
    - `newcat_close` does not restore it (newcat.c:930-950). This changes an operator setting, which the app otherwise never does. It is an open decision (section 10, item 4).
  - **Icom:** in a skim of icom.c:1148-1260, the open path reads echo, dual watch and frequency, and powers on only if `auto_power_on` is set. Never set `auto_power_on`, unlike QLog [H14][H16].
  - Every model needs a Hamlib trace of every command sent from open to close, recorded on the bench, before it ships.
- **Kenwood AF set before AF read sends garbage.** `kenwood_init` sets `ag_format = -1` (kenwood.c:902). Only `kenwood_get_level` probes the format (kenwood.c:4026-4062). `kenwood_set_level` switches on `ag_format`, and its `default:` branch only logs "Unknown ag_format" (kenwood.c:3448-3465). It then sends the uninitialised `char levelbuf[16]` to the radio (kenwood.c:3373, 3670). A read that fails during the probe leaves the format unknown too (kenwood.c:4062-4074). TS-590S/SG and TS-890S pass AF set to `kenwood_set_level` (ts590.c:578-579, ts890s.c:105), and TS-990S uses it directly (ts990s.c:364). So every in-scope Kenwood is exposed.
  - Rule in `HamlibRig`: no AF write on Kenwood until an AF read has succeeded in the current open, restore paths included.
  - Test it with a PTY simulator (`simts590` and `simts890` handle AG) and with `MockRig`.
  - Offer an upstream fix: initialise the buffer and refuse the set while the format is unknown.
- **Hamlib's own threads and cache.** In 4.7.2:
  - `rig_init` sets `poll_interval = 1000` ("enable polling by default"), and `rig_open` then starts a poll thread (rig.c:812, 1720; event.c:264-310).
  - That thread sets the cache timeout of every item to the poll interval (event.c:86). `rig_get_ptt` answers from the cache while it is fresh (rig.c:3957-3992), so TX reads, and likely mode and frequency reads, can be up to 1 s stale. Even with no poll thread, the default cache timeout is 500 ms (rig.c:827).
  - A Morse handler thread starts for rigs with `send_morse` (rig.c:1612-1614).
  - So, before `rig_open`, set through `rig_set_conf`: `poll_interval=0`, `cache_timeout=0`, `multicast_data_addr=0.0.0.0`, `multicast_cmd_addr=0.0.0.0` and `auto_power_on=0` (Hamlib: src/conf.c:106, 141, 146, 206, 216). Phase 4 CI asserts these values after open. TX gating must allow for any staleness left.
- **Serial control lines.** Set Hamlib's `rts_state` and `dtr_state` (values `Unset`, `ON`, `OFF`; Hamlib: src/conf.c:456-500) explicitly in each model profile, never leaving them `Unset`:
  - Icom and Kenwood: OFF.
  - Yaesu Enhanced port: Hamlib's default hardware handshake, except FT-710 [Y23].
  - This does not cover the moment of open. Linux raises DTR and RTS on open before any code can lower them [P13]. How Hamlib's own Windows open sets the lines was not researched; a third-party note says Windows raises DTR and RTS when a port opens [K25]. On radios where USB SEND or keying is mapped to DTR or RTS, that could key the transmitter [I16][K25][Y21]. Mitigations: setup text telling the operator to check those settings, and a dummy-load test with the operator present on each OS (section 7).
- **AF is a float in Hamlib.** It is 0.0 to 1.0, truncated to an integer on set (icom.c:3620, kenwood.c:3389). Yaesu rounds (newcat.c:4128). A local check of all 256 values with IEEE float showed an exact round trip for Icom and Kenwood. Keep a unit test for it, because restore must be exact.
- **No pushes.** Hamlib turns AI off, so state is polled: AF, mode, frequency and TX. wfview's 200 ms polling interval is a starting point to measure, not a fact about these radios [I51]. At 19200 baud with an Icom linked to [REMOTE], the budget is tight [I16].
- **Model-specific read-only checks are not in Hamlib's generic API.** Examples are Icom Output Select set to IF and the Kenwood output mix. Hamlib has `rig_send_raw` (rig.c:8930). Defer these checks, so in v1 nothing in `src/` calls `rig_send_raw`. If they are added later, `rig_send_raw` is called from one wrapper in `HamlibRig` with a fixed allowlist of read commands taken from each guide, and the phase 4 CI ban then excludes only that wrapper.
- **Security.** GitHub lists six Hamlib advisories, published 2026-10-23, whose ranges end at 4.7.2 with no patched version [H5]:
  - GHSA-3qgv-jc7m-q329, critical: SmartSDR status parsing.
  - GHSA-q55w-8m34-8wjf: TCP daemons do not limit client threads.
  - GHSA-xxp5-cm5j-pg53 and GHSA-grww-p672-wcf5: rigctld buffer overflows.
  - GHSA-8pgf-h33g-fmcv and GHSA-xgh4-3v35-8q74: NetRig, which is `netrigctl`, model 2.
  - A further advisory, GHSA-f72v-7gmh-m9mj, names `read_string_generic` on the serial path. It was audited on master at 2ba3c40, and its vulnerable `rxmax` × 5 line is not in 4.7.2's src/iofunc.c (lines 1465-1485). So, as far as checked, direct serial in 4.7.2 is not touched.
  - 4.7.3 is not tagged. The `Hamlib-4.7` branch has fixes up to 2026-11-05 [H6].
  - Re-check the advisories and the branch head when pinning. Do not offer models 2 or 4, or Hamlib's SmartSDR backend, until the build comes from a patched commit.
- **Windows build.** The official zip is a MinGW build. Generate an import library from `libhamlib-4.def`, take `pthread.h` from vcpkg pthreads, include `rig.h` in one translation unit, and treat `RIG*` as opaque [H11][H13][H14]. ClangCL with this recipe is **untested**; QLog documents plain MSVC. vcpkg's port does not support this project's x64-windows ClangCL triplet [H12]. A patched Windows DLL means cross-building with Hamlib's `scripts/build-w64.sh`.
- **Licence.**
  - Libraries are LGPL-2.1-or-later, which section 3 of that licence allows to be used under the GPL [H1][H2].
  - libhamlib also links all of `libsecurity.la` (Hamlib: security/Makefile.am, src/Makefile.am:26) [H3]. Its files carry different terms:
    - `AESStringCrypt.c`, `password.c` and `security.c`: "freeware", with permission to distribute but none stated to modify.
    - `aes.c` and `sha256.c` (Christophe Devine): GPL-2.0-or-later. That is compatible with this GPL-3.0 app, but it is not LGPL.
    - `md5.c`: no licence header. `md5.h` describes the code as Solar Designer's MD5, "placed in the public domain".
  - The freeware files are a question for the user (section 10, item 5), not a legal conclusion.
- **Distribution packages are too old.** Debian bookworm, the build container, has Hamlib 4.5.4. IC-7300MK2 and FTX-1 arrived in 4.7 [H15]. Build Hamlib from pinned source.

### 4.4 Rejected

| Option | Why rejected |
|---|---|
| Native CAT drivers per family (QSerialPort plus CivCodec or the Ft991Cat pattern) for v1 | Every quirk would have to be found on hardware we do not have. Kept as a per-family fallback. Yaesu is the cheapest if the EX write in 4.3 is unacceptable |
| rigctld sidecar | It binds 0.0.0.0 by default, its `-A` password is documented as NOT IMPLEMENTED, and any local process could send PTT or `send_raw` through it [H17]. It also adds process, signing and orphan handling |
| OmniRig | Windows only. The parameters read show no AF or volume, so there is no generic mute. v2.1 development has stopped [H20] |
| flrig as the primary path | It needs a separate install, and `rig.get_volume` returns a cached value, or 0 when the rig is offline, so it cannot be trusted for save and restore [H19]. Offered in phase 6 without mute |
| wfview as a dependency | It is a full application, not a library [H21]. Users can still reach it through model 2 |
| A CAT-to-TCI bridge | None usable. wfview's TCI server is the nearest and would need changes on both sides [H21][T19] |

## 5. Per-path detail

### 5.1 USB, all brands

- **Model table** (`src/radio/UsbModels.*`). Fail closed. It grows by phase, so that each field is a claim someone can check:
  - Phase 3: wave-1 rows only, each with a label and a channel map.
  - Phase 4 adds, only for models with a committed tester: Hamlib model id, CAT port hint and default baud, explicit DTR and RTS states, the mode-to-`ModeClass` table, and `catVerified` (the bench record id and the OSes where the trace and keying test passed; empty means CAT stays hidden).
  - Phase 5 adds `muteVerified` (bench record id or empty) and the dual-receiver flag.

  An unknown identify reply means refusal, never a guess. Firmware updates change CI-V commands, and item numbers are not stable across models [I17][I18]. Never borrow one model's menu numbers for another [A1].
- **Identify after open and refuse on mismatch.** Hamlib reports the model it found. Elecraft K2 and K3 answer `ID;` as 017, like a TS-570D, so a non-Kenwood radio can look like a Kenwood [K24].
- **Pairing a serial port to its audio device.**
  - The codecs carry no USB serial number [Y14][K16].
  - The IC-7300, IC-9700, FT-991A, FTDX101 and TS-590SG put codec and bridge under an internal hub [I24][I25][Y15][Y1][K16]. W9CF pairs them on Linux by walking the USB tree [K16]. Rigplane does it on macOS through IORegistry [I42].
  - No Windows method was found.
  - So v1 relies on the operator's selection, with name hints only, matched whitespace-tolerantly (section 2.1). The Linux PortAudio name embeds `hw:N,M`, so its device key may change across re-enumeration (**untested**) [P4].
- **Known USB IDs** (hints only, never identity):

  | Device | VID:PID | Seen on |
  |---|---|---|
  | PCM2901 codec | 08BB:2901 | IC-7300, IC-9700 [I40] |
  | PCM2903B codec | 08BB:29B3 | FT-991A, TS-590SG [Y15][K16] |
  | PCM2903C codec | 08BB:29C3 | datasheet [Y14] |
  | CP2102 | 10C4:EA60 | IC-7300, IC-9700 [I40] |
  | CP2105 | 10C4:EA70 | FT-991A [Y15][Y17] |
  | IC-705 serial | 0C26:0036 | third-party database, likely [I41] |

  IDs for the IC-7610, IC-7300MK2, IC-7760, IC-905, IC-R8600, TS-890S and TS-990S were not found.
- **Warnings at connect.** Read-only, and never rewrite the setting:
  - The stream may be IF rather than audio: Icom Output Select = IF [I8]; TH-D74 and TH-D75 menu 102 = IF or Detect, or menu 980 = Mass Storage [K21][K20].
  - Beeps and sidetone may be mixed into USB: TS-890S and TS-990S output type "All" is the default [K8][K9].
  - On Icom, the speaker control stays at zero while the app is active. A stuck AF of 0 looks like a dead receiver.
- **Input level handling stays in the app.** USB level is apparently fixed on FTDX101 and FTDX10. On FT-991A it follows DATA OUTG. On FT-710 and FTX-1 it is set per mode, so the level jumps when the mode changes [Y1][Y4][Y8][Y9]. Add an input trim. The "From radio" input meter already exists (MainWindow.cpp:571-581).
- **Port sharing.** A serial port is held by one program at a time [I42][K29]. Options:
  - Direct serial, the default.
  - A second native CAT port: IC-7300MK2 and IC-7760 with USB (B) Function set to CI-V [I3][I11]; FT-710 and FTX-1 on CAT-2 [Y8][Y9].
  - The operator's own rigctld or flrig (phase 6).
  - Audio only, with no CAT.

  So phases 4 and 5 help only operators with no other CAT program running, or with a second native CAT port. Anyone whose WSJT-X, logger or flrig already holds the port gains CAT and mute only in phase 6. The audio device can be shared from phase 3 (section 3.4).

#### Icom

Wire commands behind Hamlib's AF path (`C_CTL_LVL 0x14`, `S_LVL_AF 0x01`) [H9], from Icom's guides [I5][I7][I9][I3][I11][I13][I15][I1]:

| Purpose | Controller to radio | Reply |
|---|---|---|
| Read AF | `FE FE <addr> E0 14 01 FD` | `FE FE E0 <addr> 14 01 HH LL FD`, BCD from 00 00 to 02 55 |
| Mute | `FE FE <addr> E0 14 01 00 00 FD` | `FB` (OK) or `FA` (NG). Verify by reading again |
| Restore | `14 01 HH LL` with the saved value | `FB` or `FA` |
| Frequency | `03` | 5 BCD bytes, little-endian. IC-905: 6 bytes above 10 GHz [I13] |
| Mode | `04`, plus `1A 06` for DATA (first byte 00 = off, non-zero = on) | |
| TX state | `1C 00`, read only (00 = RX, 01 = TX). **Never send it with data** | |
| Model | `19 00` | |

The frame format is `FE FE <radio> E0 <cmd> [sub] [data] FD`. The controller address is E0 [I7].

Mode bytes differ by model, so the mode table is per model, taken from each model's CI-V guide. The common set is `00` LSB, `01` USB, `02` AM, `03` CW, `04` RTTY, `05` FM, `07` CW-R and `08` RTTY-R. Additions found:

- IC-7610 and IC-7760: `12` PSK, `13` PSK-R [I5][I11].
- IC-705: `06` WFM, `17` DV [I7].
- IC-9700: `17` DV, `22` DD [I9].
- IC-905: `17` DV, `22` DD, `23` ATV [I13].
- IC-R8600: `06` WFM, `11`, `14` and `15` S-AM variants, `16` P25, `17` D-STAR, `18` dPMR, `19` NXDN-VN, `20` NXDN-N, `21` DCR [I15].

RTTY, PSK or DATA on means Digital. Digital-voice modes (DV, D-STAR, P25, dPMR, NXDN, DCR) are mapped explicitly; whether NR helps decoded digital voice is a bench question. DD and ATV are Digital. Any code not in the model's table maps to Unknown.

| Model | Default CI-V address | Port to use | USB menu items under `1A 05` (Output Select / AF level / AF SQL) | Sources |
|---|---|---|---|---|
| IC-7300 | 94h | the single [USB] COM port | 00 59 / 00 60 / 00 61 | [I1] |
| IC-7300MK2 | B6h | Serial Port A (CI-V). USB (B) can be CI-V (00 94 = 01) | 00 69 / 00 70 / 00 71 | [I2][I3] |
| IC-7610 | 98h | [USB 1] | 00 80 / 00 82 / 00 83 | [I4][I5] |
| IC-705 | A4h | Serial Port A (CI-V) | 01 09 / 01 10 / 01 11 | [I6][I7] |
| IC-9700 | A2h | port A. USB (B) options exclude CI-V | 01 05 / 01 06 / 01 07 | [I8][I9] |
| IC-7760 | B2h | [USB B] on the controller | 01 03 / 01 05 / 01 06 | [I10][I11] |
| IC-905 | ACh | Serial Port A | 01 17 / 01 18 / 01 19 | [I12][I13] |
| IC-R8600 | 96h | front or rear USB, each with its own settings | rear 00 81 / 00 82 / 00 83; front 00 73 / 00 74 / 00 75 | [I14][I15] |
| IC-7850, IC-7851 | 8Eh | [USB B] | not researched | [I22] |
| IC-7100 | 88h | USB1 | not researched | [I26] |
| IC-7200 | 76h | [USB] | not researched | [I27] |
| IC-7410 | 80h | [USB] | not researched | [I28] |
| IC-9100 | 7Ch | USB1 | not researched | [I29] |
| IC-7600 | 7Ah | [USB] (B) | not researched | [I23] |

The addresses are the defaults printed in each model's manual. An operator may have changed them.

Icom surprises:

- **AF 0 is not sticky.** "If a control dial... is rotated after sending a CI-V command, the command will be overwritten" (IC-7610 and IC-9700 guides) [I5][I9]. IC-7300MK2 and IC-7760 default to "Cancel CI-V Remote Set Levels: All Volume Levels", so moving the RF or SQL dial also restores AF [I3][I11]. Do not change that setting; it belongs to the operator. Override behaviour on IC-705, IC-905, IC-R8600 and IC-7300 is undocumented [I7][I13][I15][I1].
- **AF is not a Transceive output.** Transceive pushes only frequency and mode, unacknowledged, and the frames may be addressed to 00 [I5][A8]. So the app polls.
- **Dual receivers.** IC-7610 and IC-7760 take Command 29 for per-receiver AF [I5][I11]. Whether Hamlib's VFO targeting uses it was not researched. IC-9700 has no Command 29, so reaching Sub appears to need `07 D0`/`07 D1`, which changes the operator's selected band (likely) [I9]. **Recommendation (the user's call, section 10, item 13): v1 mutes Main only on all dual-receiver radios**, and offers only Main as the receiver (section 3.1).
- **Serial settings.**
  - IC-7300 USB Echo polarity is printed 00 = ON, the reverse of other models [I1].
  - Link to [REMOTE] limits USB to 19,200 bps, and Unlink allows 115,200. The IC-7300 default changed from Link to Unlink at firmware 1.40/1.41 [I16][I17].
  - IC-705, IC-7300MK2 and IC-7760 USB serial works regardless of baud setting [I6][I2][I10].
- **Keying risk.** USB SEND and USB Keying can map DTR or RTS to transmit. Icom warns of unintended SEND when the virtual port connects, and provides an Inhibit Timer [I16][I8][I1].
- **Speaker extras at AF 0.** Beeps, voice, sidetone and TX monitor have their own levels. Whether they still sound at AF 0, and what USB carries during TX, is **unverified** [I1][I4].
- **Windows driver.** Install it before connecting the cable. The newer package excludes Windows on ARM, and Icom disclaims operation through a USB hub [I19][I20][I21].
- **Icom manual terms** allow individual use and prohibit redistribution. Keep the PDFs out of this public repo; cite them by URL [I5]. See section 10.

#### Kenwood

| Model | AF read / set | ID reply | Mode | TX state | CAT port | USB level menu | Sources |
|---|---|---|---|---|---|---|---|
| TS-890S | `AG;` / `AGnnn;` (000-255) | 024 | `OM0;`; C = LSB-D, D = USB-D, E = FM-D, F = AM-D | No documented read. `IF;` is undocumented but answered (Hamlib simulator, from a real radio) | Standard virtual COM. USB 9600-115200, 8N1 | 7-08 (0-100, default 100) | [K2][K8][K24] |
| TS-990S | `AG0;` / `AG0nnn;` main; `AG1;` / `AG1nnn;` sub | 022 | `OM0;` / `OM1;`; C-F = D1, G-J = D2, K-N = D3 | `IF;` P8 is always 0. Only the AI push reports it. Hamlib declares no PTT read | COM/USB-B virtual COM, menus 7-00 and 7-01 | 7-08 to 7-11; USB stereo by band, menu 7-16 | [K3][K9][K11] |
| TS-590SG | `AG0;` / `AG0nnn;` | 023 | `MD;` plus `DA;` (`DA` errors in CW and FSK; that is not a link failure) | `IF;` P8. The guide says IF "cannot read the transceiver status while it is in Data mode" (**unresolved**) | USB virtual COM, menu 68, default 115200 | 72 | [K4][K12][K13] |
| TS-590S | `AG0;` / `AG0nnn;` | 021 | `MD;` plus `DA;` | `IF;` P8 | menu 62 | 65 | [K5][K14] |
| TH-D74, TH-D75 | No published Kenwood command reference found | | | | virtual COM (CDC on TH-D75) | 918 (TH-D74), 91A (TH-D75) | [K21][K20] |

Kenwood surprises:

- **The CAT port name is the opposite of Yaesu's.** On the TS-890S, PC commands use the **Standard** port. Enhanced is for decoded characters and RTS/DTR keying [K8]. The TS-890S bridges are two CP2102N, not a CP2105 [K22].
  - Kenwood tells the two apart by internal hub port. In Windows' "Location paths", the port shown with (1) is Standard and the one with (2) is Enhanced (manual p. 1-5) [K8].
  - Hub port topology is also visible on Linux (sysfs devpath) and macOS (IORegistry `locationID`). So hub port 1 = Standard and 2 = Enhanced is a documented starting point, to be confirmed on a TS-890S on each OS (**untested** off Windows).
- **RTS and DTR keying.** On the TS-890S, RTS and DTR of both ports can map to PTT, DATA SEND or keying (Advanced Menu 17-20) [K8]. A third-party note says Windows raises both lines at open [K25].
- **DATA VOX.** With DATA VOX on, "the transceiver may send sounds emitted by the computer" over USB [K13][K9].
- **Stream contents.** TS-890S and TS-990S mix beeps and sidetone into USB by default [K8][K9]. On the TS-890S in FM with CTCSS active and Advanced Menu 13 "CTCSS Unmute for Internal Speaker" set to Unmute, the speaker passes all RX audio while ACC 2/USB passes only signals with a matching CTCSS tone (manual p. 16-8) [K8]. TS-590S/SG defaults to received audio only [K13].
- **Error replies.** `?;` means bad syntax or not possible now, `E;` a communication error, `O;` a buffer overrun. A missing reply is normal. On the TS-890S, PC commands stop when PSQ/PKS Pin Assignment (COM) is On [K2].
- **Beeps and CW sidetone do not follow AF**, so they stay audible at AF 0. This is stated for TS-590SG and TS-890S [K8][K13].
- **Hamlib's AI restore** may send `AI1` (section 4.3).
- **Read AF before writing it.** Hamlib's Kenwood AF set is unsafe until an AF read has succeeded (section 4.3).
- **Unverified:** whether `AG` survives a power cycle, and whether an AG change is pushed by AI.

#### Yaesu

Common commands, from the CAT manuals [Y3][Y5][Y8][Y9][Y10][Y11]:

- **AF read:** `AG0;`, answered `AG0nnn;`. Mute is `AG0000;`, verified with `AG0;` again, and restored with `AG0nnn;`. On FTDX101 and FTX-1, repeat with `AG1` for SUB. On FTDX10, FT-710, FT-991A and FTDX3000, P1 is fixed at 0.
- **Frequency:** `FA;`, answered `FAnnnnnnnnn;` (9 digits, Hz).
- **Mode:** `MD0;`.
- **TX state:** `TX;`, answered TX0 (off), TX1 (CAT TX on) or TX2 (radio TX on). Read it only. **Never send `TX1`.**
- **Auto Information:** `AI1;` on, `AI0;` off. It resets to off at power-off. On the FTDX10, AI works only over USB.

| Model | `ID;` reply | Sub receiver for AG | Mode letter E | CAT baud | Second CAT port | Sources |
|---|---|---|---|---|---|---|
| FTDX101D / MP | 0681 / 0682 | yes (P1 = 1) | PKT | 4800-38400 | no | [Y3] |
| FTDX10 | 0761 | no | PSK | 4800-38400 | no | [Y10] |
| FT-710 | 0800 | no | PSK | 4800-115200; CAT-1 default 38400 | CAT-2 on the Standard port | [Y8] |
| FTX-1 | 0840. CAT needs MAIN firmware 1.08 or later | yes. USB channel mapping unknown | PSK; adds H C4FM-DN and I C4FM-VW | 4800-115200; CAT-1 default 38400. Stop bits: **Yaesu's documents conflict**. The CAT manual marks 2 as the default (`1 or 2*`). The Advanced Manual says CAT-1/CAT-3 STOP BIT "Default Setting: 1bit" and CAT-2 is fixed at 1 bit, which agrees with Hamlib (ftx1.c:1005). Bench check | CAT-2 | [Y9][Y23] |
| FT-991A | 0670 | no | C4FM | 4800-38400 | no | [Y5] |
| FT-991 | 0570 (Yaesu's FT-991 CAT manual; Hamlib agrees, newcat.c:59). PR #4921's 0670 is the FT-991A's ID, so it is wrong for the FT-991 | no | C4FM | 4800-38400 (CAT RATE, menu 031) | no | [Y7][H25][A12] |
| FTDX3000 | manual: 0462. Hamlib: 460, and calls 462 an "undocumented FT-DX3000DM" (**confirm on a radio**) | no | none: codes run 1 to D only | 4800-38400 | no | [Y11][H25] |

Mode letters shared by all of them except the FTDX3000: 1 LSB, 2 USB, 3 CW-U, 4 FM, 5 AM, 6 RTTY-L, 7 CW-L, 8 DATA-L, 9 RTTY-U, A DATA-FM, B FM-N, C DATA-U, D AM-N. F is DATA-FM-N on FTDX101, FTDX10, FT-710 and FTX-1 [Y3][Y10][Y8][Y9]. The FTDX3000 uses other names for the same letters: 3 CW, 6 FSK (RTTY-LSB), 7 CW-R, 8 PKT-L, 9 FSK-R (RTTY-USB), A PKT-FM, C PKT-U [Y11]. Parse the letter as a symbol, not a hex digit.

Yaesu surprises:

- **Ports.** CP2105 Enhanced is CAT. Standard is "TX Controls (PTT control, CW Keying, Digital Mode Operation)". Yaesu warns "the transceiver may switch to the transmit mode when the computer is started" [Y3][Y21]. Open only the Enhanced port by default. Windows names them "...Enhanced COM Port (COMn)" and "...Standard COM Port (COMn)" [Y3]. That interface 0 is always Enhanced on Linux is likely but **unconfirmed**.
- **Timing and handshake.** CAT TOT defaults to 10 ms on the FTDX101, so write each command in one burst [Y3]. With CAT RTS ON, the radio monitors RTS [Y10]. On the FTDX3000, RTS low inhibits the radio's transmit data [Y11].
- **Hamlib at open** writes CAT TOT on FT-991, FT-991A, FTDX10 and FTDX3000, and turns AI off (section 4.3, with the exact strings).
- **Mute side effects.** The headphones go silent too. Tone and beep are summed after the AF volume on FTDX101 and FT-991A, so they probably stay audible [Y1][Y4]. AG stays at 0 in the radio after a crash, so the persisted prior value is mandatory.
- **EX numbering differs between documents.** The FT-991A CAT manual and the 2018 operating manual disagree [Y5][Y6]. Do not write EX items.

### 5.2 TCI: what must change before servers other than ExpertSDR3 can be claimed

The app's core premise, that muting leaves the stream alone, is a ColibriDDC measurement, not a property of TCI (MuteController.h:12-13). Before any other server is claimed:

1. **Silence guard** (section 3.3). deskHPSDR, and Thetis with "Mute will mute VAC 1 + TCI Receiver 1" ticked, would otherwise leave both programs silent indefinitely [T12][T7].
2. **Fix the two-argument mute report (a bug in today's code; phase 2).** `TciClient` takes `args.at(0)` as the mute flag (TciClient.cpp:196-197), and `parseBool` accepts "1" as true (TciProtocol.cpp:31-35). AetherSDR's init burst sends `mute:<trx>,<bool>;` for every receiver (AetherSDR TciProtocol.cpp:471-472), so the app reads the receiver number as the flag:
   - `mute:1,<anything>;` reads as muted, whatever the flag says. If that is the state the controller sees, `beginEngage` goes straight to Holding without taking ownership (MuteController.cpp:60-65). The app then reports the server muted while both programs play.
   - Every other receiver number reads as unmuted. The controller then sends `mute:true;`, which AetherSDR ignores (it needs two arguments, AetherSDR TciProtocol.cpp:1379-1404), and goes to Failed after 10 attempts.
   - So the outcome follows the receiver count, not the real mute state. The fix: with two or more arguments, read `args[1]` and take only the line for the selected `trx`. Add the AetherSDR burst with one, two and three receivers to the dialect mocks.
3. **A visible "this server's audio could not be muted" state**, replacing silent doubled audio:
   - With no `mute:` report after `ready;`, treat mute as unavailable. Quisk is an example [T14].
   - When the server ignores sets, the controller goes to Failed after 10 attempts today. Zeus and OpenX6X00 always report not muted, so they end there [T18]. So does AetherSDR once item 2 is fixed [T15].
4. **TX-aware dead-stream handling** (section 3.3; phase 2). On Thetis, the stream may carry TX monitor audio during MOX (likely) [T7]. Decide whether to gate that out.
5. **Do not identify servers by greeting.** Behaviour is detected, not looked up.
6. **Small fixes:**
   - Record the port with the mute ownership record, as section 3.5 describes (done in phase 2). The key stays the host string, so v0.1.1 debts survive and a downgrade still works.
   - Warn when neither an `audio_start` echo nor blocks arrive. piHPSDR silently ignores anything other than 48 kHz float32 [T11].
   - Show dropped blocks as "not available" when the count word is always 0 (Thetis, piHPSDR, AetherSDR, and any server older than 2.1) [T7].
   - Make the DIGL/DIGU left-copy an ExpertSDR-only behaviour, or an option. On other servers the pair is ordinary L/R, and deskHPSDR's RIGHT setting leaves left at zero [T12].
   - The UI wording that no longer says "ExpertSDR3" lands earlier, in phase 3.
7. **Dialect mocks.** Turn `MockExpert` into a set of dialects covering:
   - `start;` order
   - frames versus real samples in AUDIO_STREAM_SAMPLES
   - count word always zero
   - no echo when nothing changed
   - no mute line
   - two-argument mute, including AetherSDR's per-receiver burst (built in phase 2)
   - mute that zeroes the stream
   - stream that stops on TX
   - the legacy header
8. **Run the real server software against a simulated radio:** piHPSDR with `hpsdrsim` on macOS, Thetis with `hpsdrsim -g2` on Windows, and deskHPSDR [T11][T8]. Label the result "server tested with a simulated radio", which says nothing about the radio's own audio hardware. Never point these at ExpertSDR3's live port on this Mac.

Already robust, no change needed:

- Frames are derived from length and channels.
- The parser tolerates a missing final semicolon.
- A `start;` after `ready;` is harmless.
- Qt's capitalised `Sec-WebSocket-Key` satisfies Thetis's case-sensitive check [T9].
- Unknown rates are dropped rather than played at the wrong speed.

Not in the first claim:

- **ExpertSDR2 and other servers on TCI 1.8 or earlier.** They need legacy-header handling (format 4 as float32, channels 0 as stereo). That cannot be verified without a capture from the real software [T5].
- **wfview**, and AetherSDR with a networked Icom (sections 2.5 and 2.6).
- **Thetis versions after 2.10.3.15.** Its maintainer says TCI "is changing quite a bit for .16", and WSS is planned, while the app connects with `ws://` only [T8]. Claim specific Thetis versions only.

Recommended order for claims once the above exists:

1. ExpertSDR3 2.0 or later on other Expert Electronics hardware. `hpsdrsim` cannot stand in for SunSDR hardware, so this claim needs a SunSDR owner to run the section 7 checks. Without one, it waits.
2. Thetis 2.10.3.13 or later (including mi0bot), then piHPSDR. These can be tested against `hpsdrsim`.
3. deskHPSDR, sdr-for-linux, Quisk, NereusSDR, Lyra and Zeus follow, after a test each.

Discovery ports 40001, 50001 and 60001 cover every documented default found (TciDiscovery.cpp:95-98). Thetis and Quisk bind loopback by default, so they are found only on the same computer [T7][T14].

### 5.3 FlexAPI

**What it adds for a Flex owner, plainly:**

- **Nothing**, if they will run AetherSDR as their operating program. AetherSDR is already a Flex client with AetherRX built in, on the same platforms.
- It helps only an owner who stays on SmartSDR for Windows or iOS, Maestro or an M-model front panel, and wants AetherRX on that slice. It also helps one who wants a small headless Pi as a speaker. AetherSDR is always its own GUI station with its own slices, so it does neither.

That is a small audience. Build this path last, label it experimental, and start it only when the testers the exit needs have agreed (section 6, phase 8).

**Status of this section.** It is research pinned to FlexLib 4.3.16 and firmware 4.3.16 observations, not a specification. Re-check it against current FlexLib and firmware when phase 8 starts.

**How audio is obtained.** Connect as a **non-GUI client**, the role FlexRadio's own DAX and CAT programs take [F3][F20]. The sequence follows FlexLib [F20], with nDAX as a cross-check [F15]:

1. TCP 4992. The radio sends `V<version>` and `H<handle>`. Commands go as `C<seq>|cmd`, and each gets one reply with hex status 0 for success [F5].
2. `client program <name>`. The wiki says "not yet implemented; do not use", but FlexLib and AetherSDR send it, and FlexLib is followed here [F6][F20].
3. `sub client all`, to learn the operator's station and `client_id`, which discovery does not carry [F20].
4. `client bind client_id=<id>`. Always send it. The wiki says it does nothing since v3.0, while the user guide says DAX streams follow the bound station [F6][F3].
5. `sub slice all` and `sub radio all`.
6. The MTU command FlexLib sends (Radio.cs:2296-2316), then `client set send_reduced_bw_dax=1`, then `client udpport <port>` [F20]. The port must be free. Re-issuing it after streaming starts has undefined behaviour [F6].
7. `keepalive enable`, then `ping` once a second. The radio disconnects a client after 15 s without one [F10][A20].
8. Pick the slice by station plus `index_letter`. `slice set` itself takes the numeric index [F20][F15].
9. If the slice has no DAX channel, `slice set <N> dax=<CH>` [F7].
10. `stream create type=dax_rx dax_channel=<CH>` [F8].
11. Optionally `audio stream 0x<id> slice <N> gain <0-100>`, which is not in the wiki. FlexLib defaults to 50 [F20][F15].

**`dax audio set` is deliberately left out.** nDAX sends `dax audio set <ch> slice=<idx>` between `slice set ... dax=` and `stream create` (nDAX main.go:150-157) [F15]. FlexLib 4.3.16 never sends it [F20]. The wiki documents it with an optional `tx=<1|0>` that makes the sender the DAX transmit source [F21]. The plan follows FlexLib. If some firmware turns out to need it, allow only the form with `slice=` and no `tx=`. FakeFlexRadio tests both orders, and the Flex checklist asks whether the stream starts without it.

**Why non-GUI.** From v4.3.16, "A GUI client can only change its own slices... Non-GUI clients are not restricted" [F7]. A second GUI client would also probably take one of the two multiFLEX slots, and gets its own panadapter (measured on a FLEX-8600) [F3][A20]. Whether a non-GUI client is free of `licensed_clients` accounting is **unverified**.

**Why DAX and not `remote_audio_rx`:**

- DAX is per slice. AetherSDR relies on it being unaffected by slice mute [A16].
- `remote_audio_rx` is a post-mute mix: `audio_mute=1` zeroes its payload [A17].
- Creating `remote_audio_rx` can silence the radio's hardware outputs radio-wide, through `mute_local_audio_when_remote`, which firmware defaults to 1 [A18][F3].
- Opus exists only on `remote_audio_rx`, so this path needs no Opus dependency [F8].

**Audio format.** VITA-49 ExtDataWithStream with a 28-byte header [F20][F14]:

| Class code | Format |
|---|---|
| 0x03E3, 0x03E4, 0x03E5 | float32 at 24, 48, 96 kHz, big-endian interleaved stereo |
| 0x0123, 0x0124, 0x0125 | int16 at 24, 48, 96 kHz, big-endian mono |

FlexLib 4.3.16 warns: "consumers must resample from this rather than assume 24 kHz" [F20]. Read `sample_rate` from slice status. Decode by class code. Accept 24 and 48 kHz, and decimate or refuse 96 kHz. Existing clients send 128 frames per packet (nDAX, Aether-gate; not a FlexRadio document) [F15][F19]. The 4-bit packet count gives gap detection [F20].

**How it coexists with SmartSDR:**

- Never steal a DAX channel. Reuse the slice's channel if it has one; the guide says several clients may share a channel [F3]. Otherwise take a free one, and clear it on stop. Setting `dax=` removes the channel from that client's other slices [F7].
- FLEX-6300 and 6400 have only two DAX channels [F3]. When none is free, show a clear error.
- Firmware 4.2.18 and later auto-binds and reports `slice=` in stream status. On older firmware, send one `slice set dax=` nudge at most, per stream [A31].
- If the operator changes the slice or channel, follow it. On `in_use=0` or client disconnect, stop and wait.
- TX state comes from interlock `state=TRANSMITTING` with `tx_client_handle`. The radio never sends `mox=` [F12][A31]. In simplex, all slices are muted while transmitting [F3].

**How silencing works.**

- Send `slice set <N> audio_mute=1` on the chosen slice only, and restore the state seen beforehand [F7].
- Never write `audio_level`, any `mixer ...`, or `radio set mute_local_audio_when_remote`. Those act radio-wide, or would need restores of their own [F9][F20].
- Whether slice mute also silences line out, headphones and the M-model front speaker is implied by the user guide but not stated (likely) [F3]. Other slices stay audible and unprocessed. The UI says so.
- The radio does not restore mute after a crash [A19]. Persist ownership keyed by radio serial, `client_id` and slice letter, together with whether this app assigned the DAX channel. Document the manual fix: click unmute in SmartSDR.

**Command allowlist**, enforced in the sender and tested:

- `client program`, `client bind`, `client udpport`, `client set` (MTU and DAX bandwidth)
- `keepalive enable`, `ping`, `sub ...`
- `slice set N audio_mute=`, `slice set N dax=`
- `stream create type=dax_rx`, `stream remove`
- `audio stream ... gain`

Never `xmit`, `transmit`, `slice tune`, `client disconnect`, `mixer`, `radio set`, or `dax audio set` (see above).

**Network details.**

- The radio's VITA source port was measured as 4993 on firmware 4.3.16; the wiki says 4991. AetherSDR sends a 1-byte keepalive there every 5 s so that stateful firewalls keep the return path open [A15][F13].
- Use `NoProxy` on the TCP socket, because `QTcpSocket` honours the application proxy [A15].
- On macOS, broadcast reception needs the Local Network grant. The first attempt may be denied while the alert is up, so retry, as AetherSDR does [P17][A32].

**Vendoring.**

- Take `CommandParser` only (section 3.2). Write a new small `FlexClient`, `FlexDiscovery` and `VitaDecoder`. Rough estimate: 1,000 to 1,500 lines plus a similar amount of tests.
- AetherSDR's handshake lives in the 14,192-line `RadioModel.cpp` and registers as `client gui`, so it is not reusable [A15].
- FlexLib source is "All Rights Reserved" .NET. Read it as a reference and do not copy from it. That is what AetherSDR states it does [F20][A14].
- FlexRadio's site says "A software license is required for commercial applications", and the full terms were not found [F2]. Ask FlexRadio in writing (section 10, item 9).

## 6. Phases

Rules that hold for every phase:

- `radio/backend` absent means TCI, and settings keys are only added, never renamed.
- Each exit includes the full existing suite and a live ColibriDDC regression run that follows the live-radio safety notes:
  - `--settings <file>`
  - an explicit `--port`
  - `--no-mute` unless mute is what is being tested
  - read `mute` and `volume` first, and restore exactly those
- **Every merge to `main` that touches non-docs files publishes a signed public release at once** (.github/workflows/release.yml:4-7). So:
  - Unfinished backends must not appear in the UI until their phase exits.
  - Phase 1 is not merged on its own. It merges together with phase 2, in one PR or after phase 2 is ready on the same branch.
  - **Tester builds need a workflow change first.** Today a `workflow_dispatch` run accepts only a plain `X.Y.Z`, creates a public `vX.Y.Z` tag at the run's commit, and publishes a non-draft release (release.yml:17-27, 126-131, 255). The push-to-main bump starts from the highest clean tag, so each tester build uses up a real version number and moves `main`'s next version (release.yml:142). Before phase 3, change `release.yml` to allow `-rc` tags or draft releases, and prove a branch run once. That change is itself a non-docs merge, so ship it with the phase 1 and 2 merge.
  - From phase 4 on, tester builds carry CAT code that writes to the radio at open. Make them drafts or share them privately, never public releases.
  - **The minor bump for phase 3.** `BUMP` is a workflow env (release.yml:40). Setting it to `minor` and back takes two more non-docs merges, and each publishes a release. A manual tag pushed after the merge races the automatic run, which skips only if the merge commit is already tagged when it checks (release.yml:137). Decide the mechanism with the workflow change above, and test it on a branch.

Size is relative: S < M < L.

**Who gets value first:**

- Today's ExpertSDR3 users gain in **phases 1 and 2**, released together: the mute-report fix, transmit-aware handling for ExpertSDR3 transceivers, and output-loss detection. Those fixes are tested only against mocks, because no transceiver is available (section 7).
- An owner of only one Icom, Kenwood or Yaesu USB radio first gains in **phase 3**: processed audio, with AF turned down by hand. Phases 4 and 5 add CAT and automatic mute for tested models, and only if no other program holds the CAT port (section 5.1, "Port sharing"). Otherwise that waits for phase 6.
- An owner of only a Thetis, piHPSDR or other non-Expert TCI radio first gains a supported claim in **phase 7**. Phase 7 needs the silence guard, which is built in whichever of phases 5 and 7 comes first.
- A Flex owner first gains in **phase 8**.

| # | Phase | Scope | Main files | Exit criteria (checkable) | Size |
|---|---|---|---|---|---|
| 1 | Radio seam, TCI only | Section 3.1: the interface, an idle backend before the first connect, `EngineStatus` on `Link` with its existing fields kept, the receiver list from `identity()`, the mute key through `muteKeyResolved`. No behaviour change. Not merged on its own (rules above) | new `src/radio/RadioBackend.h`, `src/radio/TciBackend.*`; `src/app/Engine.*`, `src/main.cpp`, `src/gui/MainWindow.*`, `src/gui/RadioDiscoveryDialog.*`, `src/dsp/NrProcessor.h`, `src/dsp/NrRegistry.cpp`, `CMakeLists.txt` | ctest green on all four CI platforms, with test changes limited to type renames. `neverSendsVolume`, `aMuteOwedBeforeUpgradingIsStillRestored` and `connectingDuringADisconnectEndsUpConnected` unchanged. `grep -rn "TciClient\|tci::" src` matches only in `src/tci` and `src/radio/TciBackend.*`. A v0.1.1 settings file loads with identical keys. Headless output and SUMMARY JSON keep every existing key. Live ColibriDDC run: mute engages and restores | M |
| 2 | Fixes to today's TCI path | Two-argument `mute:` reports parsed by `trx` (section 5.2, item 2). Transmit-aware dead stream, backend-neutral through `reportsTransmit` (section 3.3). Output liveness by callback progress, not `Pa_IsStreamActive`. `MuteController` moved to `src/radio/` with an opaque `radioKey` (TCI value unchanged) and the added `/port` value (section 3.5). The `-rc` or draft release path (rules above) | `src/tci/TciClient.cpp`, `src/radio/MuteController.*` (moved), `src/app/Engine.cpp`, `src/app/Settings.*`, `src/audio/PortAudioSink.cpp`, `.github/workflows/release.yml`, `tests/test_mute_controller.cpp`, `tests/test_engine_e2e.cpp`, `tests/MockExpert.*` | New tests pass for: AetherSDR's burst with one, two and three receivers (never Holding without ownership); no `mute:false` while `trx:true`; a sink whose callbacks stop is treated as dead and the mute released; a v0.1.1 record without `/port` is restored; a record whose `/port` differs is not. Existing suites green. Live ColibriDDC regression. A branch tester build that consumes no release version | S to M |
| 3 | USB audio capture, audio only | `AudioSource`, `PortAudioSource`, `CaptureBuffer`, `UsbBackend` with `SpeakerControl::None`. `UsbModels` with wave-1 rows and channel maps only. Input enumeration on the engine thread. Picker USB section (listing only). Output-list exclusion by physical device. No default-device fallback. Channel select, rate conversion, input trim. A per-backend connection box (no Host and Port for USB) and backend-neutral wording for every status and mute string that says "ExpertSDR3" today (MainWindow.cpp:172-245, 965-1015; Info.plist.in:17-18). The mute control hidden while `SpeakerControl` is None. With the mode Unknown, a manual "not speech" selector, and a warning when NR is on. macOS entitlement and microphone permission, `--list-inputs`, `--capture-device`. No serial and no Hamlib yet | `src/audio/*` (new files), `src/radio/UsbBackend.*`, `src/radio/UsbModels.*`, `src/gui/RadioDiscoveryDialog.*`, `src/gui/MainWindow.cpp`, `src/app/Engine.cpp`, `src/app/Settings.*`, `packaging/Info.plist.in`, `packaging/macos/*.entitlements`, `scripts/package-macos.sh`, `scripts/smoke-test.sh`, `tests/test_capture.cpp`, `tests/test_drift.cpp` | CI: `test_capture` (rates, channel map, overflow counted); a joint CaptureBuffer-to-PlaybackBuffer run of at least 1800 s simulated with independent clocks at ±500 ppm and callbacks up to 1024 frames, with no underrun; a callback larger than 1024 frames raises the target and warns; `test_drift` covers 1024-frame device buffers; on signed builds only, a static check fails the release if the entitlement or usage string is missing; `--list-inputs` passes in the smoke test of every package; a UI test shows the capture device cannot be chosen as output. Hand: on a signed tester build, capture from any non-radio USB input, with the microphone prompt appearing, and the denial path showing advice. On Windows, the output exclusion with a real USB codec. May ship without a radio tester, labelled "experimental, not tested with a radio" (section 10, item 2) | L |
| 4a | Hamlib Windows spike | Link Hamlib in the ClangCL job: import lib from `.def`, vcpkg pthreads, dummy rig open and close. Can start any time | `CMakeLists.txt`, `.github/workflows/build.yml` | The Windows CI job opens and closes `RIG_MODEL_DUMMY` and reads a level. If it fails, phase 4 ships with `ENABLE_HAMLIB=OFF` on Windows, where USB stays audio-only | S |
| 4 | CAT state reads through Hamlib (Hamlib itself writes AI, and EX on four Yaesu models, at open) | **Entry:** section 10 items 0, 4, 5 and 6 decided, and at least one USB tester committed. `RigControl`, `HamlibRig` (worker thread), `MockRig`. Hamlib configured before open as in section 4.3 (no poll thread, no cache, multicast off, no auto power-on). CAT fields in `UsbModels` and `catVerified` per model and OS; without it, CAT stays hidden and the model stays audio-only. FT-991, FT-991A, FTDX10 and FTDX3000 stay audio-only until item 4 is decided. Explicit DTR and RTS, identify-and-refuse, frequency, mode with DATA flag mapped to `ModeClass`, TX state, picker Test button, CAT trace option (Hamlib debug log to a file), dialout and driver messages. Hamlib pinned and built static on macOS and Linux; Windows DLL. Models 2 and 4 not offered yet | `src/radio/RigControl.h`, `src/radio/HamlibRig.*`, `src/radio/UsbModels.*`, `CMakeLists.txt` (`ENABLE_HAMLIB`, `Qt6::SerialPort`), `.github/workflows/build.yml`, `scripts/package-*.{sh,ps1}`, `README.md` | CI: MockRig tests for NR bypass on DATA and CW modes and for TX gating. Hamlib dummy (model 1) smoke on all four platforms, asserting the section 4.3 configuration after open. On Linux and macOS, PTY-driven `simts590`, `simts890` and `simft710` read frequency and mode. A test shows `RigControl` has no transmit verb, and a CI check fails if any file under `src/` calls `rig_set_ptt`, `rig_send_raw` or another Hamlib transmit function (section 4.3 says how a future read allowlist would be scoped). Static linking puts those symbols in the binary, so check the source and `nm -u` on `HamlibRig`'s object file, not the final binary. Hand: **minimum exit is one model of one brand with `catVerified` on one OS** (section 7). Other models stay audio-only | L |
| 5 | Speaker mute through AF, per model | Level-capable `MuteController` (section 3.3: `restoreLevel`, level reports, the Level override rule). `SpeakerControl::Level` in `HamlibRig`: read, persist, set 0, verify, poll, override detection, restore, crash recovery, and no Kenwood AF write before an AF read. The silence guard, including the noise-floor and squelch cases, unless phase 7 built it first. Main receiver only. A "radio volume is at zero" banner and a "Restore radio volume now" action. `muteVerified` per model | `src/radio/HamlibRig.*`, `src/radio/UsbModels.*`, `src/radio/MuteController.*`, `src/app/Engine.cpp`, `src/gui/MainWindow.cpp`, tests | CI: controller tests (level persisted before set, level 0 counted as muted, restore or clear driven by an injected level report, a differing read after an acknowledged set goes to OperatorOverride). MockRig and PTY tests: stale read, knob turned, restore refused during TX keeps ownership, both crash cases, Kenwood write refused before a read, exact restore for all 256 levels through Hamlib's float path, a level drop to a non-zero noise floor trips the guard. Hand: **minimum exit is one model with `muteVerified`**. Until then the phase 5 code ships switched off. Guard thresholds measured on at least one real USB radio before any opt-in (section 10, item 3). Release notes list exactly the verified models | M, plus S per model |
| 6 | Sharing the radio | Hamlib model 2 (rigctld or wfview rigctld) and model 4 (flrig, no mute) from a patched Hamlib build; loopback by default, with a warning that network mode trusts the server; guidance for second CAT ports (IC-7300MK2, IC-7760, FT-710, FTX-1). Can follow phase 4 directly once the patched Hamlib exists | `src/radio/HamlibRig.*`, `src/gui/RadioDiscoveryDialog.*` | CI: `rigctld -m 1` on loopback driven through model 2; a mock XML-RPC flrig. The build provenance shows a Hamlib commit that includes the fixes for the advisories in section 4.3. Hand: one radio shared with WSJT-X | S |
| 7 | TCI beyond ExpertSDR3 | Section 5.2, items 1, 3 and 5 to 8: the silence guard applied to TCI (built here if phase 5 has not), the "could not mute" and "mute unavailable" states, the small fixes, the dialect mocks and the simulated-radio runs | `src/radio/TciBackend.*`, `src/tci/*`, `tests/MockExpert.*`, `src/app/Settings.cpp`, `src/gui/MainWindow.cpp`, `README.md` | Dialect tests green. A recorded run per claimed server against `hpsdrsim`. README support table uses the three labels in section 7 | S to M |
| 8 | FlexAPI (optional) | Section 5.3, re-checked at kickoff. **Gated on the testers the exit needs**: one FLEX-6000 owner and one FLEX-8000 or Aurora owner. With only one, claim that family only | `src/radio/FlexBackend.*`, `src/radio/FlexClient.*`, `src/radio/VitaDecoder.*`, `src/radio/FlexDiscovery.*`, vendored `CommandParser`, `scripts/sync-aether.sh`, `tests/FakeFlexRadio.*` | CI: VITA decode for all six class codes, including trailer, truncation and gaps; FakeFlexRadio covers handshake order (with and without `dax audio set`), mute and restore, crash recovery, DAX channel rules and the allowlist. Hand: the checklist in section 7 on each claimed family | L |
| R | Rename (optional) | Section 9. Any time after the name is decided; preferably before phase 3 ships, so the new microphone prompt carries the final name | section 9 | Guard tests for frozen identifiers green; upgrade tests in section 9 green | M |

## 7. Testing

**In CI, without hardware.**

- All existing suites, after every phase.
- `MockRig` at the `RigControl` seam for all mute and state logic, in the `MockExpert` pattern.
- Hamlib dummy rig (model 1) in-process on all four platforms. It stores and returns levels with no port [H23].
- `rigctld -m 1` on loopback for model 2.
- Hamlib simulators over a PTY on Linux and macOS. `simts590`, `simts890`, `simts990` and `simft710` handle AG. `simic7300`'s level handler does not cover AF (0x14 0x01), and `simft991` and `simftdx101` have no AG handler [H23]. Windows simulators would need a virtual COM driver, so skip them. A Kenwood simulator also checks that no AF set is sent before an AF read (section 4.3).
- `CaptureBuffer` and `PlaybackBuffer` with independent simulated clocks.
- A null capture device for the smoke test.
- Static packaging checks for the entitlement, `Info.plist` and package dependencies.
- `FakeFlexRadio` (phase 8).
- TCI dialect mocks (phase 7).

**Caveat.** Unlike `MockExpert`, which reproduces measured behaviour, these mocks reflect manuals only. Label them that way, and correct them from CAT traces as testers report. macOS CI runners are VMs, so capture permission cannot be proven there.

**What CI cannot prove:**

- that AF 0 leaves USB audio flowing
- that a restore is exact on a real radio
- that opening a port does not key a radio
- that slice mute leaves DAX intact

**Hardware the developer has:** an ExpertSDR3 ColibriDDC, which is receive-only. Nothing else is assumed. Phase 3 can ship without a radio tester. Phases 4 and 8 start only with committed testers. Phase 5 code ships switched off until a model has `muteVerified`. Each phase's minimum exit is stated below.

| Phase | Must be tested by hand before it ships | Available now |
|---|---|---|
| 1, 2 | ColibriDDC with ExpertSDR3 2.1: mute engages and restores, an owed v0.1.1 record is restored, unplugging the output gives the mute back | Yes |
| 2 (TX-aware part) | A TCI transceiver, such as a SunSDR2 or a Thetis radio, keyed into a dummy load | **No.** Otherwise ships tested only against the mock, and says so |
| 3 | A signed macOS build with any non-radio USB input, checking the prompt and the denial path. On Windows, the output exclusion with a real USB codec. Wanted, not required: one radio per brand on each OS (Windows, macOS, Linux, Pi): IC-7300, TS-590SG, FT-991A or FTDX101. Check device names, channels and rates | Non-radio input: owner to confirm. **Radios: no** |
| 4 | Required per model and OS for `catVerified`: identify, frequency, mode, DATA flag and TX state; a Hamlib trace from open to close; a DTR/RTS keying test into a dummy load with the operator present. Minimum to exit: one model, one OS. Wanted: the phase 3 radios, a newer Icom (IC-705 or IC-7300MK2), a TS-890S and an FT-710, on each OS | **No** |
| 5 | Every model that gets `muteVerified`: the bench script below. Minimum to exit: one model. Also, on at least one real USB radio, the silence-guard thresholds | **No** |
| 6 | One radio shared with WSJT-X through rigctld | **No** |
| 7 | piHPSDR with `hpsdrsim` (macOS, possible here), Thetis with `hpsdrsim` (needs a Windows PC), deskHPSDR | piHPSDR: yes. Thetis: **no Windows PC** |
| 8 | One FLEX-6000 and one FLEX-8000 or Aurora owner, running the checklist and returning the command log. With one owner, only that family is claimed | **No** |

**USB bench script, per model.** Use a dummy load wherever transmit is possible, with the operator present. Record every number.

1. Fixed input signal. Measure the app's input RMS with AF at its prior value, then at 0. Pass if the two are equal within the measurement's own spread.
2. While the app holds the mute, turn the AF knob. Record what the AF read returns and whether the app reports an operator override. On IC-7300MK2 and IC-7760, also move RF and SQL.
3. Power-cycle the radio while the mute is held. Record AF after power-on, and what the app does on reconnect.
4. With AF at 0: are beeps, voice, CW sidetone and monitor audible? What does the USB stream carry during transmit?
5. With USB SEND or keying mapped to DTR, then to RTS: open and close the port through the app on each OS. Was there any transmit blip?
6. Hamlib trace from open to close. List every write and compare it with section 4.3.
7. Dual receivers: L/R mapping with Main and Sub on separate frequencies.
8. Device names, VID:PID, channel count and sample rates on each OS.
9. Silence-guard thresholds: the input level with no signal at AF prior, at AF 0, and with the squelch closed in FM. Does the stream go to zeros or to a noise floor?

**Flex checklist.** Covers:

- DAX level with `audio_mute` 0 and 1
- which outputs slice mute silences
- multiFLEX accounting with SmartSDR connected
- crash recovery, including whether the radio removes the dead `dax_rx` stream
- the 4993 keepalive
- the `sample_rate` reported
- DAX gain 50 relative to full scale
- whether the DAX stream starts without `dax audio set` (section 5.3)

**One set of labels everywhere**, in release notes and in the README support tables:

- "tested with this app on real hardware" (with the model, and for USB the OS)
- "tested with a simulated radio" (TCI servers only)
- "documented only, not tested" (from the manufacturer's or server's own documents)

## 8. Packaging by OS

**macOS** (Apple Silicon, Developer ID, notarized):

- Add `NSMicrophoneUsageDescription` to `packaging/Info.plist.in`. Today only `NSLocalNetworkUsageDescription` exists, at :17, and its text names ExpertSDR3. Widen that text for Flex discovery [P5][P17].
- Add `packaging/macos/<App>.entitlements` containing only `com.apple.security.device.audio-input`. Pass `--entitlements` at the app signing step only (scripts/package-macos.sh:134); frameworks inherit [P5].
- After signing, a static check fails the release if the entitlement or the usage string is missing. Without both, "the system terminates your app" [P5]. Apply the entitlement check to signed builds only: unsigned CI builds take the ad-hoc branch, which has no hardened runtime (scripts/package-macos.sh:136-138).
- Phase 4: `install-qt-action` modules become `qtwebsockets qtserialport` in the macOS job (build.yml:47). `macdeployqt` and the framework signing loop are then expected to cover `QtSerialPort.framework` (package-macos.sh:50, 125-133; not run).
- Request microphone permission only when connecting to a capture backend, so TCI users never see a prompt. Do it from the main thread, before the engine connects. `QMicrophonePermission` is Qt 6.5 or later and GUI-only, while Linux builds Qt 6.4.2 [P7]. Guard it with `Q_OS_MACOS` and `QT_VERSION`, or use a small AVCaptureDevice `.mm` file.
- `--headless` cannot request permission and relies on silence detection plus advice.
- A denial may deliver zeros rather than an error. This has community reports only and must be measured on a signed build [P8].
- No entitlement is expected for serial ports, because the app is not sandboxed. That is an inference, not tested [P5].
- Whether CP2105 needs the Silicon Labs driver on current macOS is **unverified**. One 2023 report says yes [Y16].
- Keep the bundle id `org.vu3esv.ExpertSDR3Audio` (CMakeLists.txt:268). Local Network privacy tracks the app by its code signature [P17]. Keeping the bundle id and team is expected to keep the grants, but that is not tested.
- Hamlib is linked static, so there is no extra dylib to sign.

**Windows:**

- `install-qt-action` modules become `qtwebsockets qtserialport` in the Windows job (build.yml:144) [P9].
- Ship `libhamlib-4.dll` with `libgcc_s_seh-1.dll` and `libwinpthread-1.dll`, plus `libusb-1.0.dll` if the build links it. Do not ship `rigctld.exe` [H11].
- `windeployqt` is expected to copy `Qt6SerialPort.dll` (not run).
- Do not bundle radio drivers. Link the Icom (two packages), Kenwood and Yaesu (Silicon Labs CP210x) pages from the picker, and say the driver must be installed before plugging in [I20][I21][K1][Y21].
- Capture: default to WASAPI with `paWinWasapiAutoConvert`. Shared mode cannot go below 20 ms latency [P3].
- "Let desktop apps access your microphone" is one switch for all desktop apps. What capture gets when it is off was not found [P12].
- Flex: the installer is unelevated by default (`PrivilegesRequired=lowest`, installer.iss:22), so by default it adds no firewall rule. `PrivilegesRequiredOverridesAllowed=dialog` (installer.iss:23) does offer an elevated all-users install. Document the firewall prompt. Dismissing it can leave a block rule that beats an allow [A30].

**Linux** (Debian 12 and 13, Ubuntu 24.04 and 26.04, x86-64 and Pi arm64):

- Build dependency: `qt6-serialport-dev` (build.yml:231-234). Runtime dependency: plain `libqt6serialport6`, with no t64 alternative, added at CMakeLists.txt:302 [P9].
- Hamlib: fetch pinned source with a checksum and build static, with libusb, readline, xml, INDI, C++ and language bindings off. A branch commit needs `./bootstrap` plus autoconf, automake and libtool [H10].
- The AppImage should bundle `libudev.so.1`, since it is not on the excludelist (likely) [P10].
- On a QSerialPort or Hamlib permission error, show `sudo usermod -aG dialout $USER` and say to log in again. A .deb cannot add the user, and an AppImage has no install step [P11].
- Document the port grabbers: brltty has been reported claiming CP210x devices, and ModemManager probes TTYs [P11].
- Capture on PipeWire desktops and Raspberry Pi OS through PortAudio v19.7.0's ALSA host is **untested** [P1].
- Flex: document `sudo ufw allow 4992/udp` [A30].

**All platforms:**

- `ENABLE_HAMLIB` is a CMake option, in the style of `ENABLE_NR4`, so the TCI-only build survives a Hamlib breakage.
- Add a project-level third-party notices file. The repo has none today: `vendor/aethersdr/THIRD_PARTY_LICENSES` is upstream's copy, and `scripts/sync-aether.sh` overwrites it (sync-aether.sh:47). The new file and the About box list Hamlib (LGPL-2.1-or-later, plus the `libsecurity` files and their terms from section 4.3) and, in phase 8, AetherSDR's `CommandParser`.

## 9. Rename

**Recommendation.**

- Renaming away from "ExpertSDR3 Audio" is sensible. That name is wrong once other radios are supported, and it borrows Expert Electronics' product name. It is cheap now: on 2026-11-07 the two releases had 23 downloads in total, and the repo had 0 stars and 0 forks.
- **Do not use "AetherAudio" unless the AetherSDR maintainer agrees in public first.** No registration or naming policy was found, so this is a judgement about confusion, not a found legal bar. AetherSDR names its own parts Aether plus a noun: AetherRX, AetherTX, AetherVoice, AetherModem, AetherLab, AetherSweep, AetherMap, AetherCTR2, AetherSerialBridge [A24]. It calls its client-side audio processing "Aetherial Audio", which is nearly the same name for the same function [A25]. This tool ships the same DSP, and a Flex client under an Aether name would overlap AetherSDR directly. A collaborator has said it is "not a goal of this project to clone FlexRadio's design language or brand identity" [A26], which bears on any AetherRX look-alike.
- The maintainers seem approachable. Aether-gate is a third-party "Aether" project that AetherSDR lists and forked into its own org [A29].
- **Ask** in a public AetherSDR Discussion, or by email to kk7gwy@aethersdr.com, giving a date. Silence counts as no. Ask about the look as well as the name, and whether they would list the tool [A24].
- **If there is no explicit yes:** a neutral name, ranked by the collision evidence below: RigHush first (no `.com` match and no same-niche tool found), then BandClear (no `.com` match, but it sits near ClearQSO), then QuietRig (`quietrig.com` is registered by someone else). All three are preliminary until a trademark search is done by hand. "Rig" and "band" are ham words and name no maker. The final choice is the user's.
- **Whatever the name,** add a non-affiliation notice to the README and About box: it uses AetherSDR's AetherRX chain under GPL-3.0, and is not affiliated with or endorsed by AetherSDR, Expert Electronics, FlexRadio, Icom, Kenwood or Yaesu. Nothing like it exists today.

**Collisions found** [R6]:

- GitHub has two repos named AetherAudio, both with 0 stars.
- "Aether" is crowded in audio: 2CAudio and LX Audiolabs plugins, Umbral Audio, AETHER Spectral Restorer, Kiwi Ears and others, plus the near-spelled Ather Audio.
- In ham radio, "Aether" is already a macOS logging app, and Debian carries an `aethersdr` package.
- `aetheraudio.com` is for sale.
- No AETHER AUDIO trademark was surfaced, but many AETHER word marks were. **No official trademark search was completed:** the scripted USPTO query was blocked, and aggregator pages returned 403. The Microsoft Store was not checked.
- QuietRig: `quietrig.com` has been registered since 2025-06-18, with no site. BandClear and RigHush: `.com` WHOIS showed no match. BandClear sits near ClearQSO, a same-niche Windows tool [R6].
- Rejected names: SolidCopy, R5 Audio, ClearRig, RigClarity, RigVoice, CleanCopy and SoftCopy, all because of existing uses [R6].

**Migration: display-only rename with frozen stored identifiers.**

| Freeze forever | Why |
|---|---|
| QSettings org and app `ExpertSDR3Audio` (main.cpp:278-279, Settings.cpp:44) | Changing them orphans settings, the profile library path and owed mute records. A lost record leaves the radio silently muted |
| Profile format string `ExpertSDR3Audio.profiles` (Profiles.cpp:25) | A mismatch makes `load()` move each user's library aside as `.unreadable-<timestamp>` |
| Stored keys, including `output/muteExpert` and `muteOwnership/*` | Persisted |
| Bundle id `org.vu3esv.ExpertSDR3Audio` | Privacy grants: Local Network now, Microphone from phase 3 [P17] |
| Inno `AppId` (installer.iss:4) | A new AppId creates a second install instead of an upgrade [R3] |

**Change:**

- `setApplicationDisplayName` (not called today) [R1]
- `CFBundleName`
- target and binary name. Headless setups script the binary name, and each platform needs its own answer:
  - Linux: a compatibility symlink under the old name, kept for several releases.
  - Windows: `[InstallDelete]` removes `ExpertSDR3Audio.exe`, and the unelevated installer cannot rely on symlinks. Ship a small forwarding exe under the old name, or state the new path in the upgrade notes.
  - macOS: the `.app` path itself changes and users delete the old app. State the new path in the upgrade notes.
- artifact and DMG names in release.yml, build.yml, the three packaging scripts and RELEASING.md
- the .deb name, with versioned Provides, Conflicts and Replaces on `expertsdr3-audio` [R4]
- icons, About text, URLs and docs

Text about ExpertSDR3 as a TCI server keeps saying ExpertSDR3.

**Order:**

1. Choose the name. Ask the maintainer if keeping any Aether name. Run official searches by hand.
2. In the rename branch, add one header of frozen identifiers, with guard tests that fail if any of them changes. The profile format string is one of them, so the profile reader needs no change.
3. Rename the GitHub repo while no workflow is running. Then check `gh secret list`, a manual `ci.yml` run and the old-URL redirect, and update the local remote. Never create a repo under the old name [R2]. Do not rename the local folder casually; local tooling state is keyed to its path.
4. Rename PR: display names, artifacts, URLs, the Linux package fields and symlink, the Windows forwarding exe if chosen, and Windows `[InstallDelete]` for the old exe and shortcuts [R3].
5. Upgrade tests from v0.1.0 and v0.1.1 with seeded settings, `profiles.json` and an owed mute record:
   - Linux: `apt install ./new.deb` on each CI distro. Whether this removes the old package is **untested** [R4].
   - Windows: silent install of the old version, then the new.
   - macOS: seeded native store.
   - Extend `aMuteOwedBeforeUpgradingIsStillRestored`.
6. Cut a tester build from the branch (section 6 rules), then check on a real Mac, in this order: grant the old app Local Network access (and Microphone access, if the rename lands after phase 3); quit it; delete it; install the new app; launch it for the first time; then check Gatekeeper, settings, profiles, and whether each grant carried over. Local Network privacy uses the main executable's UUID, and Apple warns of "unexpected behavior" with "multiple versions of the same app installed" (FB15568200) [P17]. So two copies must never be installed at once during this test.
7. Merge, which auto-publishes, with upgrade notes that give the same order: quit, delete the old app, install, launch, re-grant if asked. The notes also give the new binary path for scripts on Windows and macOS.
8. Keep the symlink and Provides for several releases. Keep "formerly ExpertSDR3 Audio" in the README. Document that v0.1.1 attestations verify under the old repo name (inferred from gh's source) [R5].

## 10. Risks and open decisions

Each needs the user's call. Items 0 and 4 to 6 must be settled before phase 4 merges, because that merge is the first public release that links Hamlib.

0. **Adopt Hamlib at all.**
   - Recommendation: yes, linked in-process behind `RigControl` (section 4). It brings LGPL notices, files with "freeware" terms, open security advisories with no patched release, and writes to the radio at open. Items 4 to 6 are its conditions.
   - If no: native per-family drivers, each needing hardware to find its quirks (section 4.4).
1. **Name.**
   - Recommendation: ask the AetherSDR maintainer before any Aether name. Otherwise RigHush, BandClear or QuietRig, ranked by collisions found (section 9). Rename display-only.
2. **Hardware and testers.** This is the one place that settles when a tester is required.
   - Phase 3 may ship without a radio tester, labelled "experimental, not tested with a radio". It writes nothing to the radio.
   - Phase 4 starts only once at least one USB tester has committed, and phase 8 only once its Flex testers have. Phase 5 code ships switched off until at least one model has `muteVerified` (section 6).
   - Recommendation: recruit at least one owner per brand (IC-7300, TS-590SG or TS-890S, FT-991A or FTDX101) while phase 3 is built, and two Flex owners from different families before phase 8 starts.
3. **Mute on models without a bench record.**
   - Recommendation: off by default, with the operator turning AF down. Allow an explicit, per-model "experimental" opt-in only where the manufacturer's text or a checked schematic supports it: IC-7300, IC-7100, IC-9100, IC-7410, IC-7600, IC-R8600, FTDX101, FT-991A. Add IC-9700 only once its schematic reading is re-checked.
   - Offer no opt-in until the silence-guard thresholds have been measured on at least one real USB radio. The silence guard stays on regardless.
4. **Hamlib's own writes at open:** the Yaesu CAT TOT EX item (exact strings per model in section 4.3), and AI off and restore on Kenwood and Yaesu.
   - Recommendation: accept them for v1, disclose them per model in setup text, and raise an upstream request for an option to skip the EX write. Until this is decided, FT-991, FT-991A, FTDX10 and FTDX3000 stay audio-only: no CAT fields in the model table.
   - If unacceptable, write a native Yaesu driver behind `RigControl`. Yaesu is the cheapest family to do natively.
5. **Hamlib licence terms** of the `libsecurity` files linked into libhamlib: "freeware" for `AESStringCrypt.c`, `password.c` and `security.c`; GPL-2.0-or-later for `aes.c` and `sha256.c`; no header for `md5.c` (section 4.3).
   - Recommendation: ask Hamlib upstream about all of them before the first release that links it. The feature is documented as not implemented, so they may relicense or drop it. The GPL-2.0-or-later files are compatible with this GPL-3.0 app but must be listed in the notices. Not a legal conclusion.
6. **Hamlib security.**
   - Recommendation: pin a patched `Hamlib-4.7` commit, re-checking the advisories and the branch head at that moment. Offer no network models (2 or 4) and no SmartSDR backend until then. Move to 4.7.3 when tagged.
7. **Windows CAT.**
   - Recommendation: run the 4a spike early. If ClangCL cannot link the MinGW DLL, ship Windows USB as audio-only for that release rather than switching compiler.
8. **CAT port sharing.**
   - Recommendation: direct serial by default. Phase 6 adds rigctld and flrig, and can follow phase 4 directly once a patched Hamlib exists. Point IC-7300MK2, IC-7760, FT-710 and FTX-1 users to their second CAT port.
9. **Flex at all.**
   - Recommendation: build it only if two Flex testers from different families commit (or claim one family only), and ask FlexRadio in writing whether a free GPL client needs anything under "A software license is required for commercial applications".
10. **TCI claims with a simulated radio.**
    - Recommendation: accept them, under the separate label "tested with a simulated radio" (section 7).
11. **Icom manual terms.**
    - The research fetched Icom PDFs directly without ticking the download page's Agree box. Icom's terms prohibit redistribution and "transfer of... data etc." to third parties [I5].
    - Recommendation: keep the PDFs out of the repo, cite by URL, and keep this document's command tables as short facts.
    - Decide whether more is needed. Not legal advice.
12. **Code-copy policy.**
    - May the project copy GPL-3 code from wfview or PR #4921? AetherSDR's restriction is its own policy, not a licence bar [A3].
    - Recommendation: moot for v1 (Hamlib). Decide only if a native fallback is written.
13. **Dual-receiver radios in v1.**
    - Recommendation: mute and offer Main only on every dual-receiver USB radio (section 5.1). Sub needs per-receiver keys and, on the IC-9700, a band change.

## 11. Sources

**How this was researched, and its limits.**

- No radio was contacted.
- yaesu.com could not be fetched, so most Yaesu documents are third-party copies (rigpix, rcqsl, ure.es, exseli, manuals.plus).
- Some PDFs were searched with pdftotext before their decisive pages were read. Some ManualsLib pages came through a summarising fetch tool and are less exact.
- Downloaded copies were kept outside this repository, and nothing in them was executed. Every source below is cited by URL. Two exceptions are noted at K5 and F20.
- The Hamlib findings marked [H25] come from reading the 4.7.2 source at the `4.7.2` tag.

Local paths in this document (src/..., tests/..., packaging/..., scripts/..., .github/...) are in this repository at commit 5455f6f. A path written "Hamlib: ..." or a bare Hamlib file name (rig.c, newcat.c, kenwood.c and so on) is in Hamlib 4.7.2. A path written "AetherSDR ..." is in the AetherSDR commit below.

### AetherSDR (commit 141231e412ed554b9ab49213b2f8c68614fbf193)

Base: `https://github.com/aethersdr/AetherSDR/blob/141231e412ed554b9ab49213b2f8c68614fbf193/`

- A1 `src/core/backends/icom/IcomModels.cpp#L22-L139`; `src/core/backends/icom/IcomModels.h`
- A2 `docs/user/docs/networked-icom.md#L31-L38`, `#L95-L97`
- A3 `docs/architecture/aetherd-icom-civ-backend-design.md#L690-L711`, `#L713-L728`, `#L786-L799`, `#L834-L838`, `#L857-L858`, `#L73-L103`
- A4 `src/core/backends/icom/IcomCivBackend.cpp#L5519-L5527`, `#L969-L975`, `#L3992-L4028`
- A5 `docs/architecture/radio-capabilities-map.md#L367`
- A6 `src/core/backends/icom/IcomCivBackend.h#L610-L618`, `#L20-L37`
- A7 `src/core/PcmFrame.h#L23-L33`
- A8 `src/core/backends/icom/CivCodec.h#L14-L20`
- A9 `src/core/backends/icom/IcomCivScheduler.h#L46`
- A10 `src/core/backends/IRadioBackend.h#L97`
- A11 https://github.com/aethersdr/AetherSDR/issues/5262
- A12 https://github.com/aethersdr/AetherSDR/pull/4921
- A13 https://github.com/aethersdr/AetherSDR/issues/5205
- A14 `THIRD_PARTY_LICENSES`; https://github.com/nonoo/kappanhang/blob/main/LICENSE
- A15 `src/core/backends/flex/CommandParser.cpp`; `src/core/RadioDiscovery.cpp#L179-L184`; `src/core/backends/flex/PanadapterStream.cpp#L993-L1036`, `#L32-L35`, `#L259-L266`, `#L380-L408`; `src/core/backends/flex/FlexBackend.h#L34-L90`; `src/models/RadioModel.cpp#L7731-L7793`; `src/core/backends/flex/RadioConnection.cpp#L103-L109`
- A16 `src/core/TciServer.cpp#L4436-L4439`; `docs/user/docs/cw-decoder.md#L101`; `docs/user/docs/tci-server.md#L149`
- A17 `src/core/AudioEngine.cpp#L5748-L5750`
- A18 `src/models/RadioModel.cpp#L10002-L10007`
- A19 `docs/user/docs/audio-settings.md#L48`
- A20 `tools/flex-tailnet-shim/README.md`
- A21 `tests/gui_client_registration_recovery_test.cpp`
- A22 `src/core/backends/sim/SimBackend.h#L22-L30`
- A23 `packaging/macos/AetherSDR.entitlements`
- A24 `README.md`; `GOVERNANCE.md`; `SUPPORT.md`; `CONTRIBUTING.md`; `LICENSE`; https://www.aethersdr.com
- A25 `docs/user/docs/aetherial-audio.md`; `packaging/linux/io.github.aethersdr.aethersdr.metainfo.xml`
- A26 https://github.com/aethersdr/AetherSDR/issues/3912
- A27 Excluded-scope leads: https://github.com/aethersdr/AetherSDR/pull/6912; https://github.com/aethersdr/AetherSDR/issues/6370; `docs/user/docs/supported-radios.md#L57-L66`; `src/core/backends/icom/IcomSession.cpp#L491-L547`; `src/core/backends/icom/IcomProtocol.cpp#L553-L574`
- A28 `src/gui/PooDooLogo.h#L29-L31`; `CHANGELOG.md`; https://github.com/aethersdr/AetherSDR/pull/6536; https://github.com/aethersdr/AetherSDR/issues/3440; https://github.com/aethersdr/AetherSDR/issues/5355
- A29 https://github.com/nigelfenton/Aether-gate/blob/main/RADIO_SUPPORT.md; https://github.com/aethersdr/Aether-gate/blob/main/KENWOOD_BRINGUP.md; https://github.com/aethersdr/Aether-gate
- A30 `docs/user/docs/first-connection.md#L83-L87`; `packaging/windows/installer.iss#L96-L105`
- A31 `docs/agents/flex-protocol.md`; `docs/architecture/vita49-format.md`; `src/core/TciServer.cpp#L4451-L4500`; `src/models/RadioModel.cpp#L11410-L11470`
- A32 `src/core/RadioDiscovery.cpp#L58-L62`

### Icom

- I1 IC-7300 Full Manual: https://www.icomjapan.com/support/manual/2271/
- I2 IC-7300MK2 Basic Manual: https://www.icomjapan.com/support/manual/4459/
- I3 IC-7300MK2 CI-V Guide: https://www.icomjapan.com/support/manual/4486/
- I4 IC-7610 Basic Manual: https://www.icomjapan.com/support/manual/1711/
- I5 IC-7610 CI-V Guide (also the download terms): https://www.icomjapan.com/support/manual/1745/
- I6 IC-705 Basic Manual: https://www.icomjapan.com/support/manual/3060/
- I7 IC-705 CI-V Guide: https://www.icomjapan.com/support/manual/3064/
- I8 IC-9700 Basic Manual: https://www.icomjapan.com/support/manual/2156/
- I9 IC-9700 CI-V Guide: https://www.icomjapan.com/support/manual/2161/
- I10 IC-7760 Basic Manual: https://www.icomjapan.com/support/manual/4219/
- I11 IC-7760 CI-V Guide: https://www.icomjapan.com/support/manual/4227/
- I12 IC-905 Basic Manual: https://www.icomjapan.com/support/manual/3762/
- I13 IC-905 CI-V Guide: https://www.icomjapan.com/support/manual/3792/
- I14 IC-R8600 Instruction Manual: https://www.icomjapan.com/support/manual/1630/
- I15 IC-R8600 CI-V Guide: https://www.icomjapan.com/support/manual/1633/
- I16 USB Port Setting guide: https://www.icomjapan.com/support/manual/2574/
- I17 IC-7300 firmware notes: https://www.icomjapan.com/support/manual/2188/
- I18 Firmware notes: https://www.icomjapan.com/support/manual/1772/
- I19 USB driver guide: https://www.icomjapan.com/support/manual/3193/
- I20 USB driver (older radios): https://www.icomjapan.com/support/firmware_driver/2417/
- I21 USB driver (IC-705, IC-905, IC-7760, IC-7300MK2 and others): https://www.icomjapan.com/support/firmware_driver/4491/
- I22 IC-7850/7851 manual: https://www.icomjapan.com/support/manual/1563/; https://rigpix.com/icom/ic7850-7851_manual.pdf
- I23 IC-7600 manual: https://www.icomjapan.com/support/manual/2615/; https://rigpix.com/icom/ic7600_manual.pdf
- I24 IC-7300 service manual: https://rigpix.com/icom/ic7300_service.pdf
- I25 IC-9700 service manual: https://rigpix.com/icom/ic9700_service.pdf
- I26 IC-7100: https://rigpix.com/icom/ic7100_manual.pdf; https://rigpix.com/icom/ic7100_advanced_manual.pdf; https://rigpix.com/icom/ic7100_service.pdf
- I27 IC-7200: https://rigpix.com/icom/ic7200_advancedmanual.pdf; https://rigpix.com/icom/ic7200_manual.pdf
- I28 IC-7410: https://rigpix.com/icom/ic7410_manual.pdf
- I29 IC-9100: https://rigpix.com/icom/ic9100_manual.pdf; https://rigpix.com/icom/ic9100_service.pdf
- I30 IC-R8600 service manual: https://rigpix.com/icom/icr8600_service_june2017.pdf
- I31 IC-7610 on ManualsLib: https://www.manualslib.com/manual/1542112/Icom-Ic-7610.html?page=19; https://www.manualslib.com/manual/1542112/Icom-Ic-7610.html?page=77
- I32 IC-7851 service on ManualsLib: https://www.manualslib.com/manual/1132104/Icom-Ic-7851.html?page=46; https://www.manualslib.com/manual/1132104/Icom-Ic-7851.html?page=141
- I33 ManualsLib: https://www.manualslib.com/manual/1146210/Icom-Ic-7200.html?page=122; https://www.manualslib.com/manual/851768/Icom-Ic-7410.html?page=86; https://www.manualslib.com/manual/805388/Icom-Ic-7600.html?page=48
- I34 ID-52: https://www.manualslib.com/manual/2493468/Icom-Id-52a.html?page=202; https://www.icomjapan.com/lineup/products/ID-52A/
- I35 TI PCM2901: https://www.ti.com/product/PCM2901
- I36 https://qsl.net/aa4te/7300set.html
- I37 https://dxlabsuite.com/dxlabwiki/RTTY_SplitDecode
- I38 https://gm4fvm.blogspot.com/2020/10/main-and-sub-band-usb-audio-output-with.html (search snippet only)
- I39 https://raw.githubusercontent.com/csete/gpredict/master/doc/notes/ic-9700.txt
- I40 https://gist.github.com/peteonrails/f15c82f38c206decb3bf5dda86f0cf17
- I41 https://treexy.com/products/driver-fusion/database/ports-com-lpt-serial/icom/ic-705-serial-port-ci-v/; https://treexy.com/products/driver-fusion/database/ports-com-lpt-serial/icom/ic-705-serial-port/
- I42 https://raw.githubusercontent.com/morozsm/icom-lan/main/docs/guide/ic7610-usb-setup.md; https://raw.githubusercontent.com/morozsm/icom-lan/main/docs/internals/protocol.md
- I43 https://mail.gnu.org/archive/html/tlf-devel/2022-05/msg00017.html; https://lists.gnu.org/archive/html/tlf-devel/2022-05/msg00020.html
- I44 https://fbnews.jp/201805/technical_en/; https://www.n1fd.org/2018/11/26/wsjt-x-setup/
- I45 https://gist.github.com/edmondburnett/eadfb85ae6cc017062756d161a83089f
- I46 https://rigpix.com/icom/ic7700_manual.pdf; https://rigpix.com/icom/ic7800_manual.pdf
- I47 https://rigpix.com/icom/ic7000_manual.pdf; https://rigpix.com/icom/ic718_manual.pdf; https://rigpix.com/icom/icr9500_manual.pdf; https://rigpix.com/icom/icr30_advanced.pdf; https://rigpix.com/icom/id5100ae_manual.pdf; https://rigpix.com/kenwood/ts570d-s_manual.pdf; https://rigpix.com/kenwood/thd72ae_manual.pdf
- I48 https://www.icomjapan.com/lineup/products/ID-5200A/; https://www.bluebottlemarine.com/products/icom-multi-function-dual-band-d-star-transceiver-plus.html; https://moonrakeronline.com/yaesu-scu-57-wires-x-connection-cable-kit-for-ft-5dr-ft-3dr-ft-2dr
- I49 https://www.wimo.com/en/icom-x-026
- I50 https://www.manualslib.com/manual/1106166/Icom-Ic-7300.html?page=124
- I51 wfview rig files and source: https://github.com/wf-group/wfview/tree/master/rigs; https://github.com/eliggett/wfview/blob/master/rigs/IC-7610.rig; https://gitlab.com/eliggett/wfview
- I52 https://wfview.org/wfview-user-manual/faq/
- Excluded-scope leads (RS-BA1): https://www.wimo.com/en/rs-ba1v2; https://northgeorgiacommunications.com/product/icom-rs-ba1-ip-remote-control-software-rs-ba1-v2-advanced-dualwatch-remote-control-software/

### Kenwood

- K1 Virtual COM driver: https://www.kenwood.com/i/products/info/amateur/vcp_e.html
- K2 TS-890S PC command guide (2018-08-30 edition): https://kasc.kenwood.com/files/prod/3340/5/ts890_pc_command_e.pdf
- K3 TS-990S PC control command reference: https://people.freebsd.org/~wpaul/ts-990s/TS-990S_PC_Control_Command_Reference.pdf
- K4 TS-590SG PC command guide: https://www.kenwood.com/i/products/info/amateur/pdf/ts_590_g_pc_command_e.pdf
- K5 TS-590S PC command guide. Kenwood's URL, http://www.kenwood.com/i/products/info/amateur/pdf/ts_590_pc_command_e.pdf, refused the fetch ("Access Denied"). The copy that was read came from a mirror whose URL was not recorded. So facts cited only to K5 are **unverified** against a known source. The TS-590S ID 021 also appears in Hamlib's ID list (kenwood.c:122) [K24]
- K6 TS-480 PC control: https://www.radioamatore.info/attachments/059_TS-480_pc_control_command.pdf
- K7 TS-2000 manual (Spanish): https://www.rcqsl.com/manuals/108.pdf
- K8 TS-890S instruction manual: https://www.rcqsl.com/manuals/159.pdf
- K9 TS-990S USB audio manual: https://kasc-cdn.novena.hr/files/prod/2452/5/TS-990S_USB_Audio_manual.pdf
- K10 TS-990S manual (ES): https://kasc-cdn.novena.hr/files/prod/2452/5/TS-990S_5_1_A_8_3_ES.pdf
- K11 https://www.manualsdir.com/manuals/410855/kenwood-ts-990s.html?page=253; https://www.manualsdir.com/manuals/410855/kenwood-ts-990s.html?page=30
- K12 TS-590SG manual: https://archive.org/download/manualsonline-id-8897f8ce-9022-42bc-923f-15eca390d3a7/8897f8ce-9022-42bc-923f-15eca390d3a7.pdf; https://rigpix.com/kenwood/ts590sg_manual.pdf
- K13 https://rigpix.com/kenwood/ts590sg_indepth.pdf; https://rigpix.com/kenwood/ts590sg_usbaudio.pdf
- K14 https://www.radioamatore.info/attachments/article/803/ts590_usb_audio_manual.pdf
- K15 https://www.manualsdir.com/manuals/410851/kenwood-ts-590s.html?page=73; https://rigpix.com/kenwood/ts590s_manual.pdf
- K16 https://w9cf.github.io/notes/ts590sg/udev/ts590sg_udev.pdf
- K17 http://www.linux-usb.org/usb.ids
- K18 TI PCM2903B: https://www.ti.com/lit/gpn/pcm2903b
- K19 https://dxlabsuite.com/dxlabwiki/ConnectingKenwoodUSB; https://www.n1fd.org/2018/11/26/wsjt-x-setup/
- K20 TH-D75 operating tips: http://www.kenwood.com/i/products/info/amateur/pdf/TH-D75AE_IDM.pdf
- K21 https://rigpix.com/kenwood/thd74ae_manual.pdf; https://www.jvckenwood.com/en/press/2016/pdf/1608_press_160808.pdf
- K22 https://rigpix.com/kenwood/ts890s_schematic.pdf
- K23 https://rigpix.com/kenwood/ts2000_manualrev2.pdf; https://rigpix.com/kenwood/ts480_manual.pdf
- K24 Hamlib at 9199f8a2: https://github.com/Hamlib/Hamlib/blob/9199f8a239aed26b5eb0ee9c1a73bf51580483ce/rigs/kenwood/kenwood.c#L100; https://github.com/Hamlib/Hamlib/blob/9199f8a239aed26b5eb0ee9c1a73bf51580483ce/rigs/kenwood/kenwood.c#L5317; https://github.com/Hamlib/Hamlib/blob/9199f8a239aed26b5eb0ee9c1a73bf51580483ce/rigs/kenwood/ts990s.c; https://github.com/Hamlib/Hamlib/blob/9199f8a239aed26b5eb0ee9c1a73bf51580483ce/rigs/kenwood/ts590.c#L1909; https://github.com/Hamlib/Hamlib/blob/9199f8a239aed26b5eb0ee9c1a73bf51580483ce/simulators/simts890.c#L214
- K25 https://github.com/GregTroar/OpsLog/wiki/Kenwood-and-Xiegu
- K26 https://www.kenwood.com/i/products/info/amateur/ts_890/ts890_update_e.html; https://www.kenwood.com/i/products/info/amateur/ts_890/arcp890_e.html; https://www.jvckenwood.com/content/dam/jvckenwood/jp/pdf/jvckenwood-press-20260806-01-ja.pdf
- K27 https://tapr.org/wp-content/uploads/DCC2018-08.pdf
- K28 https://forum.yo3ggx.ro/viewtopic.php?p=1075; https://gist.github.com/shortword/d2f756965240587adb1d9c43acaf4a7d
- K29 https://forum.log4om.com/viewtopic.php?p=34061; https://la3za.blogspot.com/2019/06/port-sharing-for-wsjt-x.html; https://hamlib.sourceforge.net/html/rigctld.1.html
- K30 Excluded scope (KNS and VoIP): https://kasc.kenwood.com/files/prod/3340/5/ts890_kns_manual_e.pdf; https://github.com/open890/kenwood-ts890/blob/main/VOIP-HOWTO.md; https://github.com/open890/kenwood-ts890/blob/main/LAN-HOWTO.md; https://kenwood.com/i/products/info/amateur/arvp10_e.html; https://github.com/tonyc/open890; https://archive.org/download/manualsonline-id-0c33f307-855e-460a-a3ee-c818adf1f1d6/0c33f307-855e-460a-a3ee-c818adf1f1d6.pdf

### Yaesu

- Y1 FTDX101 service manual: https://rigpix.com/yaesu/ftdx101mp-d_service.pdf
- Y2 FTDX101 operation manual: https://www.rcqsl.com/manuals/29.pdf
- Y3 FTDX101 CAT manual: https://www.ure.es/wp-content/uploads/wpforo/attachments/1424/5853-FTDX101MPCATOMENG1912-D.pdf
- Y4 FT-991A service manual: https://rigpix.com/yaesu/ft991a_service.pdf
- Y5 FT-991A CAT manual: https://rigpix.com/yaesu/ft991a_cat_manual.pdf
- Y6 FT-991A operating manuals: https://rigpix.com/yaesu/ft991a_manual_v2020.pdf; https://www.rcqsl.com/manuals/53.pdf
- Y7 FT-991: https://rigpix.com/yaesu/ft991_manual.pdf; https://rigpix.com/yaesu/ft991_service.pdf; https://rigpix.com/yaesu/ft991_cat_manual.pdf
- Y8 FT-710: https://rigpix.com/yaesu/ft710_manual.pdf; https://exseli.com/pdf/uploaded/manual_trn_809495519.pdf
- Y9 FTX-1: https://rigpix.com/yaesu/ftx1_manual.pdf; https://rigpix.com/yaesu/ftx1_advanced.pdf; https://manuals.plus/m/48c58a32c33f061c777325efc02ca947c30b557a1bbabc970876a92e47cd98db.pdf
- Y10 FTDX10: https://rigpix.com/yaesu/ftdx10_manual.pdf; https://rigpix.com/yaesu/ftdx10_cat_manual.pdf; https://www.yaesu.com/Files/4CB893D7-1018-01AF-FA97E9E9AD48B50C/FTDX10_CAT_OM_ENG_2308-F.pdf
- Y11 FTDX3000: https://www.rigpix.com/yaesu/ftdx3000_manual.pdf; https://rigpix.com/yaesu/ftdx3000_cat_reference.pdf
- Y12 FTDX5000: https://rigpix.com/yaesu/ftdx5000_manual.pdf; https://rigpix.com/yaesu/ftdx5000_cat_manual.pdf
- Y13 FT-891: https://rigpix.com/yaesu/ft891_manual.pdf; https://rigpix.com/yaesu/ft891_advance_manual.pdf; https://rigpix.com/yaesu/ft891_cat_manual.pdf; https://digirig.net/digirig-dr-891-setup-manual/; https://freedv.org/getting-started-yaesu/
- Y14 TI PCM2903C: https://www.ti.com/lit/ds/symlink/pcm2903c.pdf
- Y15 https://gist.github.com/shortword/3acef4ba1399e04232b847498666f1f1
- Y16 https://micropipes.com/2023/07/10/setting-up-Yaesu-FTDX-3000-with-FT8-on-OS-X/
- Y17 https://raw.githubusercontent.com/torvalds/linux/master/drivers/usb/serial/cp210x.c
- Y18 https://github.com/jbdubbs/Rig-Control-Web/wiki/Spectrum-Scope-FT-710
- Y19 https://m0aws.co.uk/?p=457; https://www.qsl.net/ve3sma/FTDX10%20FT8%20Settings%20Jan%202%202022v0.3.pdf; https://www.qsl.net/g4ruj/Yaesu%20FTdx101MP.htm
- Y20 https://hackage.haskell.org/package/ham-0.1.0.0/docs/src/Ham.CAT.YaesuFT891.html
- Y21 Yaesu USB driver installation manual: https://www.yaesu.com/Files/BB2B47AE-1018-01AF-FAE48FDCB1919193/USB_Driver_Installation_Manual_ENG_2205-E.pdf
- Y22 No built-in USB audio: https://rigpix.com/yaesu/ft450d_manual.pdf; https://rigpix.com/yaesu/ft857d_manual.pdf; https://rigpix.com/yaesu/ft818nd_manual.pdf; https://rigpix.com/yaesu/ftdx1200_manual.pdf; https://rigpix.com/yaesu/ft2000_manual.pdf; https://rigpix.com/yaesu/ft2000_cat_manual.pdf; https://rigpix.com/yaesu/ft950_manual.pdf; https://rigpix.com/yaesu/ft897d_manual.pdf; https://rigpix.com/yaesu/ft817nd_manual.pdf
- Y23 Hamlib Yaesu at 9199f8a2: https://github.com/Hamlib/Hamlib/tree/9199f8a239aed26b5eb0ee9c1a73bf51580483ce/rigs/yaesu (newcat.c, ftdx101.c, ft710.c, ft991.c, ft3000.c, ftx1/ftx1_readme.txt)
- Y24 Excluded scope (SCU-LAN10): https://www.rcqsl.com/manuals/25.pdf; https://www.wimo.com/en/blog/post/scu-lan10
- Y25 https://groups.google.com/g/digipi/c/MWWNC-LkJZM

### Hamlib and other control software

- H1 https://github.com/Hamlib/Hamlib/blob/4.7.2/LICENSE
- H2 https://github.com/Hamlib/Hamlib/blob/4.7.2/COPYING.LIB#L211
- H3 https://github.com/Hamlib/Hamlib/blob/4.7.2/security/AESStringCrypt.c#L21; https://github.com/Hamlib/Hamlib/blob/4.7.2/security/password.c#L12; https://github.com/Hamlib/Hamlib/blob/4.7.2/security/security.c#L7; https://github.com/Hamlib/Hamlib/blob/4.7.2/security/aes.c#L1-L9; https://github.com/Hamlib/Hamlib/blob/4.7.2/security/sha256.c#L1-L9; https://github.com/Hamlib/Hamlib/blob/4.7.2/security/md5.c; https://github.com/Hamlib/Hamlib/blob/4.7.2/security/md5.h#L6-L11; https://github.com/Hamlib/Hamlib/blob/4.7.2/security/Makefile.am; https://github.com/Hamlib/Hamlib/blob/4.7.2/src/Makefile.am#L26
- H4 https://github.com/Hamlib/Hamlib/releases; https://github.com/Hamlib/Hamlib/blob/4.7.2/ReleaseNotes_4.7.md
- H5 https://github.com/Hamlib/Hamlib/security/advisories (read through `gh api repos/Hamlib/Hamlib/security-advisories` on 2026-11-07): GHSA-3qgv-jc7m-q329, GHSA-q55w-8m34-8wjf, GHSA-xxp5-cm5j-pg53, GHSA-8pgf-h33g-fmcv, GHSA-xgh4-3v35-8q74, GHSA-grww-p672-wcf5, GHSA-f72v-7gmh-m9mj; https://github.com/Hamlib/Hamlib/blob/4.7.2/src/iofunc.c#L1465-L1485
- H6 https://github.com/Hamlib/Hamlib/commits/Hamlib-4.7 (head 913df25602, 2026-11-05, on 2026-11-07; tags end at 4.7.2)
- H7 https://github.com/Hamlib/Hamlib/wiki/Supported-Radios; and the model `caps` structures in the 4.7.2 source, parsed for status, AF and PTT declarations (https://github.com/Hamlib/Hamlib/tree/4.7.2/rigs)
- H8 https://github.com/Hamlib/Hamlib/blob/4.7.2/rigs/yaesu/ft817.c#L336; https://github.com/Hamlib/Hamlib/blob/4.7.2/rigs/yaesu/ft857.c#L284; https://github.com/Hamlib/Hamlib/blob/4.7.2/rigs/yaesu/ft897.c#L273; https://github.com/Hamlib/Hamlib/blob/4.7.2/rigs/icom/ic706.c#L77; https://github.com/Hamlib/Hamlib/blob/4.7.2/rigs/kenwood/ts990s.c#L360
- H9 https://github.com/Hamlib/Hamlib/blob/4.7.2/rigs/icom/icom_defs.h#L86; https://github.com/Hamlib/Hamlib/blob/4.7.2/rigs/icom/icom_defs.h#L260; https://github.com/Hamlib/Hamlib/blob/4.7.2/rigs/yaesu/newcat.c#L4122; https://github.com/Hamlib/Hamlib/blob/4.7.2/rigs/kenwood/kenwood.c#L4026; https://github.com/Hamlib/Hamlib/blob/4.7.2/rigs/icom/ic7300.c#L284; https://github.com/Hamlib/Hamlib/blob/4.7.2/rigs/kenwood/ts590.c#L555
- H10 https://github.com/Hamlib/Hamlib/blob/4.7.2/.github/workflows/c-cpp.yml; https://github.com/Hamlib/Hamlib/blob/4.7.2/.github/workflows/macos-latest-no-features.yml
- H11 https://github.com/Hamlib/Hamlib/releases/download/4.7.2/hamlib-w64-4.7.2.zip; https://github.com/Hamlib/Hamlib/blob/4.7.2/scripts/README.build-Windows
- H12 https://raw.githubusercontent.com/microsoft/vcpkg/master/ports/hamlib/vcpkg.json; https://raw.githubusercontent.com/microsoft/vcpkg/master/ports/hamlib/portfile.cmake
- H13 https://github.com/Hamlib/Hamlib/blob/4.7.2/include/hamlib/rig.h#L66; https://github.com/Hamlib/Hamlib/blob/4.7.2/include/hamlib/rig.h#L2563; https://github.com/Hamlib/Hamlib/blob/4.7.2/ReleaseNotes_5.0.md
- H14 QLog: https://github.com/foldynl/QLog/blob/master/BUILDING.md; https://github.com/foldynl/QLog/tree/master/rig/drivers; https://github.com/foldynl/QLog/blob/master/packaging/appimage/build.sh; https://github.com/foldynl/QLog/blob/master/packaging/appimage/versions.sh; https://github.com/foldynl/QLog/blob/master/.github/workflows/appimage.yml; https://github.com/foldynl/QLog/blob/master/rig/Rig.cpp#L266; https://github.com/foldynl/QLog/blob/master/rig/RigctldManager.cpp; https://github.com/foldynl/QLog/blob/master/Changelog; https://github.com/foldynl/QLog/blob/master/rig/drivers/HamlibRigDrv.cpp#L291
- H15 https://formulae.brew.sh/api/formula/hamlib.json; https://tracker.debian.org/pkg/hamlib; https://packages.ubuntu.com/search?keywords=libhamlib-dev&searchon=names&suite=all&section=all
- H16 https://github.com/Hamlib/Hamlib/blob/4.7.2/src/rig.c#L1522; https://github.com/Hamlib/Hamlib/blob/4.7.2/doc/man1/rigctl.1#L197
- H17 https://github.com/Hamlib/Hamlib/blob/4.7.2/doc/man1/rigctld.1#L225; https://github.com/Hamlib/Hamlib/blob/4.7.2/doc/man1/rigctld.1#L408
- H18 https://github.com/Hamlib/Hamlib/blob/4.7.2/include/hamlib/riglist.h#L101; https://github.com/Hamlib/Hamlib/blob/4.7.2/rigs/dummy/netrigctl.c#L563; https://github.com/Hamlib/Hamlib/blob/4.7.2/rigs/dummy/flrig.c#L2271; https://github.com/Hamlib/Hamlib/blob/4.7.2/rigs/dummy/flrig.c#L2358
- H19 https://github.com/w1hkj/flrig/blob/master/src/server/xml_server.cxx#L738; https://github.com/w1hkj/flrig/blob/master/src/support/status.cxx#L91; https://machamradio.com/blog/2023/11/02/flrig-v2-0-04-now-available/
- H20 https://github.com/VE3NEA/OmniRig/blob/master/LICENSE; https://github.com/VE3NEA/OmniRig/blob/master/OmniRig_TLB.pas#L59; https://github.com/VE3NEA/OmniRig/blob/master/OmniRig.ridl#L182; https://www.dxatlas.com/OmniRig; https://www.hb9ryz.ch/omnirig/
- H21 wfview: https://github.com/wf-group/wfview; https://github.com/wf-group/wfview/tree/master/src/radio; https://github.com/wf-group/wfview/blob/master/WFSERVER.md; https://github.com/wf-group/wfview/blob/master/src/rigctld.cpp#L59; https://github.com/wf-group/wfview/blob/master/WHATSNEW#L17; https://github.com/eliggett/wfview/tree/master/src/radio
- H22 Bridges: https://github.com/ftl/tciadapter; https://github.com/jfrancis42/hamlib-tci-sidecar; https://github.com/jsttylr/flex-emulator; https://bridges.n6nu.org/
- H23 https://github.com/Hamlib/Hamlib/blob/4.7.2/rigs/dummy/dummy.c#L1295; https://github.com/Hamlib/Hamlib/blob/4.7.2/rigs/dummy/dummy.c#L2417; https://github.com/Hamlib/Hamlib/blob/4.7.2/README.developer#L478; https://github.com/Hamlib/Hamlib/blob/4.7.2/README.developer#L777; https://github.com/Hamlib/Hamlib/blob/4.7.2/simulators/sim.h; https://github.com/Hamlib/Hamlib/blob/4.7.2/simulators/Makefile.am; https://github.com/Hamlib/Hamlib/blob/4.7.2/simulators/simic7300.c#L273; https://github.com/Hamlib/Hamlib/tree/4.7.2/simulators
- H24 WSJT-X build leads, unverified: https://sourceforge.net/p/wsjt/wsjtx-superbuild; https://msys2-jtsdk.sourceforge.io/installation.html; https://wsjt.sourceforge.io/wsjtx-doc/wsjtx-main-2.7.0.html
- H25 Hamlib 4.7.2 source, read for this plan: https://github.com/Hamlib/Hamlib/blob/4.7.2/src/conf.c#L456-L500; https://github.com/Hamlib/Hamlib/blob/4.7.2/src/serial_cfg_params.h#L28; https://github.com/Hamlib/Hamlib/blob/4.7.2/src/rig.c#L8930; https://github.com/Hamlib/Hamlib/blob/4.7.2/rigs/icom/icom.c#L1148-L1260; https://github.com/Hamlib/Hamlib/blob/4.7.2/rigs/icom/icom.c#L3620; https://github.com/Hamlib/Hamlib/blob/4.7.2/rigs/kenwood/kenwood.c#L1156-L1168; https://github.com/Hamlib/Hamlib/blob/4.7.2/rigs/kenwood/kenwood.c#L1228-L1231; https://github.com/Hamlib/Hamlib/blob/4.7.2/rigs/kenwood/kenwood.c#L3389; https://github.com/Hamlib/Hamlib/blob/4.7.2/rigs/yaesu/newcat.c#L50-L80; https://github.com/Hamlib/Hamlib/blob/4.7.2/rigs/yaesu/newcat.c#L800-L905; https://github.com/Hamlib/Hamlib/blob/4.7.2/rigs/yaesu/newcat.c#L930-L950; https://github.com/Hamlib/Hamlib/blob/4.7.2/rigs/yaesu/newcat.c#L4128; https://github.com/Hamlib/Hamlib/blob/4.7.2/rigs/kenwood/kenwood.c#L902; https://github.com/Hamlib/Hamlib/blob/4.7.2/rigs/kenwood/kenwood.c#L3370-L3465; https://github.com/Hamlib/Hamlib/blob/4.7.2/rigs/kenwood/kenwood.c#L3670; https://github.com/Hamlib/Hamlib/blob/4.7.2/rigs/kenwood/kenwood.c#L4026-L4074; https://github.com/Hamlib/Hamlib/blob/4.7.2/rigs/kenwood/ts590.c#L578-L579; https://github.com/Hamlib/Hamlib/blob/4.7.2/rigs/kenwood/ts890s.c#L105; https://github.com/Hamlib/Hamlib/blob/4.7.2/rigs/kenwood/ts990s.c#L364; https://github.com/Hamlib/Hamlib/blob/4.7.2/src/rig.c#L812-L827; https://github.com/Hamlib/Hamlib/blob/4.7.2/src/rig.c#L1612-L1614; https://github.com/Hamlib/Hamlib/blob/4.7.2/src/rig.c#L1696-L1720; https://github.com/Hamlib/Hamlib/blob/4.7.2/src/rig.c#L3957-L3992; https://github.com/Hamlib/Hamlib/blob/4.7.2/src/event.c#L86; https://github.com/Hamlib/Hamlib/blob/4.7.2/src/event.c#L264-L310; https://github.com/Hamlib/Hamlib/blob/4.7.2/src/network.c#L1612; https://github.com/Hamlib/Hamlib/blob/4.7.2/src/conf.c#L106-L221; https://github.com/Hamlib/Hamlib/blob/4.7.2/rigs/yaesu/ftx1/ftx1.c#L1005

### PortAudio, Apple, Qt, Linux and Windows

- P1 https://github.com/PortAudio/portaudio/releases; https://raw.githubusercontent.com/PortAudio/portaudio/v19.7.0/CMakeLists.txt; https://raw.githubusercontent.com/PortAudio/portaudio/master/CMakeLists.txt
- P2 https://raw.githubusercontent.com/PortAudio/portaudio/v19.7.0/include/portaudio.h
- P3 https://raw.githubusercontent.com/PortAudio/portaudio/v19.7.0/include/pa_win_wasapi.h; https://github.com/PortAudio/portaudio/issues/314
- P4 https://raw.githubusercontent.com/PortAudio/portaudio/v19.7.0/src/hostapi/alsa/pa_linux_alsa.c
- P5 https://developer.apple.com/tutorials/data/documentation/bundleresources/entitlements/com.apple.security.device.audio-input.json; https://developer.apple.com/tutorials/data/documentation/bundleresources/information-property-list/nsmicrophoneusagedescription.json; https://developer.apple.com/tutorials/data/documentation/security/hardened-runtime.json; https://developer.apple.com/tutorials/data/documentation/avfoundation/requesting-authorization-to-capture-and-save-media.json; https://developer.apple.com/tutorials/data/documentation/bundleresources/entitlements/com.apple.security.device.usb.json
- P6 https://developer.apple.com/forums/thread/696384; https://eclecticlight.co/2021/01/08/notarisation-privacy-controls/
- P7 https://doc.qt.io/qt-6/permissions.html
- P8 https://github.com/spatialaudio/python-sounddevice/issues/267
- P9 https://packages.debian.org/bookworm/qt6-serialport-dev; https://packages.ubuntu.com/resolute/libqt6serialport6
- P10 https://raw.githubusercontent.com/AppImageCommunity/pkg2appimage/master/excludelist
- P11 https://wiki.debian.org/SystemGroups; https://bugs.launchpad.net/ubuntu/+source/brltty/+bug/1958224
- P12 https://support.microsoft.com/en-us/windows/privacy/turn-on-app-permissions-for-your-microphone-in-windows
- P13 https://raw.githubusercontent.com/torvalds/linux/master/drivers/tty/tty_port.c
- P14 https://doc.qt.io/qt-6/qserialport.html
- P15 https://github.com/drowe67/freedv-gui/blob/master/README.md#L78; https://freedv.org/mooneers-freedv-update-march-2025/; https://github.com/wf-group/wfview/blob/master/include/wfviewtypes.h#L686; https://raw.githubusercontent.com/saitohirga/WSJT-X/master/Audio/soundin.cpp
- P16 https://github.com/aethersdr/Aether-gate/blob/main/aether_gate/adapters/icom7300.py#L195
- P17 https://developer.apple.com/documentation/technotes/tn3179-understanding-local-network-privacy; https://developer.apple.com/documentation/technotes/tn3127-inside-code-signing-requirements; https://developer.apple.com/documentation/bundleresources/information-property-list/cfbundleidentifier
- P18 https://support.apple.com/guide/mac-help/change-firewall-settings-on-mac-mh11783/mac

### TCI

- T1 https://eesdr.com/en/software-en/expertsdr3-en
- T2 TCI 2.1: https://github.com/ExpertSDR3/TCI/blob/6cbccdd700/TCI%20Protocol%202.1.pdf; https://github.com/ExpertSDR3/TCI
- T3 https://eesdr.com/images/Document/ESDR3/ExpertSDR3_User_Manual_ENG_DX.pdf
- T4 https://eesdr.com/images/Document/ExpertSDR2%201.3.1_Release_Notes.pdf; https://eesdr.com/en/software-en/expertsdr2-en
- T5 TCI 1.6 and earlier: https://github.com/maksimus1210/TCI/blob/cb397507fa/doc/TCI_interface_1.6.pdf; https://github.com/maksimus1210/TCI/blob/cb397507fa/projects_example/TciClient/libs/TciClient/tciclient.h; https://github.com/ExpertSDR3/TCI/blob/5fb1f1aa/Protocol%20TCI.pdf
- T6 https://github.com/jimahlstrom/quisk/blob/78f170eef5/tci.c#L21-L26
- T7 Thetis: https://github.com/ramdor/Thetis/blob/852bf0ef0b/Project%20Files/Source/Console/TCIServer.cs; https://github.com/ramdor/Thetis/blob/852bf0ef0b/Project%20Files/Source/ReleaseNotes.txt#L59-L60; https://github.com/ramdor/Thetis/blob/852bf0ef0b/ReadMe.md; https://github.com/ramdor/Thetis/blob/852bf0ef0b/Project%20Files/Source/ChannelMaster/pipe.c#L190; https://github.com/ramdor/Thetis/blob/852bf0ef0b/Project%20Files/Source/Console/audio.cs#L1279-L1293; https://github.com/ramdor/Thetis/blob/852bf0ef0b/Project%20Files/Source/Console/console.cs#L28476-L28489; https://github.com/ramdor/Thetis/blob/852bf0ef0b/Project%20Files/Source/Console/setup.designer.cs#L30141; https://github.com/ramdor/Thetis/blob/852bf0ef0b/Project%20Files/Source/Console/radio.cs#L1079-L1089; https://github.com/ramdor/Thetis/blob/852bf0ef0b/Project%20Files/Source/ChannelMaster/cmaster.c#L366-L367; https://github.com/ramdor/Thetis/blob/852bf0ef0b/Project%20Files/Source/ChannelMaster/tci.c; https://github.com/ramdor/Thetis/blob/852bf0ef0b/Project%20Files/Source/Console/enums.cs
- T8 https://github.com/ramdor/Thetis/issues/622; https://github.com/ramdor/Thetis/issues/624
- T9 https://github.com/qt/qtwebsockets/blob/dev/src/websockets/qwebsocket_p.cpp#L1514; https://github.com/jtdx-project/jtdx/blob/master/TCITransceiver.cpp#L313
- T10 Thetis forks: https://github.com/mi0bot/OpenHPSDR-Thetis/blob/0cef1c90ff/Project%20Files/Source/Console/TCIServer.cs; https://github.com/TAPR/OpenHPSDR-Thetis/blob/619b13f3d5/Project%20Files/Source/Console/TCIServer.cs#L736; https://github.com/eu2av/OpenHPSDR-Thetis-Enhanced; https://github.com/sv1eia/Thetis-RADE; https://github.com/linoobs/Brick-SDR-Software; https://github.com/kk68/ArtemisSDR
- T11 piHPSDR: https://github.com/dl1ycf/pihpsdr/blob/82aa8c7c5d/src/tci.c; https://github.com/dl1ycf/pihpsdr/blob/82aa8c7c5d/src/tci_audio.c#L137-L149; https://github.com/dl1ycf/pihpsdr/blob/82aa8c7c5d/src/receiver.c#L1139-L1141; https://github.com/dl1ycf/pihpsdr/blob/82aa8c7c5d/README.md; https://github.com/dl1ycf/pihpsdr/blob/82aa8c7c5d/src/hpsdrsim.c
- T12 deskHPSDR: https://github.com/dl1bz/deskhpsdr/blob/d148da70a3/src/receiver.c#L1538-L1582; https://github.com/dl1bz/deskhpsdr/blob/d148da70a3/src/tci.c; https://github.com/dl1bz/deskhpsdr/blob/d148da70a3/src/tci_audio.h#L43-L47
- T13 https://www.sparksdr.com/manual; https://github.com/g0kad/SparkSDR2CAT4OM
- T14 Quisk: https://github.com/jimahlstrom/quisk/blob/78f170eef5/tci.c
- T15 AetherSDR TCI: https://github.com/aethersdr/AetherSDR/blob/141231e4/src/core/TciProtocol.cpp#L465-L472 (init burst); https://github.com/aethersdr/AetherSDR/blob/141231e4/src/core/TciProtocol.cpp#L1379-L1404 (`cmdMute`); https://github.com/aethersdr/AetherSDR/blob/141231e4/src/core/TciServer.cpp#L1647; https://github.com/aethersdr/AetherSDR/blob/141231e4/src/core/TciIoWorker.cpp#L105
- T16 https://github.com/OK1BR/sdr-for-linux/blob/c04150b11b/src/tci_server.c; https://github.com/OK1BR/sdr-for-linux/blob/c04150b11b/docs/TCI-SCOPE.md#L170
- T17 https://github.com/N8SDR1/Lyra-SDR-cpp/blob/083b0f7025/src/tci_server.cpp; https://github.com/boydsoftprez/NereusSDR/blob/eed6b74b44/src/core/TciProtocol.cpp#L699-L714; https://github.com/mikeg-dal/QK4/blob/5191c8b193/docs/tci-server-design.md
- T18 https://github.com/Zeus-SDR/station-engine/blob/b497b5b21b/Station.Engine.Hosting/Tci/TciSession.cs#L1303-L1316; https://github.com/Guru-RF/OpenX6X00/blob/733c425e40/src/tci_server.c#L648-L652; https://github.com/drexjj/zbitx/blob/0490cd6291/src/tci.c#L647-L658
- T19 wfview TCI: https://github.com/wf-group/wfview/blob/0985685467/src/tciserver.cpp#L41; https://github.com/wf-group/wfview/blob/HEAD/include/tciserver.h; https://github.com/wf-group/wfview/blob/HEAD/include/prefs.h
- T20 https://github.com/kd9taw/Nexus/blob/e3b9ef1e68/crates/tempo-tcisim/src/dialect.rs

### FlexRadio

- F1 https://www.flexradio.com/products/categories/radios/
- F2 FlexLib download and terms: https://www.flexradio.com/software/smartsdr-v4-x-api-flexlib
- F3 SmartSDR Software User Guide (Rev v4.1.6): https://edge.flexradio.com/www/offload/20260423063240/SmartSDR-Software-User-Guide.pdf
- F4 https://github.com/flexradio/smartsdr-api-docs/wiki/Discovery-protocol
- F5 https://github.com/flexradio/smartsdr-api-docs/wiki/SmartSDR-TCPIP-API
- F6 https://github.com/flexradio/smartsdr-api-docs/wiki/TCPIP-client
- F7 https://github.com/flexradio/smartsdr-api-docs/wiki/TCPIP-slice
- F8 https://github.com/flexradio/smartsdr-api-docs/wiki/TCPIP-stream
- F9 https://github.com/flexradio/smartsdr-api-docs/wiki/TCPIP-mixer
- F10 https://github.com/flexradio/smartsdr-api-docs/wiki/TCPIP-keepalive
- F11 https://github.com/flexradio/smartsdr-api-docs/wiki/SmartSDR-Supported-Modes
- F12 https://github.com/flexradio/smartsdr-api-docs/wiki/SmartSDR-Status-Responses
- F13 https://github.com/flexradio/smartsdr-api-docs/wiki/SmartSDR-Ethernet-API
- F14 G3WGV API primer: https://g3wgv.com/flexradio/API-Primer-V1.002.pdf
- F15 nDAX (MIT): https://github.com/kc2g-flex-tools/nDAX
- F16 https://www.flexradio.com/developers
- F17 smartsdr-api-docs has no licence file: https://github.com/flexradio/smartsdr-api-docs
- F18 Lead only: https://community.flexradio.com/discussion/5867268/dax-audio-muting
- F19 Aether-gate sim adapter: https://github.com/nigelfenton/Aether-gate (commit 8ec6c4e)
- F20 FlexLib 4.3.16 source, from the F2 zip: ModelInfo.cs, Discovery.cs, Radio.cs, Slice.cs, DAXRXAudioStream.cs, RXRemoteAudioStream.cs, Vita/VitaFlex.cs, Vita/VitaIFDataPacket.cs, Vita/VitaPacketPreamble.cs. Also flexclient's `discovery_unix.go`: https://github.com/kc2g-flex-tools/flexclient at commit 684470b9482932d59b7606e0ae5d903409d33de5
- F21 https://github.com/flexradio/smartsdr-api-docs/wiki/TCPIP-dax (`dax audio set`)

### Rename

- R1 https://doc.qt.io/qt-6/qsettings.html; https://doc.qt.io/qt-6/qstandardpaths.html; https://doc.qt.io/qt-6/qguiapplication.html
- R2 https://docs.github.com/en/repositories/creating-and-managing-repositories/renaming-a-repository
- R3 https://jrsoftware.org/ishelp/topic_setup_appid.htm; https://jrsoftware.org/ishelp/topic_setup_usepreviousappdir.htm; https://jrsoftware.org/ishelp/topic_setup_usepreviousgroup.htm; https://jrsoftware.org/ishelp/topic_installdeletesection.htm; https://jrsoftware.org/ishelp/topic_uninstalldeletesection.htm
- R4 https://www.debian.org/doc/debian-policy/ch-relationships.html; https://wiki.debian.org/RenamingPackages; https://cmake.org/cmake/help/latest/cpack_gen/deb.html
- R5 https://github.com/cli/cli/blob/trunk/pkg/cmd/attestation/verify/policy.go
- R6 Collision checks:
  - Audio products and plugins: https://www.kvraudio.com/product/aether-by-2caudio; https://www.kvraudio.com/product/aether-by-lx-audiolabs; https://www.umbralaudio.com/; https://www.aetherapp.org/; https://www.wallpaper.com/technology/aether-audio-eyewear; https://www.head-fi.org/showcase/kiwi-ears-aether.27951/reviews; https://www.head-fi.org/showcase/audiodream-aether-nexus.27756/; https://atheraudio.com/
  - App stores and package archives: https://apps.apple.com/us/app/aether/id404629109; https://apps.apple.com/us/app/aether-pro-audio-video-camera/id6755774662; https://sources.debian.org/api/src/aethersdr/; https://itunes.apple.com/search; https://play.google.com/store/search?q=aether%20audio&c=apps; https://flathub.org/api/v2/search; https://sources.debian.org/api/search/aetheraudio/; https://apps.microsoft.com/search?query=aether+audio
  - Trademark aggregators: https://uspto.report/TM/85983676; https://www.trademarkelite.com/trademark/trademark-detail/85983675/AETHER; https://www.trademarkelite.com/trademark/trademark-detail/77052959/QUIETR; https://markinton.com/trademark/quietgrid-88880114; https://gleanmark.com/trademark/uspto-87645334
  - Same-niche tools and rejected names: https://clearqso.com/; https://themodernham.com/noise-suppression-ai-and-ham-radio-a-perfect-mix/; https://solidcopy.app; https://clearrig.com

## 12. The TS-990S build (2026-11-22)

The developer now has a Kenwood TS-990S on the bench, connected to a Mac by USB. Its codec is a
PCM2903B (08BB:29B3), and its CAT port is a CP2102 (10C4:EA60). Phases 1, 3, 4 and 5 were built
for that one model, on branch `feature/multi-radio`, one commit per phase. Where this differs
from the plan above:

- **A built-in driver, not Hamlib (the user's decision, section 10, item 0).** `Ts990Cat`
  (`src/radio/Ts990Cat.*`) speaks the TS-990S's own commands from Kenwood's PC Control Command
  Reference. It sends an allowlist only, checked where commands are queued:
  - Reads: `ID;`, `FA;`, `FB;`, `OM0;`, `OM1;` and `AG0;`.
  - One write: `AG0000;` to `AG0255;`.

  Nothing writes at open, so the Hamlib conditions in section 4.3 do not arise. Qt SerialPort
  opens the port with DTR low and RTS high. Dropping RTS too, as v0.1.3 did, kept the radio
  from answering: Kenwood's CAT handshakes with RTS and CTS, per the command reference. The
  TS-990S has no menu setting that keys it from USB DTR or RTS.
- **A port another program holds is never opened.** On the bench, v0.1.3 opened the port
  alongside RUMlogNG. The logger read the radio's answers, and the app's baud and RTS settings
  changed under it. `otherProgramsUsing()` now names any holder (libproc on macOS, `/proc` on
  Linux; Windows refuses a second open itself), and the driver waits for the port to be free.
  Sharing stays phase 6. The Radio page's Test sends `ID;` alone.
  Hamlib stays the plan for further models: a second built-in driver is written only for a
  model that is on the bench.
- **No transmit read.** The TS-990S's `IF;` always reports P8 as 0, and pushes would need an
  `AI` write. So the chain's transmit bypass does not follow this radio. It is receive-only as
  far as this app goes.
- **Mode names** are the radio's own (`CW-R`, `FSK`, `PSK`, `USB-D1` and so on). `modeBypassesNr`
  learned them. There is no `ModeClass` yet.
- **The mute key** is `usb:<model>`, not the bridge serial or the port. One radio can show up
  under more than one port name: on a Mac its CP2102 has one for each driver, and Windows may
  renumber a COM port. An AF owed back must be found whichever name is chosen. Two radios of
  the same model on one computer would share a record, which is accepted.
- **The operator's override lasts longer** for a level than for a switch. It holds until the
  setting is chosen again or a new session starts (`MuteController::rearm()`). A USB radio's
  silence guard can stop and restart this app's audio within a session, and that must not
  turn the AF back down over the operator's knob.
- **A failed engage keeps its record** for a level: its sets may have landed unseen. A later
  reading above 0 clears the record and sends nothing.
- **Sets never undo each other.** A newer AF set replaces one not yet sent. As a session
  closes, a pending restore (a level above 0) is written out, and the port is drained before
  it closes.
- **`MuteController` stays in `src/tci/`.** It gained `Actions::sendLevel`,
  `Ownership::priorLevel` and `onServerLevel()`. A level read above 0 at engage is taken as
  the prior, and 0 never is.
- **AF is read on every poll** while the model has `afMute`, even with the setting off. A
  record owed from a crash is then restored on the next connection without the setting.
- **`muteVerified` is `afMuteVerified`** in `UsbModels`, and is still false until the bench
  test in docs/RELEASING.md passes.

