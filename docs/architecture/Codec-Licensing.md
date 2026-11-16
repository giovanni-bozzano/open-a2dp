# Codec licensing

[Architecture index](README.md) | [Root licence](../../LICENSE)

OpenA2DP's own code is Apache-2.0. The codecs are separate: each encoder carries
its own licence, and the repository licence does not replace it.

## What ships

| Codec | Encoder | Licence |
|---|---|---|
| SBC | This repository's encoder in `codecs/sbc` | Apache-2.0; the SBC patents have expired |
| AAC-LC | Windows Media Foundation, called from `codecs/aac` | No AAC encoder code or binary is distributed by this project |
| aptX | Qualcomm's AOSP encoder, vendored in `codecs/aptx/classic` | Apache-2.0, including its patent grant |
| aptX HD | Qualcomm's AOSP encoder, vendored in `codecs/aptx/hd` | Apache-2.0, including its patent grant |
| aptX LL | The classic aptX encoder, reused under the LL identity | Apache-2.0, including its patent grant |
| LDAC | Sony's AOSP encoder, vendored in `codecs/ldac` | Apache-2.0 |

The vendored aptX, aptX HD and LDAC encoder sources are unmodified from AOSP and
keep their licence files beside the sources. `scripts/check-vendored-encoders.ps1` verifies
their hashes, so a change in those trees is visible before it ships.

## Notices for distribution

Distribution requires:

- `LICENSE`, the Apache-2.0 licence for this repository.
- The Apache-2.0 notice and licence text for the vendored aptX, aptX HD and LDAC
  encoders, naming AOSP as their source.
- A notice that AAC is encoded by Windows Media Foundation.

The encoder licences cover the encoder code and the patent grants they carry.
They do not grant codec certification or the use of a vendor's marks.

## Marks and certification

- **aptX** and **aptX HD** are Qualcomm marks. This project uses the encoders
  Qualcomm contributed to AOSP and does not claim aptX certification.
- **LDAC** is a Sony mark. LDAC certification is separate from the source licence
  and is not granted by it. This project does not claim LDAC certification.
- **AAC** is a trademark of its owners. This project calls the encoder Windows
  provides and distributes no AAC implementation.

## Adding a codec

A new encoder brings its own source licence, patent position and notice. Add them
with the code, not after it.

The authoritative texts are with the code: `codecs/aptx/classic/LICENSE-APACHE-2.0.txt`,
`codecs/aptx/hd/LICENSE-APACHE-2.0.txt`, `codecs/ldac/LICENSE` and the repository
[LICENSE](../../LICENSE).
