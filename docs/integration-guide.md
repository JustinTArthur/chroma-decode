# Integration guide

How to consume libchromadec from your application: opening a source, decoding
frames, and handling the output. This guide is for **any** consumer, whether
you are starting from scratch or replacing an existing `ld-chroma-decoder`
integration. If you are doing the latter, jump to
[Migrating from ld-chroma-decoder](#migrating-from-ld-chroma-decoder) once
you have skimmed the basics.

For the exhaustive per-function contract, see the
[C API reference](api-reference.md).

## Linking the library

libchromadec installs both a CMake package config and a pkg-config file, so a
downstream project links it idiomatically from either build system.

=== "CMake"

    ```cmake
    find_package(chromadec CONFIG REQUIRED)
    target_link_libraries(my_consumer PRIVATE chromadec::chromadec)
    ```

=== "Meson"

    ```meson
    chromadec_dep = dependency('chromadec', version: '>= 0.1')
    executable('my_consumer', 'main.c', dependencies: chromadec_dep)
    ```

=== "pkg-config"

    ```sh
    cc main.c $(pkg-config --cflags --libs chromadec)
    ```

Then include the umbrella header (or the individual headers you need):

```c
#include <chromadec/chromadec.h>
```

The public surface is pure C, so the headers are consumable from both C and
C++ without a C++ runtime requirement at the boundary.

## A minimal decode

The shape of every integration is the same: initialise, open a source, create
and configure a decoder, commit, decode frames, and free everything in reverse
order of creation.

```c
#include <chromadec/chromadec.h>
#include <stdio.h>

int main(void) {
    if (chd_init() != CHD_OK)
        return 1;

    /* 1. Open a source. NULL sidecar → auto-locate <path>.db or <path>.json. */
    chd_video_t *video = NULL;
    if (chd_video_open_composite("capture.tbc", NULL, &video) != CHD_OK) {
        fprintf(stderr, "open: %s\n", chd_last_error());
        return 1;
    }

    chd_video_info_t info;
    chd_video_get_info(video, &info);   /* info.num_frames, standard, etc. */

    /* 2. Create a decoder bound to the source. */
    chd_decoder_t *dec = NULL;
    if (chd_decoder_create(video, CHD_DEC_AUTO, &dec) != CHD_OK) {
        fprintf(stderr, "decoder: %s\n", chd_last_error());
        chd_video_free(video);
        return 1;
    }

    /* 3. Configure options, then commit before the first decode. */
    chd_decoder_set_option_str(dec, CHD_OPT_OUTPUT_FORMAT, "yuv444p16");
    chd_decoder_set_option_f64(dec, CHD_OPT_CHROMA_GAIN, 1.0);
    if (chd_decoder_commit(dec) != CHD_OK) {
        fprintf(stderr, "commit: %s\n", chd_last_error());
        chd_decoder_free(dec);
        chd_video_free(video);
        return 1;
    }

    /* 4. Decode frames by index (random access). */
    for (int64_t i = 0; i < info.num_frames; i++) {
        chd_frame_t *frame = NULL;
        chd_status_t rc = chd_decode_frame(dec, i, &frame);
        if (rc != CHD_OK) {
            fprintf(stderr, "frame %lld: %s\n", (long long)i, chd_last_error());
            break;
        }

        chd_frame_info_t fi;
        chd_frame_get_info(frame, &fi);

        const void *y = NULL;
        ptrdiff_t stride = 0;
        chd_frame_get_plane(frame, CHD_PLANE_Y, &y, &stride);
        /* ... consume the borrowed plane pointer here ... */

        chd_frame_free(frame);   /* you own each decoded frame */
    }

    /* 5. Free in reverse order: decoder borrows the video, so it goes first. */
    chd_decoder_free(dec);
    chd_video_free(video);
    chd_shutdown();              /* required only if an NN model was loaded */
    return 0;
}
```

!!! warning "Free order matters"
    A decoder holds a borrowed pointer to its video source. Freeing the video
    while a decoder still references it is a use-after-free. Always free the
    decoder (and any decoded frames) **before** the video.

## Opening other source types

[`chd_video_open_composite`](api-reference.md#chd_video_open_composite) handles
ld-decode `.tbc` files. For CVBS captures use
[`chd_video_open_composite`](api-reference.md#chd_video_open_composite)
(single `.composite` file) or
[`chd_video_open_yc`](api-reference.md#chd_video_open_yc) (separate
Y/C files). Both accept an optional
[`chd_video_params_t`](api-reference.md#chd_video_params_t) override for when
metadata is absent or you need to force parameters.

## Configuring the decode

Options are set by name through the typed setters and take effect at the next
[`chd_decoder_commit`](api-reference.md#chd_decoder_commit). The full set lives
in the [option registry](api-reference.md#option-registry); a few common ones:

```c
chd_decoder_set_option_str (dec, CHD_OPT_OUTPUT_FORMAT, "yuv444p16");
chd_decoder_set_option_f64 (dec, CHD_OPT_CHROMA_GAIN, 1.2);
chd_decoder_set_option_bool(dec, CHD_OPT_REVERSE_FIELD_ORDER, 1);
chd_decoder_set_option_i32 (dec, CHD_OPT_THREAD_COUNT, 0);   /* 0 = auto */
chd_decoder_commit(dec);
```

Setting an option that does not apply to the decoder kind returns
`CHD_E_INVALID_ARG`; use
[`chd_decoder_has_option`](api-reference.md#setting-options) to probe.

## Pixel formats and plane access

The output format is chosen with `CHD_OPT_OUTPUT_FORMAT`
(`"yuv444p16"`, `"yuv444ps"`, `"rgb48"`, `"rgbs"`, `"gray16"`, `"grays"`). For
each decoded frame:

- [`chd_frame_get_plane`](api-reference.md#chd_frame_get_plane) borrows a
  read-only pointer plus a **byte** stride into 16-bit plane data for the
  integer formats. The pointer belongs to the frame. Never free it, and never
  use it after the frame is freed.
- [`chd_frame_get_plane_float`](api-reference.md#chd_frame_get_plane_float)
  is the equivalent zero-copy borrow for the float formats. `"yuv444ps"` and
  `"grays"` expose the normalized `E′Y E′Cb E′Cr` signals of ITU-R
  BT.601 / ITU-T H.273 that the integer formats quantize. `"rgbs"` exposes
  normalized `E′R E′G E′B` planes computed direct from the decoder's
  component signals via the BT.601/H.273 Y′CbCr → R′G′B′ matrix, with no
  intermediate integer quantization.

Which planes are valid depends on the format: Y/Cb/Cr, R/G/B, or a single Y
plane for `"gray16"`.

## Neural decoders

The neural decoder kinds (`CHD_DEC_NN_TRANSFORM3D`, `CHD_DEC_LDZEUG_*`) need a
model attached before commit:

```c
chd_nn_session_opts_t opts;
chd_nn_session_opts_default(&opts);     /* AUTO provider, sane defaults */
opts.provider = CHD_NN_EP_AUTO;

chd_nn_model_t *model = NULL;
if (chd_nn_model_load_from_file("chroma_net_v2.onnx", &opts, &model) != CHD_OK) {
    fprintf(stderr, "model: %s\n", chd_last_error());
    /* ... */
}
/* Or, to load a model embedded in the binary (no file needed):
 *   chd_nn_model_load_from_memory(model_bytes, model_len, &opts, &model); */

chd_decoder_t *dec = NULL;
chd_decoder_create(video, CHD_DEC_NN_TRANSFORM3D, &dec);
chd_decoder_set_nn_model(dec, model);   /* model must outlive the decoder */
chd_decoder_commit(dec);
```

The model is **borrowed**, not owned. Keep it alive for the decoder's
lifetime, free the decoder first, then `chd_nn_model_free(model)`.

!!! warning "Shutdown after NN use"
    Once any model has been loaded, call
    [`chd_shutdown`](api-reference.md#chd_shutdown) exactly once before process
    exit. It tears down the ONNX Runtime environment and is intentionally never
    automatic. Skip it and you risk a static-destruction crash on exit.

Provider availability can be probed up front with
[`chd_nn_provider_is_available`](api-reference.md#chd_nn_provider_is_available),
and the provider actually chosen by `CHD_NN_EP_AUTO` read back with
[`chd_nn_model_get_active_provider`](api-reference.md#chd_nn_model_get_active_provider).
See [NN model conventions](nn-models.md) for model paths, magnitude scales, and
per-platform provider notes.

## Dropout correction

Configure correction on the decoder, optionally registering extra captures of
the same content as replacement sources **before** creating the decoder:

```c
chd_video_add_extra_source_composite(video, "second-pass.tbc");

chd_dropout_opts_t dz = { .enabled = 1, .overcorrect = 0, .intra_field_only = 0 };
chd_decoder_set_dropout(dec, &dz);
chd_decoder_commit(dec);
/* after each decode: */
chd_dropout_stats_t stats;
chd_decoder_get_last_dropout_stats(dec, &stats);
```

## Dropout detection

Concealment *hides* dropouts; sometimes you want to *see* where they are — for
example to produce a mask clip highlighting damaged regions alongside the
decoded video. The detected regions are source metadata, available without
running the chroma decoder, so extract them cheaply with a `CHD_DEC_NONE`
decoder. Configure it with the same geometry options (padding, output format,
line overrides, field order) as your video decoder so the two line up
pixel-for-pixel:

```c
chd_decoder_t *mask_dec = NULL;
chd_decoder_create(video, CHD_DEC_NONE, &mask_dec);
chd_decoder_set_option_i32(mask_dec, CHD_OPT_PADDING_MULTIPLE, 1);
chd_decoder_commit(mask_dec);

chd_output_info_t oi;
chd_decoder_get_output_info(mask_dec, &oi);   /* canvas size, no decode needed */
```

The simplest path is `chd_decode_dropout_mask`, which returns a single-plane
frame (`0` clean, set when dropped) sized to the output framing — ready to use as
a mask plane. Its format follows the committed output format's precision domain,
so the mask matches the sample type of the decode clip it accompanies: an integer
output format gives a `GRAY16` mask (`0xFFFF` dropped, read with
`chd_frame_get_plane`); a float output format (`grays`, `yuv444ps`, `rgbs`) gives
a `GRAYS` mask (`1.0` dropped, read with `chd_frame_get_plane_float`). The
example above commits the default `yuv444p16`, so the mask is `GRAY16`:

```c
chd_frame_t *mask = NULL;
if (chd_decode_dropout_mask(mask_dec, frame_index, &mask) == CHD_OK) {
    const void *data; ptrdiff_t stride;
    chd_frame_get_plane(mask, CHD_PLANE_Y, &data, &stride);
    /* ... composite the mask against the decoded luma plane ... */
    chd_frame_free(mask);
}
```

For finer control — feathered or coloured overlays, say — ask for the raw spans
and rasterise them yourself:

```c
chd_dropout_span_t *spans = NULL;
size_t count = 0;
chd_decoder_get_dropout_spans(mask_dec, frame_index, &spans, &count);
for (size_t i = 0; i < count; i++) {
    /* mark columns [spans[i].x_start, spans[i].x_end) on row spans[i].y */
}
chd_dropout_spans_free(spans);
```

Both report the raw detected dropouts in the active-output coordinate space, so
they overlay frames from a video decoder committed with the same geometry. A
`CHD_DEC_NONE` decoder rejects [`chd_decode_frame`](api-reference.md#chd_decode_frame)
with `CHD_E_DECODER_INCOMPATIBLE` — it exists only to serve geometry and dropout
queries.

## Decoding many frames in parallel

[`chd_decode_frames_async`](api-reference.md#chd_decode_frames_async) fans a
list of indices across the decoder's worker pool and delivers each result to a
callback.

```c
static void on_frame(void *user, chd_status_t s, int64_t idx, chd_frame_t *f) {
    if (s == CHD_OK) {
        /* consume f from a worker thread; must be thread-safe */
        chd_frame_free(f);          /* the callback owns the frame */
    }
}

int64_t indices[] = {0, 1, 2, 3};
chd_decode_frames_async(dec, indices, 4, on_frame, NULL, NULL);
```

!!! warning "It blocks, runs on worker threads, and hands you ownership"
    Despite the name the call **blocks until every frame is delivered**. The
    callback runs on worker threads, possibly concurrently, so it must be
    thread-safe, and each delivered frame is yours to
    [`chd_frame_free`](api-reference.md#chd_frame_free). Pass a
    [`chd_cancel_t`](api-reference.md#cancellation) to stop early; cancelled
    indices arrive as `CHD_E_CANCELLED` with a `NULL` frame.

## Error handling pattern

Every fallible call returns [`chd_status_t`](api-reference.md#status-codes).
On failure, [`chd_last_error()`](api-reference.md#chd_last_error) gives a
thread-local detail string for that thread's most recent failing call:

```c
chd_status_t rc = chd_video_open_composite(path, NULL, &video);
if (rc != CHD_OK) {
    fprintf(stderr, "%s: %s\n", chd_status_str(rc), chd_last_error());
}
```

`chd_status_str` is a stable static code name; `chd_last_error` is the
context-rich, per-thread message.

---

## Migrating from ld-chroma-decoder

If your project currently builds against `ld-chroma-decoder` (as a git
submodule, a vendored copy, or in-tree decoder classes), this section maps the
old surface onto libchromadec. The structural goal is to **drop the vendored
code and dynamically link `libchromadec` instead**.

### Structural changes

1. Remove the submodule / vendored `ld-chroma-decoder` (and any local patches).
2. Add `find_package(chromadec CONFIG REQUIRED)` and link
   `chromadec::chromadec`.
3. Replace direct use of the C++ decoder classes and the CLI-driven process
   model with the C ABI flow shown above: open → create → set options →
   commit → decode → free.

### CLI flags → ABI

The old command-line flags map onto the ABI in four ways: as the **decoder
kind** at create time, as **option-registry** entries, as **open arguments**,
or as part of **your decode loop**.

| `ld-chroma-decoder` flag | libchromadec equivalent |
|---|---|
| `-f, --decoder <name>` | `chd_decoder_kind_t` passed to [`chd_decoder_create`](api-reference.md#chd_decoder_create) (`ntsc2d` → `CHD_DEC_NTSC_2D`, `transform3d` → `CHD_DEC_TRANSFORM_3D`, …) |
| `-b, --blackandwhite` | decoder kind `CHD_DEC_MONO` |
| `--simple-pal` | decoder kind `CHD_DEC_PAL_2D` (vs the transform PAL kinds) |
| `-r, --reverse` | `CHD_OPT_REVERSE_FIELD_ORDER` (bool) |
| `--chroma-gain` | `CHD_OPT_CHROMA_GAIN` (f64) |
| `--chroma-phase` | `CHD_OPT_CHROMA_PHASE_DEG` (f64) |
| `--chroma-nr` | `CHD_OPT_CHROMA_NR_LEVEL` (f64) |
| `--luma-nr` | `CHD_OPT_LUMA_NR_LEVEL` (f64) |
| `--ntsc-phase-comp` | `CHD_OPT_PHASE_COMPENSATION` (bool) |
| `--adapt-threshold` | `CHD_OPT_COMB_ADAPT_THRESHOLD` (f64) |
| `--chroma-weight` | `CHD_OPT_COMB_CHROMA_WEIGHT` (f64) |
| `--transform-threshold` | `CHD_OPT_TRANSFORM_THRESHOLD` (f64) |
| `--transform-thresholds <file>` | `CHD_OPT_TRANSFORM_THRESHOLDS_FILE` (str) |
| `-o, --oftest` | `CHD_OPT_COMB_SHOW_MAP` (bool) |
| `--pad, --output-padding` | `CHD_OPT_PADDING_MULTIPLE` (i32; **defaults to `1` / no padding here**) |
| `-p, --output-format` | `CHD_OPT_OUTPUT_FORMAT` (str) + `CHD_OPT_OUTPUT_Y4M_HEADERS` (bool) |
| `-t, --threads` | `CHD_OPT_THREAD_COUNT` (i32, `0` = auto) |
| `--ffll / --lfll` (field lines) | `CHD_OPT_FIRST_ACTIVE_FIELD_LINE` / `CHD_OPT_LAST_ACTIVE_FIELD_LINE` (i32; **inclusive** — last line is included) |
| `--ffrl / --lfrl` (frame lines) | `CHD_OPT_FIRST_ACTIVE_FRAME_LINE` / `CHD_OPT_LAST_ACTIVE_FRAME_LINE` (i32; **inclusive** — last line is included) |
| `--input-metadata <file>` | the `sidecar_path` argument to [`chd_video_open_composite`](api-reference.md#chd_video_open_composite) |
| `-s, --start` / `-l, --length` | the frame **indices** you pass to [`chd_decode_frame`](api-reference.md#chd_decode_frame). There is no global start/length; you drive the range |
| `--show-ffts` and other debug flags | not part of the stable ABI |

### Things that change in spirit, not just syntax

- **No process-per-decode.** Where you previously shelled out to the
  `ld-chroma-decoder` binary and piped its output, you now hold a decoder
  handle and pull frames directly: no subprocess, no stdout parsing.
- **Random access instead of a stream.** `chd_decode_frame` takes an index, so
  seeking is free; the old `--start`/`--length` windowing becomes a loop bound.
- **Output is in-memory planes**, not a Y4M byte stream; request Y4M framing
  with `CHD_OPT_OUTPUT_Y4M_HEADERS` only if you specifically need it.
- **Neural decoders are first-class** here (`CHD_DEC_NN_TRANSFORM3D`,
  `CHD_DEC_LDZEUG_*`) rather than out-of-tree patches.
- **Active frame and field lines are inclusive.**
  `CHD_OPT_FIRST_ACTIVE_FRAME_LINE` / `CHD_OPT_LAST_ACTIVE_FRAME_LINE` and
  `CHD_OPT_FIRST_ACTIVE_FIELD_LINE` / `CHD_OPT_LAST_ACTIVE_FIELD_LINE` name the
  first and last lines that are *part of* the active region; the last line is
  included, not one-past-the-end. If you previously passed ld-chroma-decoder's
  exclusive `--lfrl` / `--lfll` values through, subtract one. **TBC sidecar
  metadata is handled for you:** tbc-tools writes `last_active_frame_line` /
  `last_active_field_line` as exclusive bounds, and the SQLite reader translates
  them to the inclusive scheme on ingest, so a v4 sidecar decodes to exactly the
  picture its author intended. With the default active region you get a full
  **486-line picture for 525-line (NTSC)** systems and **576 lines for 625-line
  (PAL)** systems.
- **No output padding by default.** `CHD_OPT_PADDING_MULTIPLE` defaults to `1`
  (emit the active region as-is). decode-orc / tbc-tools pad the frame to a
  multiple of 8 unless told otherwise — set `CHD_OPT_PADDING_MULTIPLE`
  explicitly if you need codec-friendly dimensions. Padding now applies
  uniformly to every output format, including `yuv444ps` and `rgbs`.