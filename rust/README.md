# chromadec Rust bindings

First-party Rust bindings for [libchromadec](../README.md), the composite video
chroma decoding library. Two crates:

- **`chromadec-sys`** — raw `unsafe` FFI bindings generated from the C headers
  with bindgen.
- **`chromadec`** — safe, idiomatic wrapper (owning handles, `Result` errors,
  zero-copy plane views).

Most consumers want `chromadec`.

## Building

Requires Rust 1.85 or newer (the `chromadec` crate uses edition 2024).

`chromadec-sys`'s build script finds the native library via pkg-config, or via
`CHROMADEC_LIB_DIR` + `CHROMADEC_INCLUDE_DIR` (with optional `CHROMADEC_STATIC=1`).

```sh
# Against an in-tree Meson build, without installing:
PKG_CONFIG_PATH=../build/meson-uninstalled \
  DYLD_LIBRARY_PATH=../build/src \
  cargo test
```

On Linux use `LD_LIBRARY_PATH` instead of `DYLD_LIBRARY_PATH`.

See the [Rust bindings guide](../docs/rust-bindings.md) for the full API map and
examples.