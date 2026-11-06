# Releasing

[`release.yml`](../.github/workflows/release.yml) builds every platform and publishes a GitHub
Release. It publishes only if every platform built, passed its tests and smoke tests, so a partial
release never goes out.

**Releases are currently unsigned on Windows and macOS, by choice.** Both warn or refuse on first
run, and the release notes tell people what to do. The Mac signing path is still in place: see
below.

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
| macOS 13+ (Apple Silicon) | `-macos-arm64.dmg` | **Ad-hoc only for now**, see below. With the secrets set: Developer ID, hardened runtime, notarized and stapled, on both the app and the DMG |
| Windows 10+ (x64) | `-windows-x64-setup.exe`, `-windows-x64-portable.zip` | **Not Authenticode signed**, see below. Sigstore provenance once public |
| Raspberry Pi (64-bit OS) | `-raspberrypi-arm64.deb`, `.AppImage` | Sigstore provenance once public |
| Linux x86-64 | `-linux-x86_64.deb`, `.AppImage` | Sigstore provenance once public |

Plus `SHA256SUMS`.

Linux has no equivalent of notarization, so a signature tying each file to this repository and
workflow run is what signing can mean there. It needs no key to manage, because GitHub's
identity signs it through Sigstore, and anyone can check it:

```sh
gh attestation verify <file> --repo VU3ESV/ExpertSDR3-Audio
```

**The repository is private, and this step only runs on a public one,** because GitHub requires
its Enterprise plan for attestations on private repositories. So for now these files have
checksums only, and the step starts working by itself on the first release after the repository
goes public. Also, while it is private, only people with access to it can download a release.

## Costs while private

Actions is free on a public repository. On a private one it draws on the plan's monthly minutes,
reportedly with macOS billed at 10x and Windows at 2x. A full run takes about 5 minutes on macOS,
17 on Windows and 25 across Linux, so about 110 billed minutes. Merges to `main` each cut a
release, and every pull request runs CI, so both spend this automatically.

To fix one platform without paying for the others:

```sh
gh workflow run ci.yml --ref <branch> -f platforms="linux windows"
```

## macOS signing secrets

These are optional. With none set, the Mac build is ad-hoc signed and macOS refuses to open it
until its quarantine flag is cleared. Setting them turns on Developer ID signing and notarization
with no other change.

They are the same six secrets as LP-700-App (its `docs/SIGNING-SECRETS.md`), with the same meaning.
Each has to be set on this repository too, since GitHub never lets a secret be read back:

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

It is all or nothing: if any are set, a missing or wrong one stops the release, rather than
quietly shipping a build that is signed but not notarized. Notarization has never run yet, so the
first signed release is also its first test.

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
- **The DeepFilterNet library is AetherSDR's prebuilt one,** checked against the SHA-256 pinned in
  its setup script, and built from source if unavailable.
