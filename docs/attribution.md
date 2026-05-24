# Attribution

libchromadec extracts and consolidates work originally authored across several
upstream projects. This page lists the primary contributors whose work is
preserved here. The repo's `git log` and `.mailmap` are the authoritative
source — this page summarises.

## Original tools/ld-chroma-decoder and tools/library code

The git history extracted from `ld-decode` via `git filter-repo` preserves the
authorship of every commit that touched `tools/ld-chroma-decoder/` or
`tools/library/` up to commit `f39e59e18` (the last commit before the `tools/`
directory was removed from ld-decode). Use `git shortlog -sne` to enumerate.

Particular contributors visible in that history (non-exhaustive):

- **Simon Inns** (simoninns) — original `ld-chroma-decoder` and TBC library
  author; ongoing maintainership of decode-orc / encode-orc / CVBS spec
- **Chad Page** — original ld-decode author, NTSC comb filter work
- **Adam Sampson** — Transform-PAL implementation, decoder pool, output writer
- **Phillip Blucas** — PALcolour and PAL-M support
- **Dani Funker** — chroma weight, NTSC 3D adaptive threshold options
- many more — see `git log src/legacy/`

## nnTransform3D models and original harnesses

- **asdfqazsnbb** — original author of the nnTransform3D models and both v1
  and v2 harnesses. The models were originally distributed through Discord
  chats before v2 was open-sourced on GitHub
  (see `~/Development/Analog Decoding Models/nnTransform3D/Repos/nnTransform3D`).
- **harrypm** — first to bring the nnTransform3D code into a public repo
  (the `tbc-tools` fork of `ld-decode-tools`), where it lived as patches to
  `src/ld-chroma-decoder/comb.{h,cpp}` plus `nnTransform3D_kernel.cu`.

## ldzeug2 models and harnesses

- **jsaowji** — author of the ldzeug2 project (Python/VapourSynth) and the
  underlying color_cnn / luma_sep / luma_sep_frame CNN models. See
  `~/Development/Repos/ldzeug2`.
- The integration in C++ at `~/Development/Repos/vapoursynth-analog/src/ldzeug_decoders.{h,cpp}`
  was contributed by the vapoursynth-analog maintainers building on jsaowji's
  reference implementation.

## CVBS file format specification

- **Simon Inns** and the CVBS spec contributors at
  `~/Development/Repos/cvbs-file-format-specification` — define the file
  format this library reads alongside the `.tbc` format.
