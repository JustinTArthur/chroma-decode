# C API reference

The complete public C ABI of libchromadec. Every exported symbol is prefixed
`chd_` and declared in a header under `<chromadec/>`. Include the umbrella
header to pull in the whole surface:

```c
#include <chromadec/chromadec.h>
```

or include only the headers you need (`<chromadec/video.h>`,
`<chromadec/decoder.h>`, …). The library is pure C at the boundary and C++17
internally; nothing in these headers requires a C++ compiler to consume.

## Conventions

These rules hold across the whole ABI. They are stated once here rather than
repeated on every function.

### Return values and error detail

- A function that can fail returns [`chd_status_t`](#status-codes); `CHD_OK`
  (`0`) is success, any other value is failure.
- Accessors that cannot meaningfully fail return their value directly
  (`chd_version`, `chd_cancel_is_requested`, `chd_nn_provider_is_available`,
  `chd_has_feature`).
- **Output parameters are written only on `CHD_OK`.** On failure, a `**out`
  handle is left untouched. Initialise your pointer to `NULL` and check the
  status before using it.
- After a failure, [`chd_last_error()`](#chd_last_error) returns a
  human-readable, thread-local detail string for the *most recent* failing
  call on the calling thread. [`chd_status_str()`](#chd_status_str) maps a
  status code to a stable static string.

### Ownership and lifetime

- Every `chd_*_create` / `chd_*_open_*` / `chd_*_load` that yields a handle
  transfers ownership to the caller. Release it with the matching
  `chd_*_free`. Freeing `NULL` is always safe.
- Handles are **opaque**. You only ever hold a `chd_video_t *`,
  `chd_decoder_t *`, etc. Their layout is private and may change between
  minor versions without breaking your binary.
- Decoded frames ([`chd_frame_t`](#frames)) are owned by the caller and freed
  with [`chd_frame_free`](#chd_frame_free). This includes frames delivered to an
  async callback.
- Plane pointers from [`chd_frame_get_plane`](#chd_frame_get_plane) are
  **borrowed**: do not free them; they remain valid only until the owning
  frame is freed.

### Threading

- The error-detail string is **thread-local**. Each thread sees only the
  result of its own calls.
- A single `chd_decoder_t` is **not** safe for concurrent option mutation and
  decoding. Setting options / committing must not overlap a `chd_decode_frame`
  call on the same decoder.
- Distinct handles are independent and may be used concurrently from different
  threads.
- [`chd_decode_frames_async`](#chd_decode_frames_async) invokes your callback
  from internal worker threads; the callback must be thread-safe.

### Lifecycle

Call [`chd_init`](#chd_init) once before any other call, and
[`chd_shutdown`](#chd_shutdown) once before process exit **if any NN model was
loaded**. See [Library lifecycle](#library-lifecycle) for why shutdown is not
automatic.

---

## Library lifecycle

Declared in `<chromadec/video.h>` (lifecycle) and `<chromadec/version.h>`
(version / feature probes).

### chd_init

```c
chd_status_t chd_init(void);
```

Initialise the library. Call once at startup before any other `chd_*` call.
Idempotent and currently cheap, but always pair it with a successful return
check; future versions may perform real one-time setup here.

### chd_shutdown

```c
void chd_shutdown(void);
```

Tear down process-wide library state: currently the ONNX Runtime environment
singleton created the first time an NN model is loaded.

!!! warning "Required after NN use, and never automatic"
    If your process loaded **any** NN model ([`chd_nn_model_load_from_file`](#chd_nn_model_load_from_file)),
    call `chd_shutdown()` exactly once before exit. It is deliberately *not*
    registered with `atexit`: ORT execution-provider libraries run their own
    static destructors on unload, and the ordering against an `atexit` teardown
    is fragile (especially on Windows). If no NN model was ever loaded,
    `chd_shutdown()` is a harmless no-op.

### chd_version / chd_version_string

```c
void        chd_version(int *major, int *minor, int *patch);
const char *chd_version_string(void);   /* e.g. "0.1.0" */
```

Runtime library version. Any of the three out-pointers to `chd_version` may be
`NULL`. The string returned by `chd_version_string` is static and must not be
freed. Compile-time equivalents are the `CHROMADEC_VERSION_*` macros in
`<chromadec/version.h>`.

### chd_has_feature

```c
int chd_has_feature(const char *feature);   /* 1 = compiled in, 0 = not */
```

Query optional build features. Recognised names: `"nn"`, `"cuda"`, `"fftw"`,
`"sqlite"`. Returns `0` for `NULL` or any unknown name.

---

## Error handling

Declared in `<chromadec/errors.h>`.

### Status codes

`chd_status_t` is the return type of every fallible call.

| Code                            | Meaning                                                                                   |
|---------------------------------|-------------------------------------------------------------------------------------------|
| `CHD_OK`                        | Success (value `0`).                                                                      |
| `CHD_E_INVALID_ARG`             | A null/out-of-domain argument, or a call made out of order (e.g. decoding before commit). |
| `CHD_E_FILE_NOT_FOUND`          | An input or sidecar path does not exist.                                                  |
| `CHD_E_IO`                      | Read/write failure on an otherwise-present file.                                          |
| `CHD_E_FORMAT_UNSUPPORTED`      | The container/sample encoding is not handled.                                             |
| `CHD_E_METADATA_MISSING`        | Required metadata (sidecar) could not be located.                                         |
| `CHD_E_METADATA_CORRUPT`        | Metadata was found but failed to parse.                                                   |
| `CHD_E_PRESET_UNKNOWN`          | An unknown preset name was requested.                                                     |
| `CHD_E_DECODER_UNKNOWN`         | Unknown decoder kind.                                                                     |
| `CHD_E_DECODER_INCOMPATIBLE`    | Decoder kind is invalid for this video standard/encoding.                                 |
| `CHD_E_NN_MODEL_LOAD`           | The NN model file failed to load.                                                         |
| `CHD_E_NN_PROVIDER_UNAVAILABLE` | The requested execution provider is not available in this build/host.                     |
| `CHD_E_NN_INFERENCE`            | Inference failed at runtime.                                                              |
| `CHD_E_OUT_OF_RANGE`            | A frame index (or similar) is outside the valid range.                                    |
| `CHD_E_CANCELLED`               | The operation was cancelled via a [`chd_cancel_t`](#cancellation).                        |
| `CHD_E_INTERNAL`                | An unexpected internal error.                                                             |
| `CHD_E_OOM`                     | Allocation failure.                                                                       |

### chd_status_str

```c
const char *chd_status_str(chd_status_t s);
```

Map a status code to a stable, static, English string (e.g.
`"CHD_E_FILE_NOT_FOUND"`-style). Never `NULL`; never freed. Suitable for logs
and assertions; it does not vary by call site.

### chd_last_error

```c
const char *chd_last_error(void);
```

Return a thread-local, human-readable description of the **most recent failing
call on the calling thread**, including call-site context the status code
alone cannot convey (which path, which option, etc.). The pointer is valid
until the next `chd_*` call on the same thread. Never freed.

### chd_clear_last_error

```c
void chd_clear_last_error(void);
```

Reset the calling thread's error-detail string to empty.

---

## Core types

Declared in `<chromadec/types.h>`. Opaque handle types:

```c
typedef struct chd_video    chd_video_t;
typedef struct chd_decoder  chd_decoder_t;
typedef struct chd_frame    chd_frame_t;
typedef struct chd_nn_model chd_nn_model_t;
typedef struct chd_cancel   chd_cancel_t;
```

### Enumerations

| Enum | Values |
|---|---|
| `chd_video_standard_t` | `CHD_STD_UNKNOWN`, `CHD_STD_NTSC`, `CHD_STD_PAL`, `CHD_STD_PAL_M` |
| `chd_sample_encoding_t` | `CHD_ENC_UNKNOWN`, `CHD_ENC_CVBS_U10_4FSC`, `CHD_ENC_CVBS_U16_4FSC`, `CHD_ENC_RAW_S16_28M`, `CHD_ENC_RAW_S16_40M`, `CHD_ENC_CVBS_TPG21_4FSC` |
| `chd_signal_state_t` | `CHD_SIG_UNKNOWN`, `CHD_SIG_STANDARD_TBC_LOCKED`, `CHD_SIG_STANDARD_TBC_UNLOCKED`, `CHD_SIG_STANDARD_RAW`, `CHD_SIG_NONSTANDARD_TBC_LOCKED`, `CHD_SIG_NONSTANDARD_TBC_UNLOCKED`, `CHD_SIG_NONSTANDARD_RAW` |
| `chd_plane_t` | `CHD_PLANE_Y`, `CHD_PLANE_CB`, `CHD_PLANE_CR`, `CHD_PLANE_R`, `CHD_PLANE_G`, `CHD_PLANE_B` |
| `chd_pixel_format_t` | `CHD_PIXEL_YUV444P16`, `CHD_PIXEL_YUV444PS`, `CHD_PIXEL_RGB48`, `CHD_PIXEL_RGBS`, `CHD_PIXEL_GRAY16`, `CHD_PIXEL_GRAYS` |
| `chd_clamp_t` | `CHD_CLAMP_NONE`, `CHD_CLAMP_LEGAL_RGB_SDR`, `CHD_CLAMP_LEGAL_RGB_HDR`, `CHD_CLAMP_LEGAL_YCBCR_BT601` |

### chd_video_params_t

Caller-supplied parameters used to override or supply metadata when opening a
CVBS source (see [`chd_video_open_composite`](#chd_video_open_composite)).

```c
typedef struct chd_video_params {
    chd_video_standard_t  standard;
    chd_sample_encoding_t encoding;
    chd_signal_state_t    signal_state;
    int32_t  field_width;
    int32_t  field_height;
    double   sample_rate_hz;
    int32_t  active_video_start;
    int32_t  active_video_end;
    int32_t  first_active_frame_line;
    int32_t  last_active_frame_line;
    int32_t  black_16b_ire;
    int32_t  white_16b_ire;
    int32_t  blanking_16b_ire;
    int      is_widescreen;
    int      is_subcarrier_locked;
    int      is_first_field_first;
} chd_video_params_t;
```

### chd_video_info_t

Read-back description of an opened video, filled by
[`chd_video_get_info`](#chd_video_get_info). Superset of the params struct:
adds the derived `fsc_hz` subcarrier frequency and the decodable `num_frames`
count.

```c
typedef struct chd_video_info {
    chd_video_standard_t  standard;
    chd_sample_encoding_t encoding;
    chd_signal_state_t    signal_state;
    int32_t  field_width;
    int32_t  field_height;
    double   sample_rate_hz;
    double   fsc_hz;
    int32_t  active_video_start;
    int32_t  active_video_end;
    int32_t  first_active_frame_line;
    int32_t  last_active_frame_line;
    int32_t  black_16b_ire;
    int32_t  white_16b_ire;
    int32_t  blanking_16b_ire;
    int64_t  num_frames;
    int      is_widescreen;
    int      is_subcarrier_locked;
    int      is_first_field_first;
} chd_video_info_t;
```

### chd_frame_info_t

```c
typedef struct chd_frame_info {
    chd_pixel_format_t format;
    int32_t  width;
    int32_t  height;
    int32_t  num_planes;
    int64_t  frame_index;
} chd_frame_info_t;
```

### chd_output_info_t

```c
typedef struct chd_output_info {
    chd_pixel_format_t format;
    int32_t  width;
    int32_t  height;
    int32_t  num_planes;
    int64_t  num_frames;
} chd_output_info_t;
```

The committed output framing, filled by
[`chd_decoder_get_output_info`](#chd_decoder_get_output_info): the active-picture
`width` and `height` after crop and padding (the dimensions a decoded frame or a
dropout mask fills), the pixel `format` and its `num_planes`, and the source
`num_frames`.

---

## Video sources

Declared in `<chromadec/video.h>`. A `chd_video_t` is an opened input plus its
metadata; it is the argument you pass to [`chd_decoder_create`](#chd_decoder_create).

### chd_video_open_composite

```c
chd_status_t chd_video_open_composite(const char *tbc_path,
                                const char *sidecar_path_or_null,
                                chd_video_t **out);
```

Open an ld-decode `.tbc` file. `sidecar_path_or_null`:

- `NULL`: auto-locate `<tbc_path>.db` (SQLite) or `<tbc_path>.json` next to
  the data file.
- `const char *`: an explicit path to either a `.db` or `.json` sidecar.

### chd_video_open_composite

```c
chd_status_t chd_video_open_composite(const char *composite_path,
                                           const char *meta_path_or_null,
                                           const chd_video_params_t *override_or_null,
                                           chd_video_t **out);
```

Open a single-file CVBS composite capture (`<basename>.composite`).

- `meta_path_or_null`: `NULL` auto-locates `<basename>.meta`; otherwise the
  explicit path.
- `override_or_null`: `NULL` means all parameters come from metadata; a
  non-null struct supplies values when metadata is absent, or overrides them.

### chd_video_open_yc

```c
chd_status_t chd_video_open_yc(const char *y_path,
                                    const char *c_path,
                                    const char *meta_path_or_null,
                                    const chd_video_params_t *override_or_null,
                                    chd_video_t **out);
```

Open a dual-file CVBS Y/C pair. Metadata and override semantics match
[`chd_video_open_composite`](#chd_video_open_composite).

### chd_video_get_info

```c
chd_status_t chd_video_get_info(const chd_video_t *v, chd_video_info_t *out);
```

Fill `*out` with the opened video's [info](#chd_video_info_t). Note that
`num_frames` and `is_first_field_first` reflect the field order in effect; a
decoder's `reverse_field_order` option can change the decodable frame count
(see the [option registry](#option-registry)).

### Extra sources for multi-source dropout

```c
chd_status_t chd_video_add_extra_source_composite(chd_video_t *v, const char *tbc_path);
chd_status_t chd_video_add_extra_source_composite(chd_video_t *v,
                                                       const char *path,
                                                       const char *meta_path_or_null);
chd_status_t chd_video_add_extra_source_yc(chd_video_t *v,
                                                const char *y_path,
                                                const char *c_path,
                                                const char *meta_path_or_null);
```

Register additional captures of the same content as replacement candidates for
[dropout correction](#dropout-correction). Add them to the primary
`chd_video_t` before creating a decoder.

### chd_video_free

```c
void chd_video_free(chd_video_t *v);
```

Release a video handle. Decoders created from it must be freed first.

---

## Decoder

Declared in `<chromadec/decoder.h>`.

### chd_decoder_create / chd_decoder_free { #chd_decoder_create }

```c
chd_status_t chd_decoder_create(chd_video_t *v, chd_decoder_kind_t kind, chd_decoder_t **out);
void         chd_decoder_free(chd_decoder_t *d);
```

Create a decoder of `kind` bound to an opened video. `CHD_DEC_AUTO` selects a
default appropriate to the video standard.

| `chd_decoder_kind_t`                               |                                             |
|----------------------------------------------------|---------------------------------------------|
| `CHD_DEC_AUTO`                                     | Pick a sensible default for the standard.   |
| `CHD_DEC_MONO`                                     | Luma only.                                  |
| `CHD_DEC_NTSC_1D` / `_2D` / `_3D` / `_3D_NO_ADAPT` | NTSC comb decoders.                         |
| `CHD_DEC_PAL_2D`                                   | PAL 2D comb.                                |
| `CHD_DEC_TRANSFORM_2D` / `_3D`                     | Transform-domain decoders.                  |
| `CHD_DEC_NN_TRANSFORM3D`                           | Neural 3D transform (requires an NN model). |
| `CHD_DEC_LDZEUG_COLOR_CNN`                         | Neural colour CNN.                          |
| `CHD_DEC_LDZEUG_LUMA_SEP` / `_FRAME`               | Neural luma separation (field / frame).     |
| `CHD_DEC_NONE`                                     | Geometry/metadata only — no chroma decode.  |

`CHD_DEC_NONE` builds no chroma-decoding engine. Commit still resolves the
output framing, so [`chd_decoder_get_output_info`](#chd_decoder_get_output_info)
and the decode-free dropout queries
([`chd_decoder_get_dropout_spans`](#chd_decoder_get_dropout_spans),
[`chd_decode_dropout_mask`](#chd_decode_dropout_mask)) work — but
[`chd_decode_frame`](#chd_decode_frame) and
[`chd_decode_frames_async`](#chd_decode_frames_async) return
`CHD_E_DECODER_INCOMPATIBLE`. Use it to read dropout regions or output geometry
without paying for chroma decoding.

### Setting options

```c
chd_status_t chd_decoder_set_option_f64(chd_decoder_t *d, const char *name, double v);
chd_status_t chd_decoder_set_option_i32(chd_decoder_t *d, const char *name, int32_t v);
chd_status_t chd_decoder_set_option_bool(chd_decoder_t *d, const char *name, int v);
chd_status_t chd_decoder_set_option_str(chd_decoder_t *d, const char *name, const char *v);
chd_status_t chd_decoder_has_option(const chd_decoder_t *d, const char *name);
```

Strongly-typed setters keyed by the option-name macros in the
[registry](#option-registry). Setting an option that is not meaningful for the
decoder kind returns `CHD_E_INVALID_ARG`. `chd_decoder_has_option` reports
whether a name applies to this decoder.

!!! warning "Not concurrent with decode"
    Option setters and [`chd_decoder_commit`](#chd_decoder_commit) must not run
    concurrently with [`chd_decode_frame`](#chd_decode_frame) on the same
    decoder.

### chd_decoder_set_nn_model

```c
chd_status_t chd_decoder_set_nn_model(chd_decoder_t *d, chd_nn_model_t *m);
```

Attach a loaded [NN model](#neural-network-models) to a neural decoder kind.
The model must outlive the decoder; the decoder does not take ownership.

### chd_decoder_commit

```c
chd_status_t chd_decoder_commit(chd_decoder_t *d);
```

Apply pending options and prepare the decoder for use. **Required before the
first [`chd_decode_frame`](#chd_decode_frame).** Cheap to call repeatedly.
Call it again after changing options.

### chd_decoder_get_output_info

```c
chd_status_t chd_decoder_get_output_info(const chd_decoder_t *d,
                                         chd_output_info_t *out);
```

Fill `*out` with the committed [output framing](#chd_output_info_t) — the
post-crop/padding dimensions, pixel format, plane count, and frame count.
Requires a prior [`chd_decoder_commit`](#chd_decoder_commit); returns
`CHD_E_INVALID_ARG` otherwise. This is the only way to learn the output
dimensions without decoding a frame, which the decode-free
[dropout-detection](#dropout-detection) paths rely on.

### Option registry

Stable option names (string macros). The comment column gives the value type
and any decoder-kind restriction.

| Macro / name                        | Type | Notes                                                                                                                        |
|-------------------------------------|------|------------------------------------------------------------------------------------------------------------------------------|
| `CHD_OPT_CHROMA_GAIN`               | f64  | Chroma gain.                                                                                                                 |
| `CHD_OPT_CHROMA_PHASE_DEG`          | f64  | Chroma phase, degrees.                                                                                                       |
| `CHD_OPT_CHROMA_NR_LEVEL`           | f64  | Chroma noise reduction.                                                                                                      |
| `CHD_OPT_LUMA_NR_LEVEL`             | f64  | Luma noise reduction.                                                                                                        |
| `CHD_OPT_PADDING_MULTIPLE`          | i32  | Output padding multiple (default `1` = no padding).                                                                          |
| `CHD_OPT_REVERSE_FIELD_ORDER`       | bool | Swap field order (matches `ld-chroma-decoder -r`).                                                                           |
| `CHD_OPT_PHASE_COMPENSATION`        | bool | NTSC phase compensation.                                                                                                     |
| `CHD_OPT_COMB_DIMENSIONS`           | i32  | Comb dimensionality, in `{1,2,3}`.                                                                                           |
| `CHD_OPT_COMB_ADAPTIVE`             | bool | Adaptive comb.                                                                                                               |
| `CHD_OPT_COMB_ADAPT_THRESHOLD`      | f64  | Adaptive threshold.                                                                                                          |
| `CHD_OPT_COMB_CHROMA_WEIGHT`        | f64  | Chroma weighting.                                                                                                            |
| `CHD_OPT_COMB_SHOW_MAP`             | bool | Visualise the comb decision map.                                                                                             |
| `CHD_OPT_TRANSFORM_THRESHOLD`       | f64  | Transform-decoder threshold.                                                                                                 |
| `CHD_OPT_TRANSFORM_THRESHOLDS_FILE` | str  | Per-bin thresholds file.                                                                                                     |
| `CHD_OPT_FIRST_ACTIVE_FIELD_LINE`   | i32  | First active field line (inclusive).                                                                                         |
| `CHD_OPT_LAST_ACTIVE_FIELD_LINE`    | i32  | Last active field line (inclusive).                                                                                          |
| `CHD_OPT_FIRST_ACTIVE_FRAME_LINE`   | i32  | First active frame line (inclusive).                                                                                         |
| `CHD_OPT_LAST_ACTIVE_FRAME_LINE`    | i32  | Last active frame line (inclusive — the line is included in the output).                                                     |
| `CHD_OPT_NN_INPUT_MAGNITUDE_SCALE`  | f64  | nnTransform3D input magnitude scale.                                                                                         |
| `CHD_OPT_NN_CHROMA_BANDPASS`        | bool | ldzeug2 luma-sep chroma bandpass.                                                                                            |
| `CHD_OPT_OUTPUT_FORMAT`             | str  | `"yuv444p16"`, `"yuv444ps"`, `"rgb48"`, `"rgbs"`, `"gray16"`, or `"grays"`.                                                  |
| `CHD_OPT_OUTPUT_CLAMP`              | str  | `"none"` (default), `"legal_rgb_sdr"`, `"legal_rgb_hdr"`, or `"legal_ycbcr_bt601"`. See [Output clamping](#output-clamping). |
| `CHD_OPT_OUTPUT_Y4M_HEADERS`        | bool | Emit Y4M stream headers.                                                                                                     |
| `CHD_OPT_THREAD_COUNT`              | i32  | Worker threads (`0` = auto).                                                                                                 |

### chd_decode_frame

```c
chd_status_t chd_decode_frame(chd_decoder_t *d, int64_t frame_index, chd_frame_t **out);
```

Decode a single frame by index (random access). On `CHD_OK`, `*out` is a new
[frame](#frames) the caller must free with [`chd_frame_free`](#chd_frame_free).
An out-of-range index returns `CHD_E_OUT_OF_RANGE`. Requires a prior
[`chd_decoder_commit`](#chd_decoder_commit).

### chd_decode_frames_async

```c
typedef void (*chd_frame_done_cb)(void *user, chd_status_t s, int64_t idx, chd_frame_t *f);

chd_status_t chd_decode_frames_async(chd_decoder_t *d,
                                     const int64_t *indices, size_t n,
                                     chd_frame_done_cb cb, void *user,
                                     chd_cancel_t *cancel_or_null);
```

Decode `n` frames identified by `indices`, fanning the work across the
decoder's worker pool. For each index the callback fires with the resulting
status, the index, and the frame.

!!! warning "Blocking, multi-threaded, and you own the frame"
    Despite the name, this call **blocks until every frame has been delivered**.
    It joins all workers before returning. The callback runs on **worker
    threads**, possibly concurrently, so it must be thread-safe. Each delivered
    frame is owned by your callback: call [`chd_frame_free`](#chd_frame_free)
    when done with it. On cancellation the callback receives `CHD_E_CANCELLED`
    with a `NULL` frame (nothing to free). Pass `NULL` for `cancel_or_null` to
    disable cancellation.

Requires a prior [`chd_decoder_commit`](#chd_decoder_commit).

---

## Frames

Declared in `<chromadec/frame.h>`. A `chd_frame_t` holds one decoded frame's
planes in the decoder's configured output format.

### chd_frame_get_info

```c
chd_status_t chd_frame_get_info(const chd_frame_t *f, chd_frame_info_t *out);
```

Fill `*out` with the frame's [format, dimensions, plane count, and
index](#chd_frame_info_t).

### chd_frame_get_plane

```c
chd_status_t chd_frame_get_plane(const chd_frame_t *f, chd_plane_t p,
                                 const void **out_data,
                                 ptrdiff_t *out_stride_bytes);
```

Zero-copy borrow of a read-only pointer to a 16-bit plane `p` and its row stride
in **bytes**. The pointer is owned by the frame. Do not free it, and do not use
it after [`chd_frame_free`](#chd_frame_free). Valid for the integer pixel formats;
which planes are valid depends on the frame's [pixel format](#chd_frame_info_t)
(Y/Cb/Cr for `CHD_PIXEL_YUV444P16`, R/G/B for `CHD_PIXEL_RGB48`, or a single Y
plane for `CHD_PIXEL_GRAY16`). For float frames use
[`chd_frame_get_plane_float`](#chd_frame_get_plane_float).

### chd_frame_get_plane_float

```c
chd_status_t chd_frame_get_plane_float(const chd_frame_t *f, chd_plane_t p,
                                       const float **out_data,
                                       ptrdiff_t *out_stride_bytes);
```

Zero-copy borrow of a `float` plane — same borrowing mechanism and
ownership/lifetime rules as [`chd_frame_get_plane`](#chd_frame_get_plane), over
the frame's float storage. Valid for the float pixel formats:

- `CHD_PIXEL_YUV444PS` exposes `E′Y` (plane Y, `0.0` = black, `1.0` = white)
  and `E′Cb`/`E′Cr` (planes Cb/Cr, centred at `0.0` with a `±0.5` range).
- `CHD_PIXEL_GRAYS` exposes `E′Y` only (plane Y).
- `CHD_PIXEL_RGBS` exposes `E′R`/`E′G`/`E′B` (planes R/G/B, `0.0` = black, `1.0`
  = white). Computed directly from the decoder's component signals via the
  BT.601/H.273 MatrixCoefficients=5/6 Y′CbCr → R′G′B′ matrix; no intermediate
  Y′CbCr integer quantization.

For `CHD_PIXEL_YUV444PS` and `CHD_PIXEL_GRAYS` these are the normalized
colour-difference signals `E′Y E′Cb E′Cr` of ITU-R BT.601 / ITU-T H.273; the
integer formats are narrow-range quantizations of the same signals, so float
output preserves full precision before quantization.

## Output clamping

`CHD_OPT_OUTPUT_CLAMP` controls how out-of-range or sync-reserved sample codes
are handled.

| Token               | Meaning                                                                                                                                                                                                                                                                                                                                                                                       |
|---------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `none` (default)    | No signal-domain clamp. Integer formats still saturate at the bit-depth limits per ITU-T H.273 `Clip1` (`[0, 65535]` in 16-bit). Float formats emit raw values which may fall below `0` or above `1`. Most faithful to the decoded signal.                                                                                                                                                    |
| `legal_rgb_sdr`     | Limit output to values that map between R′G′B′ black and white. Y′CbCr formats: `Y′` stays between black (`16·256`) and white (`235·256`); `Cb`/`Cr` stays within the standard `±112·256` excursion around neutral gray (`128·256`). RGB formats: components are clamped to `[black, white]` = `[0, 1]`. Suppresses super-white, sub-black, and over-saturated chroma.                        |
| `legal_rgb_hdr`     | Map to positive-only R′G′B′ with unconstrained headroom past SDR white: components are floored at black (`0`) with no ceiling, preserving HDR highlights and removing negative excursion. Affects RGB formats (`rgbs`; `rgb48` already saturates at `0`); a **no-op** for Y′CbCr / GRAY formats, which have no clean per-component box for the positive-R′G′B′ region.                        |
| `legal_ycbcr_bt601` | Clamp to values that map to ITU-R BT.601-7 §2.5.3 video-allowed codes `[1.00d, 254.75d]` scaled to output bit-depth. Useful before quantizing for downstream SD-SDI or DV transport. In RGBS, it clamps components to bounds achievable from BT.601-legal Y′CbCr (`R′∈[-0.863, +1.884]`, `G′∈[-0.667, +1.690]`, `B′∈[-1.073, +2.093]`). Doesn't affect integer RGB which is already stricter. |

For the GRAY formats (`gray16`, `grays`) the RGB-domain modes act on the luma
axis only, because a luma sample alone cannot carry R′G′B′ legality — `grays`
may be the `E′Y` of a split decode (for example luma from one decoder and
`E′Cb`/`E′Cr` from another), in which case the eventual R′G′B′ depends on
chroma that is not present in this frame. Accordingly, `legal_rgb_sdr` clamps
`Y′`/`E′Y` to the nominal narrow-range luma extent (`[16·256, 235·256]` /
`[0, 1]`), which doubles as a sensible luma clamp, while `legal_rgb_hdr` is a
no-op (flooring luma at black would be an R′G′B′ assumption with no luma-domain
justification). Use `legal_ycbcr_bt601` for the BT.601 §2.5.3 sync-safe luma
range, or `none` to preserve all headroom for later recombination.

The scaling relationship between "limited-range" matrix coefficient values like
Y′CbCr and their corresponding R′G′B′ values is unaffected by clamp options.
For example, `CHD_OPT_OUTPUT_CLAMP=none` with "yuv444p16" will *not* result in
"full-range" JFIF Y′CbCr.

The `legal_ycbcr_bt601` clamp matches the default behavior of tools like
decode-orc and `ld-chroma-decoder`.

### chd_frame_free

```c
void chd_frame_free(chd_frame_t *f);
```

Release a frame and invalidate every plane pointer previously borrowed from
it. Safe on `NULL`.

---

## Dropout correction

Declared in `<chromadec/dropout.h>`. Dropout correction replaces damaged
samples, optionally drawing on the [extra sources](#extra-sources-for-multi-source-dropout)
registered on the video.

```c
typedef struct chd_dropout_opts {
    int enabled;
    int overcorrect;        /* extend dropout boundaries by ±24 samples */
    int intra_field_only;   /* skip cross-field replacement candidates */
} chd_dropout_opts_t;

typedef struct chd_dropout_stats {
    int32_t corrected;
    int32_t failed;
    int64_t total_distance;
} chd_dropout_stats_t;
```

### chd_decoder_set_dropout

```c
chd_status_t chd_decoder_set_dropout(chd_decoder_t *d, const chd_dropout_opts_t *opts);
```

Configure dropout correction on a decoder. Takes effect at the next
[`chd_decoder_commit`](#chd_decoder_commit).

### chd_decoder_get_last_dropout_stats

```c
chd_status_t chd_decoder_get_last_dropout_stats(const chd_decoder_t *d,
                                                chd_dropout_stats_t *out);
```

Return the correction counters from the **most recent**
[`chd_decode_frame`](#chd_decode_frame) on this decoder.

## Dropout detection

The functions above *conceal* dropouts during a decode; these *expose* the
flagged regions without running the chroma decoder, so a consumer can build its
own visualisation (for example a mask clip marking damaged areas). Dropouts are
detected upstream and stored in the source metadata, so these work regardless of
whether concealment is enabled — and pair naturally with a
[`CHD_DEC_NONE`](#chd_decoder_create) decoder to skip chroma decoding entirely.

```c
typedef struct chd_dropout_span {
    int32_t y;        /* active-output row */
    int32_t x_start;  /* half-open [x_start, x_end) within the active width */
    int32_t x_end;
} chd_dropout_span_t;
```

### chd_decoder_get_dropout_spans

```c
chd_status_t chd_decoder_get_dropout_spans(chd_decoder_t *d, int64_t frame_index,
                                           chd_dropout_span_t **out_spans,
                                           size_t *out_count);
void         chd_dropout_spans_free(chd_dropout_span_t *spans);
```

Return the raw detected dropout regions for one frame, mapped into the committed
[output framing](#chd_output_info_t): each span's `y`, `x_start`, and `x_end`
index the same coordinate space as [`chd_frame_get_plane`](#chd_frame_get_plane)
for a frame from the same committed decoder (interlace weave, crop, padding, and
field order applied; spans clipped to the active picture, sorted by `y` then
`x_start`). On `CHD_OK`, `*out_spans` is a newly-allocated array of `*out_count`
spans the caller releases with `chd_dropout_spans_free`; a frame with no
dropouts yields `*out_count == 0` and `*out_spans == NULL`. An out-of-range
index returns `CHD_E_OUT_OF_RANGE`. Requires a prior
[`chd_decoder_commit`](#chd_decoder_commit).

### chd_decode_dropout_mask

```c
chd_status_t chd_decode_dropout_mask(chd_decoder_t *d, int64_t frame_index,
                                     chd_frame_t **out);
```

Rasterise the same detected regions into a single-plane [frame](#frames)
matching the output framing: `0` for clean samples, set for dropped ones. The
mask format follows the committed output format's precision domain: a float
output format (`yuv444ps`, `rgbs`, `grays`) yields a
[`CHD_PIXEL_GRAYS`](#enumerations) mask (`1.0` dropped), any integer format
yields a [`CHD_PIXEL_GRAY16`](#enumerations) mask (`0xFFFF` dropped). The mask
clip pairs with the decode clip's sample type. Read it with
[`chd_frame_get_info`](#chd_frame_get_info) /
[`chd_frame_get_plane`](#chd_frame_get_plane) (or
[`chd_frame_get_plane_float`](#chd_frame_get_plane_float) for a `GRAYS` mask) and
free it with [`chd_frame_free`](#chd_frame_free). Does not run the chroma
decoder.

---

## Neural-network models

Declared in `<chromadec/nn.h>`. Available only when the library was built with
NN support (`chd_has_feature("nn")`). A `chd_nn_model_t` wraps a loaded ONNX
model and its execution-provider session.

### Execution providers

```c
typedef enum chd_nn_provider {
    CHD_NN_EP_AUTO     = 0,   /* platform default chain */
    CHD_NN_EP_CPU      = 1,
    CHD_NN_EP_CUDA     = 2,
    CHD_NN_EP_TENSORRT = 3,
    CHD_NN_EP_COREML   = 4,
    CHD_NN_EP_DIRECTML = 5,
    CHD_NN_EP_MIGRAPHX = 6
} chd_nn_provider_t;
```

### Session options

```c
typedef struct chd_nn_session_opts {
    chd_nn_provider_t provider;     /* AUTO unless caller pins */
    int32_t device_id;              /* 0 unless multi-GPU */
    int     enable_graph_optim;     /* default 1 */
    int     enable_mem_pattern;     /* default 1 */
    int32_t inter_op_threads;       /* default 1 */
    int32_t intra_op_threads;       /* default 1 */
    const char *engine_cache_dir;   /* see below */
    void *reserved[4];              /* zero-initialised; do not repurpose */
} chd_nn_session_opts_t;
```

`engine_cache_dir` controls caching of compiled EP engines (TensorRT plans,
MIGraphX binaries), which otherwise recompile on the first inference (a
15–30 s cost):

- `NULL`: auto-pick a per-user cache directory (created if absent):
  `$XDG_CACHE_HOME/chromadec` (Linux), `$HOME/Library/Caches/chromadec`
  (macOS), `%LOCALAPPDATA%/chromadec` (Windows).
- `""`: caching disabled (recompile every load; useful for CI and cold-start
  benchmarking).
- *path*: use this absolute directory (created if it doesn't exist).

Honoured by the TensorRT and MIGraphX EPs; the CUDA EP uses its own internal
PTX cache that is not configurable here.

!!! note "Why threads default to 1"
    The decoder pool already parallelises across frames; raising the intra-op
    thread count oversubscribes the CPU.

The `reserved` array exists so future minor versions can add fields without
breaking source compatibility; always leave it zeroed.
[`chd_nn_session_opts_default`](#chd_nn_session_opts_default) does this for you.

### chd_nn_session_opts_default

```c
void chd_nn_session_opts_default(chd_nn_session_opts_t *out);
```

Fill `*out` with default session options (AUTO provider, the defaults noted
above, zeroed reserved fields). Always initialise via this function rather than
by hand, so new fields pick up correct defaults.

### chd_nn_model_load_from_file / chd_nn_model_load_from_memory / chd_nn_model_free { #chd_nn_model_load_from_file }

```c
chd_status_t chd_nn_model_load_from_file(const char *model_path,
                                         const chd_nn_session_opts_t *opts_or_null,
                                         chd_nn_model_t **out);
chd_status_t chd_nn_model_load_from_memory(const void *model_data,
                                           size_t model_size,
                                           const chd_nn_session_opts_t *opts_or_null,
                                           chd_nn_model_t **out);
void chd_nn_model_free(chd_nn_model_t *m);
```

Load an ONNX model — either from a file on disk
(`chd_nn_model_load_from_file`) or from an in-memory buffer
(`chd_nn_model_load_from_memory`), for callers that embed the model as a
compiled-in byte array and want no filesystem dependency. The buffer is
consumed during the call and need not outlive it. Both produce an identical
`chd_nn_model_t`. `opts_or_null` of `NULL` uses
[defaults](#chd_nn_session_opts_default). On `CHD_E_NN_PROVIDER_UNAVAILABLE`
the pinned provider isn't available in this build/host;
`CHD_E_NN_MODEL_LOAD` indicates a bad or unreadable model. Remember
[`chd_shutdown`](#chd_shutdown) before exit once any model has been loaded.

### chd_nn_model_get_active_provider

```c
chd_status_t chd_nn_model_get_active_provider(const chd_nn_model_t *m,
                                              chd_nn_provider_t *out);
```

Report the execution provider actually selected for a loaded model. Useful
after `CHD_NN_EP_AUTO`, which resolves a platform-specific chain.

### chd_nn_provider_is_available

```c
int chd_nn_provider_is_available(chd_nn_provider_t p);   /* 1 = yes, 0 = no */
```

Query whether a provider can be used on this build and host without attempting
a model load.

---

## Cancellation

Declared in `<chromadec/pipeline.h>`. A `chd_cancel_t` is an optional
cooperative-cancellation token for [`chd_decode_frames_async`](#chd_decode_frames_async).

```c
chd_status_t chd_cancel_create(chd_cancel_t **out);
void         chd_cancel_request(chd_cancel_t *c);
int          chd_cancel_is_requested(const chd_cancel_t *c);
void         chd_cancel_free(chd_cancel_t *c);
```

Create a token, pass it to an async decode, and call `chd_cancel_request` from
any thread to ask the in-flight workers to stop. Remaining indices are
delivered to the callback as `CHD_E_CANCELLED` with a `NULL` frame.
`chd_cancel_is_requested` polls the flag. Free the token after the async call
returns.

!!! note "Threading model"
    The only thread-count knob is the `CHD_OPT_THREAD_COUNT` decoder option.
    Pools are **per-decoder**: `N` decoders running `T` threads each can spawn
    up to `N × T` workers in your process.