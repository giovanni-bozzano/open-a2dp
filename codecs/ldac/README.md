# LDAC encoder (vendored)

Vendored from AOSP `platform/external/libldac` at commit
`eeee1a3f5f8df1282e3a6d297085885fd886737b` (`refs/heads/main`, 2026-11-12). The drop
provides the encoder this project uses.

- Copyright (C) 2003 - 2016 Sony Corporation, Apache-2.0. Every file keeps its own licence
  header; `LICENSE` and `NOTICE` are the drop's own. The `NOTICE` states that Sony's LDAC
  certification is required to use LDAC in a product; Apache-2.0 does not grant it. See
  [codec licensing](../../docs/architecture/Codec-Licensing.md).
- The sources are unmodified apart from line endings. Do not patch them: fixes belong upstream.
  What MSVC lacks is supplied from outside the drop by `compat/msvc.h` (force-included, so
  `__attribute__` compiles away) and `compat/unistd.h` (the drop includes it and calls nothing
  from it). Those two files are this repository's, not Sony's.
- Android.bp is the build's own record of what actually compiles, and it is only three
  translation units: `src/ldaclib.c` and `src/ldacBT.c` pull the rest of `src` in as includes,
  and `abr/src/ldacBT_abr.c` is the adaptive bitrate helper. Compiling every `.c` in `src`
  compiles two mutually exclusive variants and does not link.
- `SHA256SUMS` records the LF form of every vendored file, so a CRLF checkout verifies
  unchanged. `scripts/check-vendored-encoders.ps1` checks all three drops and exits
  non-zero on a change; `-Update` rewrites the manifests after a deliberate re-vendor.
- The library offers `ldacBT_*` (encoder handle, EQMID quality modes, PCM format) and the
  `abr` helpers. It encodes 16, 24 and 32-bit PCM at 44.1, 48, 88.2 and 96 kHz in stereo or
  dual channel; the A2DP capability IE and the encoder wrapper are this repository's, not part
  of the drop.
