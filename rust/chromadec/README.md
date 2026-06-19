# chromadec

Safe Rust bindings for **libchromadec**, the composite video chroma decoding library.

[![crates.io](https://img.shields.io/crates/v/chromadec.svg)](https://crates.io/crates/chromadec)
[![docs.rs](https://docs.rs/chromadec/badge.svg)](https://docs.rs/chromadec)
[![license](https://img.shields.io/badge/license-GPL--3.0--or--later-blue.svg)](#license)

`chromadec` wraps the libchromadec C API in owning Rust types with `Drop`-based
cleanup. It decodes composite (CVBS) and S-Video captures — ld-decode TBC files
and CVBS captures — into Y′CbCr or RGB frames, exposing the library's NTSC/PAL
2D and 3D decoders, the PAL Transform decoders, dropout correction, and the
optional neural decoders.

## Quick start

```rust,no_run
use chromadec::{Decoder, DecoderKind, Plane, Video};

fn main() -> chromadec::Result<()> {
    let mut video = Video::open_composite("capture.tbc", None, None)?;
    let mut decoder = Decoder::new(&mut video, DecoderKind::Ntsc3d)?;
    decoder.set_option_f64(chromadec::options::CHROMA_GAIN, 1.0)?;
    decoder.commit()?;

    let frame = decoder.decode_frame(0)?;
    let y = frame.plane_u16(Plane::Y)?;
    println!("{}x{}, first sample {}", y.width(), y.height(), y.row(0)[0]);
    Ok(())
}
```

The C API's handle types map to owning Rust types — `Video`, `Decoder`,
`Frame`, `NnModel`, `Cancel` — that free their C resources on drop, and errors
come back as `Result<T, chromadec::Error>`. Library initialisation happens
automatically on first use. The raw FFI bindings are re-exported as
`chromadec::sys`.

## Requirements

This crate links the native **libchromadec** library, which must be present at
build time. It is a binding, not a bundled build: the C library — and its own
dependencies, such as SQLite (and ONNX Runtime when neural decoding is enabled)
— must be available. `chromadec-sys`'s build script locates libchromadec, in
order:

1. via `pkg-config` (a system-installed `libchromadec`);
2. from a meson build tree — point `PKG_CONFIG_PATH` at its `meson-uninstalled`
   (and `meson-private`) directory;
3. explicitly, via the `CHROMADEC_LIB_DIR` and `CHROMADEC_INCLUDE_DIR`
   environment variables.

## Features

- `tokio` — an async `FrameStream` adapter (`decode_frames_stream`) that runs
  the batch decoder on a `spawn_blocking` worker and yields frames as a
  `futures_core::Stream`, in completion order or requested-index order. Off by
  default.

## Documentation

- API reference: <https://docs.rs/chromadec>
- Integration guide (with an ld-chroma-decoder migration appendix):
  `docs/rust-bindings.md` in the repository.

## Minimum supported Rust version

Rust 1.85 (edition 2024).

## License

GPL-3.0-or-later.