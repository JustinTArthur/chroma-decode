# Rust bindings

libchromadec ships first-party Rust bindings as a two-crate workspace under
`rust/` in the source tree:

- **`chromadec-sys`** — raw, `unsafe` FFI declarations generated from the C
  headers with [bindgen](https://github.com/rust-lang/rust-bindgen). One-to-one
  with the C API, no policy of its own. Its build script locates the native
  library and emits the link directives.
- **`chromadec`** — the safe, idiomatic wrapper. Owning handle types with
  `Drop` impls, `Result`-based error handling, slices instead of
  pointer-plus-length pairs, and Rust enums mirroring the C enums.

Rust has no stable ABI, so these are consumed as **source crates** layered on
the same C ABI everything else links against — the native library artifact is
unchanged. Almost all consumers want the safe `chromadec` crate; reach for
`chromadec-sys` directly only when you need a C entry point the wrapper does not
expose yet.

## Adding the dependency

The crates are not published to crates.io yet; depend on them by path or git.

```toml
[dependencies]
chromadec = { git = "https://github.com/JustinTArthur/chroma-decode", branch = "main" }
```

`chromadec-sys` is pulled in transitively; you do not name it unless you call
raw FFI.

## Finding the native library

`chromadec-sys`'s build script locates libchromadec in this order:

1. **`CHROMADEC_LIB_DIR` + `CHROMADEC_INCLUDE_DIR`** — explicit override. Set
   both. `CHROMADEC_STATIC=1` links the static archive instead of the shared
   library.
2. **pkg-config** — probes for `chromadec`. Point `PKG_CONFIG_PATH` at an
   installed prefix's `lib/pkgconfig`, or at a Meson build tree's
   `meson-uninstalled` directory to build against an in-tree library without
   installing.

For a shared library that is not on the system's default search path at
runtime, set the loader path (`LD_LIBRARY_PATH` on Linux, `DYLD_LIBRARY_PATH`
on macOS, `PATH` on Windows) or link statically.

```sh
# Against an installed prefix
cargo build

# Against an in-tree Meson build, no install
PKG_CONFIG_PATH=/path/to/chroma-decode/build/meson-uninstalled \
  DYLD_LIBRARY_PATH=/path/to/chroma-decode/build/src \
  cargo run --example video_info -- capture.tbc
```

## A minimal decode

```rust
use chromadec::{Decoder, DecoderKind, Plane, Video};

fn main() -> chromadec::Result<()> {
    let mut video = Video::open_composite("capture.tbc", None, None)?;
    let mut decoder = Decoder::new(&mut video, DecoderKind::Ntsc3d)?;
    decoder.set_option_f64(chromadec::options::CHROMA_GAIN, 1.0)?;
    decoder.commit()?;

    let frame = decoder.decode_frame(0)?;
    let y = frame.plane_u16(Plane::Y)?;
    for row in y.rows() {
        // row: &[u16], `y.width()` samples wide
        let _ = row;
    }
    Ok(())
}
```

`chd_init` is called automatically the first time you open a source or load a
model, so there is no explicit init step.

## How the C API maps to Rust

| C API                  | Rust                                           |
|------------------------|------------------------------------------------|
| `chd_video_t *`        | `Video` (frees on drop)                        |
| `chd_decoder_t *`      | `Decoder<'v>` (borrows the `Video`)            |
| `chd_frame_t *`        | `Frame` (frees on drop)                        |
| `chd_nn_model_t *`     | `NnModel` (frees on drop)                      |
| `chd_cancel_t *`       | `Cancel` (frees on drop)                       |
| `chd_status_t` return  | `Result<T, Error>`                             |
| `chd_last_error()`     | captured into `Error::message`                 |
| `CHD_OPT_*` names      | `chromadec::options::*`                        |
| `chd_frame_get_plane*` | `Frame::plane_u16` / `plane_f32` → `PlaneView` |

A `Decoder` mutably borrows its `Video` for its whole lifetime, so the
borrow checker enforces the C contract that the video outlives the decoder.
`PlaneView` borrows its `Frame`, so plane data cannot outlive the frame that
owns it — the zero-copy borrow is sound without a copy.

### Errors

Every fallible call returns `chromadec::Result<T>`. An `Error` carries the
[`Status`] code and the thread-local detail string the library recorded
(`chd_last_error`), captured on the thread the failing call ran on. `Error`
implements `std::error::Error`, so it composes with `?` and error libraries.

### Decoding paths

| You want                                         | Use                                                            |
|--------------------------------------------------|----------------------------------------------------------------|
| Random access, or simple sequential decode       | `decode_frame(i)` — synchronous, no worker pool                |
| Max throughput over a batch, blocking the caller | `decode_frames` — parallel, completion-order callback          |
| Async / Tokio integration                        | `decode_frames_stream` — `FrameOrder::Completion` or `Indexed` |

### Parallel decode

`Decoder::decode_frames` wraps `chd_decode_frames_async`. It takes a closure
invoked from worker threads as each frame completes (in completion order, not
index order), and blocks until the batch finishes. The closure must be `Sync`;
a panic inside it is caught, the batch is unwound, and the panic is re-raised
on the calling thread once the C call returns — it never unwinds across the FFI
boundary. Pass a [`Cancel`] to make still-queued frames report
`Status::Cancelled`.

Need the results in index order? `decode_frames` only returns once the whole
batch is done, so no special call is required — index a `Vec<Option<Frame>>`
by position from the callback, or collect the items and sort by index. For
ordered *streaming* without holding the whole batch in memory, use the async
stream's `FrameOrder::Indexed` instead.

### Async stream

With the `tokio` feature, `chromadec::decode_frames_stream` runs that same
batch decode on a `spawn_blocking` worker and yields each frame through a
bounded channel, returning a `FrameStream` that implements
`futures_core::Stream` (and has an inherent `recv().await`). It is a runtime
consumer, not a provider: call it from within your own Tokio runtime (either
flavor).

Ownership of the `Video` moves into the worker, which builds, configures
(via the closure), commits, and runs the decoder there — so the non-`'static`
decoder borrow never has to cross threads. Behaviour is set by a
`StreamOptions`:

- `channel_depth` bounds how many decoded frames may sit buffered before the
  worker blocks, applying backpressure to a slow consumer; `None` uses
  `DEFAULT_CHANNEL_DEPTH` (8, which caps channel-held memory in the tens of MB
  for SD frames), `Some(0)` is rejected.
- `order` selects delivery: `FrameOrder::Completion` (default) yields each
  frame as soon as it finishes — maximum throughput, any order, the same
  contract as `decode_frames`. `FrameOrder::Indexed` yields them in
  requested-index order instead: a worker that finishes ahead of its turn
  parks until that frame is emitted, so at most the decoder's worker count of
  frames are held back — the worker count bounds the reorder window, no extra
  knob. The trade-off is head-of-line blocking: a slow early frame stalls
  delivery of later frames already decoded.

Cancel externally with `FrameStream::cancel` / `cancel_handle`; dropping the
stream requests cancel so an in-flight decode stops promptly. Call
`finish().await` afterwards for the overall decode status, including a setup
error that produced an empty stream.

## Neural decoders

`NnModel::load_from_file` / `load_from_memory` mirror the C loaders;
`SessionOpts` mirrors `chd_nn_session_opts_t` with `Default` matching
`chd_nn_session_opts_default()`. Bind a model with `Decoder::set_nn_model`
before `commit`. The decoder takes its own reference to the model, so the
`NnModel` handle may be dropped once bound. Backend availability in the current
build/host is queryable with `chromadec::backend_is_available`.

[`Status`]: https://docs.rs/chromadec/latest/chromadec/enum.Status.html
[`Cancel`]: https://docs.rs/chromadec/latest/chromadec/struct.Cancel.html
