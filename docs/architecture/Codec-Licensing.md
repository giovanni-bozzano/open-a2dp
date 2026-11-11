# Codec licensing

What this product ships, what each codec obliges a distributor to do, and why the line sits
where it is. This is the decision record for anyone distributing a build, not legal advice:
a distributor confirms the position with counsel.

## What ships

| Codec | Encoder | Licence position |
|---|---|---|
| SBC | `codecs/sbc`, this repository's | none: the patents have expired |
| AAC-LC | Windows Media Foundation's encoder, driven from `codecs/aac` | no encoder code is distributed, so the obligation stays with the encoder in the OS |
| aptX | Qualcomm's AOSP encoder, vendored in `codecs/aptx/classic` | Apache-2.0 with its patent grant: notices only |
| aptX HD | Qualcomm's AOSP encoder, vendored in `codecs/aptx/hd` | Apache-2.0 with its patent grant: notices only |
| aptX LL, aptX Adaptive, aptX Lossless | — | outside the AOSP grant, so not shipped |

`LICENSE` is Apache-2.0 and settles copyright and contributed patents for this repository. It
does not cover the codecs; those obligations are the ones below.

## aptX and aptX HD

Qualcomm contributed `encoder_for_aptx` and `encoder_for_aptxhd` to AOSP (`system/embdrv/`)
in 2023 under Apache-2.0, stating that the contribution exists so the encoders can be
distributed as part of finished products. Apache-2.0 carries an express patent grant from the
contributor, which is why the vendored encoders are the only aptX route this project takes.
Two limits come with the grant:

- It covers the encoder code, not the mark. Nothing here claims aptX certification.
- It covers aptX and aptX HD only. aptX LL, aptX Adaptive, aptX Lossless and the decoders
  need a licence direct from Qualcomm, so none of them is in this tree.

aptX LL is the near miss worth naming: it is classic aptX with smaller packets and its own
vendor-specific signalling, and the same encoder API produces it. Encoding it would still be
using an aptX product outside the grant.

The vendored files are unmodified from AOSP commit
`745ee92b87ed62b5a8a1bac47d5df8cad623bf59` and keep their Apache-2.0 text beside them. Three
details make the shipped encoders correct rather than licensed, and are code obligations
described in [Protocol and codecs](Protocol-and-Codecs.md#pcm-depth-and-24-bit-aptx-hd):

1. aptX HD takes 24-bit samples, so the endpoint lists 24-in-32 and the ring carries that
   depth; expanding 16-bit input would feed zeroed low bits to the predictor.
2. The vendor-specific capability IE and its vendor and codec IDs come from the AOSP headers,
   never from a reverse-engineered library.
3. Classic aptX is sent without an RTP header and aptX HD inside one, so packetization is per
   codec.

## AAC

No AAC encoder implementation is in this repository. `codecs/aac/AacCodec.cpp` drives
`CLSID_AACMFTEncoder`, the encoder Windows ships, and what this tree owns is the capability
negotiation and the LATM framing (`LatmWriter`).

That is the decision: the package calls the OS encoder and distributes none of it. Via LA's
AAC programme licenses "manufacturers or developers of end-user encoder and/or decoder
products" (US$0.98 per unit for the first 500,000 a year, tiering down to US$0.10, with no
fee on bitstreams), and its policy text is written against encoder products rather than their
callers, while Microsoft licenses the implementation inside Windows. A distributor should
treat that as the position it is rather than as a written clearance, and ship the notice.

Adding an in-tree or third-party AAC encoder would put the pool licence on the product and
needs its own decision before any of that code lands.

## What a distributor must do

- Ship `LICENSE`, an Apache-2.0 notice naming AOSP and the vendored encoder files, and the
  AAC notice for the Windows encoder this package calls.
- Ship no aptX LL, Adaptive or Lossless encoder, no aptX decoder and no licensed codec DLL:
  binaries are deployed, never committed.
- Add a third-party notice entry naming vendor, version and licence before adding any other
  codec provider.
- Keep codec obligations here; the repository licence does not extend to them.
