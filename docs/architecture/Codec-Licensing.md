# Codec licensing

What this repository may contain per A2DP codec, and what a distributor of a built
product must license. This is engineering guidance, not legal advice: confirm with
counsel before shipping anything that encodes AAC or the aptX family.

| Codec | Specification | Shipping it needs |
|---|---|---|
| SBC | A2DP 12.5/12.6, public | Nothing (patents long expired) |
| AAC-LC | ISO/IEC 14496-3, public | AAC patent licence per end-user product |
| aptX, aptX HD, aptX LL | Qualcomm proprietary, not published | Qualcomm aptX software licence + SDK |

The repository itself is Apache-2.0 (`LICENSE`), which settles copyright and contributor
patents only. None of the licences below are triggered by publishing source; they are
triggered by distributing binaries of an encoder product.

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

The compliant shape, if a licence is obtained:

1. The encoder stays in user mode beside the AAC encoder. A closed binary cannot live in
   the kernel path, and the SDK targets user mode.
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

AAC is standardized, and the in-tree LC encoder is written against the public
specification, so the source itself is unencumbered by copyright. Patents are separate:
Via LA's AAC programme states that a licence is needed by "manufacturers or developers of
end-user encoder and/or decoder products", at a published per-unit rate (US$0.98 for the
first 500,000 units a year, falling through the tiers to US$0.10) with no fee on
distributed bitstreams. The programme is current, not legacy: it added licensees through
2026 and revises its patent list quarterly.

Expiry is the usual argument for ignoring it, so record the nuance rather than repeat the
argument. Core MPEG-2/4 LC patents have expired in some jurisdictions, and Fedora's legal
review treats the US LC-AAC patents as expired while stripping HE and HEv2 from its
`fdk-aac-free` package for patent reasons. Via LA still licenses the family including LC,
and later pool entries run to 2029 and 2030 in other jurisdictions. Writing and publishing
this encoder is therefore fine; whether a distributor owes royalties depends on the
profiles enabled, the countries and the dates.

Consequences for this project:

- Keeping the encoder in this repository is fine; the repository is not an end-user
  product.
- Apache-2.0 grants patent rights held by the contributors — it has a patent clause — so
  the source licence and the patent licence are orthogonal: a downstream builder still needs the pool licence (or
  bilateral licences) for the units they sell.
- Publishing the source is not what the pool charges for; distributing an encoder binary,
  including a signed driver package of your own, is.
- If that position needs to be softer, the Fedora precedent is the fallback: keep the
  LC-only build, state the position in the README, and do not distribute binaries until
  licensed.

## How others handle it

- Windows inbox (`BthA2dp`): Microsoft's own table gives aptX Classic from Windows 10
  1507, AAC from Windows 11 21H2, and aptX Adaptive from Windows 11 24H2 on Qualcomm
  radios. Microsoft holds whatever licences those need; a replacement driver inherits
  none of them, so "Windows already does AAC" is not an argument available here.
- Alternative A2DP Driver (`bluetoothgoodies.com`, Luculent Systems): closed source, sold
  per machine, with SBC, aptX, aptX HD, aptX LL and LDAC in the base edition and **AAC
  only in a separate paid "with AAC CODEC support" edition licence**. That is the shape a
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
2. Obtain the aptX SDK and licence, and confirm it permits a user-mode DLL loaded at
   runtime with the binary redistributed separately.
3. Add the provider seam in `codecs/` (interface plus loader) once the SDK's API is
   known; the interface is shaped by that API, not designed ahead of it.
4. Extend the codec preference, ABI and settings window with the family only behind a
   provider-present check, and add a mock provider for negotiation tests.
5. Validate on licensed hardware; no test rig here can prove an aptX stream because the
   encoder cannot be present.
