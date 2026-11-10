# Codec licensing

What this repository may contain per A2DP codec, and what a distributor of a built
product must license. This is engineering guidance, not legal advice: confirm with
counsel before shipping anything that encodes AAC or the aptX family.

| Codec | Specification | Shipping it needs |
|---|---|---|
| SBC | A2DP 12.5/12.6, public | Nothing (patents long expired) |
| AAC-LC | ISO/IEC 14496-3, public | AAC patent licence per end-user product |
| aptX, aptX HD, aptX LL | Qualcomm proprietary, not published | Qualcomm aptX software licence + SDK |

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
specification, so the source itself is unencumbered by copyright. Patents are the issue:
Via LA's AAC programme states that a licence is needed by "manufacturers or developers
of end-user encoder and/or decoder products", with per-unit fees on encoders and
decoders and no fees for distributing bitstreams.

Consequences for this project:

- Keeping the encoder in this repository is fine; the repository itself is not an
  end-user product.
- Anyone building a product that ships the encoder needs the pool licence (or bilateral
  licences) for the units they sell. The README should say so, so a downstream user does
  not assume the source licence covers the patents.
- Using Windows' Media Foundation encoder does not change that arithmetic for a product
  that encodes AAC; it only changes who wrote the code.
- The AAC encoder was written for this tree and remains the reference for the SBC/AAC
  negotiation tests; that is independent of any licensing decision.

## Repository rules

- No third-party codec source is copied in. Any exception needs a compatible licence,
  attribution, and a note in the file.
- No binary blobs, including licensed codec DLLs; they are deployed, not committed.
- Adding a licensed provider requires a third-party notice entry naming the vendor, the
  version and the licence that covers redistribution.
- This repository has no `LICENSE` file yet. Until one is chosen and committed, the
  default is all rights reserved, which conflicts with the "open source" claim in the
  README; decide that before publishing, and check it against any SDK terms.

## Order of work

1. Choose and commit the repository licence; check it against an SDK that forbids
   static linking into the driver.
2. Obtain the aptX SDK and licence, and confirm it permits a user-mode DLL loaded at
   runtime with the binary redistributed separately.
3. Add the provider seam in `codecs/` (interface plus loader) once the SDK's API is
   known; the interface is shaped by that API, not designed ahead of it.
4. Extend the codec preference, ABI and settings window with the family only behind a
   provider-present check, and add a mock provider for negotiation tests.
5. Validate on licensed hardware; no test rig here can prove an aptX stream because the
   encoder cannot be present.
