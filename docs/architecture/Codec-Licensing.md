# Codec licensing

What this repository may contain per A2DP codec, and what a distributor of a built
product must license. This is engineering guidance, not legal advice: confirm with
counsel before shipping anything that encodes AAC or the aptX family.

| Codec | What ships | Shipping it needs |
|---|---|---|
| SBC | this repository's encoder | Nothing (patents long expired) |
| AAC-LC | Windows Media Foundation's encoder, driven from `codecs/aac` | pool licence only if an encoder implementation is distributed |
| aptX, aptX HD, aptX LL | nothing; Qualcomm proprietary | Qualcomm aptX software licence + SDK |

The repository itself is Apache-2.0 (`LICENSE`), which settles copyright and contributor
patents only. None of the licences below are triggered by publishing source; they are
triggered by distributing binaries.

## aptX, aptX HD, aptX Low Latency

Qualcomm states that commercial use of aptX requires a software licence agreement, and
the encoders are delivered as Qualcomm libraries with the licensed SDK, not as a
published bitstream specification. There is therefore nothing to implement against:

- A clean-room encoder is not feasible without the specification; the only public
  implementations are reverse engineered (FFmpeg's LGPL aptX codecs are decoders).
  Bringing that code here would import LGPL obligations into a repository that ships a
  kernel driver, and it would still not grant Qualcomm's rights in the codec.
- Guessing the vendor-specific negotiation is worse than not supporting it: the exact
  vendor ID, codec ID and payload layout are Qualcomm's, not AVDTP's, and a wrong value
  negotiates a stream the sink cannot decode.
- Windows is no help here: its inbox aptX encoder lives inside `BthA2dp` and is not
  exposed to third parties. The documented extension is the A2DP offload SIOP, which
  requires the codec to run in the audio DSP or the controller, not in a user-mode
  encoder. There is no Media Foundation equivalent of `CLSID_AACMFTEncoder` for aptX.

The compliant shape, if a licence is obtained:

1. The encoder stays in user mode beside the AAC path. A closed binary cannot live in
   the kernel, and the SDK targets user mode.
2. The SDK's DLL is loaded at runtime from a path outside this repository. The tree keeps
   only the provider interface and the loader; no Qualcomm headers, no binary blobs.
3. Negotiation is A2DP "vendor specific" (Media Codec Type 0xFF) with a vendor- and
   codec-specific payload supplied by the provider. The provider owns those bytes; the
   driver's SBC/AAC paths stay codec-agnostic.
4. The names "aptX", "aptX HD" and "aptX Low Latency" are Qualcomm trademarks: the
   settings window may only show them when a licensed provider is actually present.
5. aptX LL is a mode of the family, not a separate algorithm: it needs the same encoder
   with smaller packets. Whether a build supports it is the provider's answer, not ours.

Until then, the codec preference must not list the family, because a sink that offers
only aptX has to fail cleanly with "the sink does not advertise SBC or AAC".

## AAC

The repository contains no AAC encoder implementation. `codecs/aac/AacCodec.cpp` drives
`CLSID_AACMFTEncoder`, the encoder Microsoft ships in Windows, and what this tree owns is
the IE negotiation and the LATM framing around it (`LatmWriter`). That distinction decides
the obligation:

- **An encoder you wrote** is the case the pool licenses. Via LA's AAC programme requires a
  licence from "manufacturers or developers of end-user encoder and/or decoder products",
  at US$0.98 per unit for the first 500,000 a year, falling through the tiers to US$0.10,
  with no fee on distributed bitstreams. The programme is active, not legacy: it added
  licensees through 2026 and revises its patent list quarterly.
- **A program that calls the Windows encoder** distributes no encoder code, and Microsoft
  licenses the implementation it puts in the OS. The pool's policy text is written against
  encoder/decoder products rather than their callers, and the settled practice for Windows
  software is to use the OS encoder. Treat that as risk reduction, not as written
  permission: the pool's own reading is that a product able to encode AAC needs a licence.
- **Expiry** is the third argument. Core LC patents have expired in some jurisdictions;
  Fedora's legal review treats the US LC patents as expired and strips HE/HEv2 from
  `fdk-aac-free` for patent reasons, while later pool entries run to 2029 and 2030
  elsewhere. Publishing source is unaffected either way; royalties depend on the profiles,
  the countries and the dates.

Consequences for this project:

- Apache-2.0 grants patent rights held by the contributors, not third-party codec patents,
  so the source licence and any patent licence are orthogonal.
- Publishing the source needs no licence. Distributing a binary containing an encoder you
  wrote does. Distributing one that drives the Windows encoder is the middle case above.
- If a release has to be unambiguous, ship SBC-only; see the next section.

## Distribution options

| Package | Contains | Position |
|---|---|---|
| Driver + service (today) | SBC encoder, AAC through the Windows encoder | no AAC implementation shipped; pool licence arguable, common practice is to ship with a notice |
| Driver only | SBC encoder | nothing to license; AAC is never selected because it requires the encoder service |
| Driver + licensed provider | aptX family DLL from the Qualcomm SDK | Qualcomm licence, redistribution on their terms |
| Driver + `libfreeaptx` | reverse-engineered aptX | LGPL code, but an unlicensed implementation of a proprietary codec; upstream forbids binary redistribution |

### Disabling AAC

Two real ways and one that only looks like one:

- **Ship without `OpenA2dpSvc.exe`** (driver-only install). AAC is selected only when the
  encoder service is registered — `SelectConfig` in `SignalingWorkItem.cpp` gates the
  automatic order and the forced-AAC path on `ServiceRegistered` — so the package is
  SBC-only with no code change. The AAC SEP is still advertised (see the comment in
  `ResetSession` about the registration race), and the sink may offer AAC; selection then
  falls back to SBC.
- **Build without AAC.** Skip `SetLocalSep(kLocalAacSeid, ...)` in `ResetSession` under a
  build define and leave the service out of the package, so a sink is never offered a codec
  the package cannot deliver.
- **Not a disable.** A registry or UI preference inside the full package: the encoder code
  and the capability still ship, which is exactly what the licence question is about.

## How others handle it

- Windows inbox (`BthA2dp`): Microsoft's own table gives aptX Classic from Windows 10
  1507, AAC from Windows 11 21H2, and aptX Adaptive from Windows 11 24H2 on Qualcomm
  radios. Microsoft holds whatever licences those need; a replacement driver inherits none
  of them, so "Windows already does AAC" is not an argument available here — but calling
  Microsoft's encoder is a different act from shipping one, as above.
- Alternative A2DP Driver (`bluetoothgoodies.com`, Luculent Systems): closed source, sold
  per machine, with SBC, aptX, aptX HD, aptX LL and LDAC in the base edition and **AAC only
  in a separate paid "with AAC CODEC support" edition licence**. That is the shape a
  per-unit pool licence imposes, and it is the closest comparable product to this one.
- Linux (BlueZ, PipeWire): uses `libfreeaptx` (LGPL-2.1+, reverse engineered from FFmpeg)
  for aptX, aptX HD and aptX LL, and Sony's `ldacBT` (Apache-2.0) for LDAC. Debian ships
  `libfreeaptx` in main despite its provenance; Debian keeps `fdk-aac` out of main as
  non-free, and Fedora ships `fdk-aac-free` with the HE profiles stripped for patent
  reasons. The community accepts the aptX risk and avoids shipping an AAC encoder.
- Android: ships Qualcomm aptX binaries under licence, and the pool's own FAQ says its
  licence does not cover Android OEMs, who license AAC themselves.

The pattern is consistent: nobody has a lawful in-tree aptX encoder without Qualcomm, and
AAC encoders are either licensed, stripped to the free profiles, or left out.

## Repository rules

- No third-party codec source is copied in. Any exception needs a compatible licence,
  attribution, and a note in the file.
- No binary blobs, including licensed codec DLLs; they are deployed, not committed.
- Adding a licensed provider requires a third-party notice entry naming the vendor, the
  version and the licence that covers redistribution.
- `LICENSE` is Apache-2.0. Keep the codec obligations in this document rather than
  implying in the README that it covers codec patents.

## Order of work

1. Repository licence: done, Apache-2.0.
2. Decide the AAC stance for the first release: service included with a notice, or
   driver-only SBC. The code supports both today.
3. Obtain the aptX SDK and licence, and confirm it permits a user-mode DLL loaded at
   runtime with the binary redistributed separately.
4. Add the provider seam in `codecs/` (interface plus loader) once the SDK's API is
   known; the interface is shaped by that API, not designed ahead of it.
5. Extend the codec preference, ABI and settings window with the family only behind a
   provider-present check, and add a mock provider for negotiation tests.
6. Validate on licensed hardware; no test rig here can prove an aptX stream because the
   encoder cannot be present.
