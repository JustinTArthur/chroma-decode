# chroma-decode (libchromadec)

A shared library for decoding 4×fsc-sampled composite analog video (CVBS / TBC)
into component Y'CbCr output. Bundles classical decoders (1D/2D/3D comb,
PALcolour, Transform-PAL 2D/3D) and neural-network decoders (nnTransform3D
v1+v2, ldzeug2 color_cnn + luma_sep) behind a stable C ABI.

## Status

**Pre-alpha (bootstrap).** The repo currently holds the skeleton and
preserved git history extracted from `ld-decode` (`tools/ld-chroma-decoder/`
and `tools/library/`, ~478 commits). The Qt-based legacy implementation lives
under `src/legacy/` and is in the process of being ported, Qt-free, into the
final layout.

The public C ABI under `include/chromadec/` is **stub-only** at this stage.
Every function returns `CHD_E_INTERNAL`. See
[docs/architecture.md](docs/architecture.md) for the plan.

## Building (once functional)

Needs Meson 1.8 or newer.

```
python -m venv .venv && source .venv/bin/activate
pip install meson ninja
meson setup build
meson compile -C build
meson test -C build
```

Use `meson configure build -Donnxruntime_root=/path/to/onnxruntime` if your
ONNX Runtime install isn't discoverable via pkg-config.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE). Inherited from `ld-decode`.

## Attribution

This library extracts work originally authored across several upstream
projects. See [docs/attribution.md](docs/attribution.md) for the full list of
contributors whose work is preserved here.
