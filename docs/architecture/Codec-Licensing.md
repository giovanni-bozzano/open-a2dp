# Codec licensing

What this repository may contain per A2DP codec, and what a distributor of a built
product must license. This is engineering guidance, not legal advice: confirm with
counsel before shipping anything that encodes AAC or the aptX family.

| Codec | What ships | Shipping it needs |
|---|---|---|
| SBC | this repository's encoder | Nothing (patents long expired) |
| AAC-LC | Windows Media Foundation's encoder, driven from `codecs/aac` | pool licence only if an encoder implementation is distributed |
| aptX, aptX HD | Qualcomm's Apache-2.0 encoders, available in AOSP | notices; no licence fee |
| aptX LL, aptX Adaptive, aptX Lossless | nothing | Qualcomm aptX software licence |

The repository itself is Apache-2.0 (`LICENSE`), which settles copyright and contributor
patents only. None of the licences below are triggered by publishing source; they are
triggered by distributing binaries.

## aptX, aptX HD, aptX Low Latency

Qualcomm's general position is that commercial use of aptX requires a software licence, and
most of the family is only available as licensed libraries. There are two significant
exceptions and one hard limit:

- **The aptX and aptX HD encoders are open source.** In 2023 Qualcomm contributed
  `encoder_for_aptx` and `encoder_for_aptxhd` to AOSP (`system/embdrv/`), Apache-2.0, and
  said the purpose was "to enable people to distribute the encoders as part of their
  finished products". Apache-2.0 carries a patent grant from the contributor, so this is
  materially stronger ground than a reverse-engineered implementation. Conditions: keep the
  Apache notices, and remember that the contribution covers the encoder code, not the mark.
- **Everything else still needs Qualcomm**: aptX LL, aptX Adaptive, aptX Lossless, the
  decoders, and use of "aptX" as a certification brand. Qualcomm's statement is explicit
  that "all other aptX products require a license direct from Qualcomm".
- **aptX LL is technically classic aptX** with smaller packets and its own vendor-specific
  signalling, which is why the reverse-engineered libraries offer LL from the same code.
  Encoding LL frames with the AOSP classic encoder would still be using an aptX product
  outside the grant, so LL stays out until there is a licence.

Engineering fit for this repository: the AOSP encoders are self-contained C
(`aptXbtenc.c`, `QmfConv.c`, `ProcessSubband.c`, `AptxTables.h`), expose
`aptxbtenc_init` / `aptxbtenc_encodestereo`, take 16-bit (classic) or 24-bit (HD) stereo PCM
and keep roughly 1.6 KB
of state per channel, so they belong in the user-mode service beside AAC. HD is vendored at
`codecs/aptx/hd/` and classic aptX at `codecs/aptx/classic/`, both from AOSP commit
`745ee92b87ed62b5a8a1bac47d5df8cad623bf59` and unmodified there (Apache-2.0 text beside
each); three details have to be right:

1. 24-bit input. HD takes 24-bit samples, so the endpoint must list 24-in-32 and carry it
   through the ring: there is no 16->24 expansion step, which would feed zeroed low bits to
   the predictor ([PCM depth](Protocol-and-Codecs.md#pcm-depth-and-24-bit-aptx-hd)).
2. The vendor-specific capability IE and its vendor and codec IDs. Take them from the
   Apache-2.0 AOSP headers; do not guess, and do not copy them from a reverse-engineered
   library.
3. Packetization. AOSP's classic aptX path writes no RTP header, while aptX HD is
   encapsulated in RTP; the media path needs that distinction per codec.

The alternatives, and what each is actually worth:

| Implementation | Licence | aptX | HD | LL | Patent grant | Binary |
|---|---|---|---|---|---|---|
| AOSP `encoder_for_aptx*` | Apache-2.0 | yes | yes | no | yes, from contributor | yes, with notices |
| libfreeaptx | LGPL-2.1+ | yes | yes | via parameters | no | yes, LGPL relink duties |
| FFmpeg `aptxenc`/`aptxhdenc` | LGPL-2.1+ | yes | yes | no | no | yes, LGPL relink duties |
| libopenaptx (pali) | GPL-3.0+ | yes | yes | via parameters | no | no, would force GPL |
| Android vendor blob | proprietary | yes | yes | yes | n/a | no |
| Qualcomm SDK | commercial | yes | yes | yes | contractual | on their terms |

`libfreeaptx` and FFmpeg are the same code lineage (FFmpeg 4.0), so their differences are
licence text and packaging rather than quality. The AOSP encoders are the only option with
a patent grant, and the only one that does not depend on reverse engineering.

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
| Driver + AOSP aptX/HD encoders | aptX and aptX HD, Apache-2.0 | shippable with notices; no aptX LL |
| Driver + Qualcomm SDK | aptX family DLL | Qualcomm licence, redistribution on their terms |
| Driver + `libfreeaptx` | reverse-engineered aptX | LGPL code without a patent grant; upstream warns against binary redistribution |

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
- Android: ships Qualcomm's aptX and aptX HD encoders from AOSP (Apache-2.0) and licenses
  the rest; the pool's own FAQ says its licence does not cover Android OEMs, who license
  AAC themselves.

The pattern: an in-tree aptX encoder is now possible for aptX and HD only, AAC encoders are
either licensed, stripped to the free profiles, or left out, and aptX LL has no open route.

## Repository rules

- Vendored codec code keeps its own licence and headers. AOSP files are Apache-2.0, like
  this repository, and need a NOTICE entry naming the source and the licence.
- No binary blobs, including licensed codec DLLs; they are deployed, not committed.
- Adding a licensed provider requires a third-party notice entry naming the vendor, the
  version and the licence that covers redistribution.
- `LICENSE` is Apache-2.0. Keep the codec obligations in this document rather than
  implying in the README that it covers codec patents or third-party code.

## Order of work

1. Repository licence: done, Apache-2.0.
2. Decide the AAC stance for the first release: service included with a notice, or
   driver-only SBC. The code supports both today.
3. Vendor the AOSP aptX and aptX HD encoders (Apache-2.0, notices) behind a user-mode
   provider seam, add the vendor-specific IE and the per-codec packetization, and gate the
   codec preference on the provider being present.
4. Add the 24-bit path so aptX HD has something to encode; 16-bit input would make the
   feature mostly cosmetic.
5. aptX LL, Adaptive and Lossless stay out until a Qualcomm licence exists.
6. Validate on licensed hardware; no test rig here can prove an aptX stream end to end, so
   the tests must cover framing and packetization against a reference sink fixture.
