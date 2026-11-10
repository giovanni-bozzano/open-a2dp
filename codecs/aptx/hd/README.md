# aptX HD encoder (vendored)

Vendored from AOSP `platform/packages/modules/Bluetooth`, directory
`system/embdrv/encoder_for_aptxhd/`, at commit `745ee92b87ed62b5a8a1bac47d5df8cad623bf59`
(2026-11-10). The sibling directory `../classic` holds the classic aptX drop from the same
commit.

- Copyright (C) 2022 The Android Open Source Project, Apache-2.0. Every file keeps its own
  licence header; the licence text is in `LICENSE-APACHE-2.0.txt`.
- The sources are unmodified apart from line endings, and are compiled with warnings as
  errors turned off for that reason. Do not patch them here: fixes belong upstream.
- `AptxHdCodec.cpp`, `AptxHdStreamEncoder.cpp` and `../../include/OpenA2dp/AptxHdCodec.h` are
  this repository's wrapper, not part of the vendored drop.
- `SHA256SUMS` records the LF form of every vendored file, so a CRLF checkout verifies
  unchanged. `scripts/check-vendored-aptx.ps1` checks it and exits non-zero on a change;
  `-Update` rewrites it after a deliberate re-vendor.

Qualcomm contributed these encoders to AOSP under Apache-2.0 with a contributor patent
grant, which is why they are the only aptX implementation this project can ship; see
[codec licensing](../../../docs/architecture/Codec-Licensing.md). Distributing binaries also
requires shipping the Apache-2.0 licence text, and aptX branding or certification stays with
Qualcomm.
