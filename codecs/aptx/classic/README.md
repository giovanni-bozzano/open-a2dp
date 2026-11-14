# aptX encoder (vendored)

Vendored from AOSP `platform/packages/modules/Bluetooth`, directory
`system/embdrv/encoder_for_aptx/`, at commit `745ee92b87ed62b5a8a1bac47d5df8cad623bf59`
(2026-11-10). The sibling directory `../hd` holds the aptX HD drop from the same commit.

- Copyright (C) 2022 The Android Open Source Project, Apache-2.0. Every file keeps its own
  licence header; the licence text is in `LICENSE-APACHE-2.0.txt`.
- The sources are unmodified apart from line endings, and are compiled with warnings as
  errors turned off for that reason. Do not patch them here: fixes belong upstream.
- `AptxCodec.cpp`, `AptxStreamEncoder.cpp` and `../../include/OpenA2dp/AptxCodec.h` are this
  repository's wrapper, not part of the vendored drop.
- The helper files share their names with the HD drop but are different code (the HD set is
  the 24-bit `_HD` variants), so both trees are compiled. Quoted includes keep each drop on
  its own headers.
- `SHA256SUMS` records the LF form of every vendored file, so a CRLF checkout verifies
  unchanged. `scripts/check-vendored-encoders.ps1` checks all three drops and exits
  non-zero on a change; `-Update` rewrites the manifests after a deliberate re-vendor.

Qualcomm contributed these encoders to AOSP under Apache-2.0 with a contributor patent
grant; see [codec licensing](../../../docs/architecture/Codec-Licensing.md). Distributing
binaries also requires shipping the Apache-2.0 licence text, and aptX branding or
certification stays with Qualcomm.
