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
  (`chd_version`, `chd_cancel_is_requested`, `chd_nn_backend_is_available`,
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

Query optional build features. Recognised names: `"nn"`, `"onnxruntime"`,
`"coreml"`, `"cuda"`, `"fftw"`, `"sqlite"`. Returns `0` for `NULL` or any unknown
name.

`"nn"` reports whether the neural-decoder framework is present — true when *any*
inference backend is built. The individual backends have their own flags:
`"onnxruntime"` (the ONNX Runtime backend, build option `with_onnxruntime`) and
`"coreml"` (the native CoreML backend, build option `with_coreml`). The two are
independent: a macOS build with `-Dwith_onnxruntime=false -Dwith_coreml=enabled`
reports `"nn"`=1, `"coreml"`=1, `"onnxruntime"`=0, and runs the ldzeug and
nnTransform3D decoders entirely on native CoreML (`.onnx` models and the
`CHD_NN_ORT_*` backends are then unavailable — see
[`chd_nn_model_load_from_file`](#chd_nn_model_load_from_file)).

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
| `CHD_E_NN_BACKEND_UNAVAILABLE`  | The requested inference backend is not available in this build/host.                      |
| `CHD_E_NN_INFERENCE`            | Inference failed at runtime.                                                              |
| `CHD_E_OUT_OF_RANGE`            | A frame index (or similar) is outside the valid range.                                    |
| `CHD_E_CANCELLED`               | The operation was cancelled via a [`chd_cancel_t`](#cancellation).                        |
| `CHD_E_INTERNAL`                | An unexpected internal error.                                                             |
| `CHD_E_OOM`                     | Allocation failure.                                                                       |
| `CHD_E_UNSUPPORTED`             | The query does not apply to this object (e.g. a chroma-ident query on a non-4:4:0 frame). |

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
| `chd_video_standard_t` | `CHD_STD_UNKNOWN`, `CHD_STD_NTSC`, `CHD_STD_PAL`, `CHD_STD_PAL_M`, `CHD_STD_SECAM` |
| `chd_sample_encoding_t` | `CHD_ENC_UNKNOWN`, `CHD_ENC_CVBS_U10_4FSC`, `CHD_ENC_CVBS_U16_4FSC`, `CHD_ENC_CVBS_TPG21_4FSC`, `CHD_ENC_CVBS_S16_FSC`, `CHD_ENC_RAW_S16_28M`, `CHD_ENC_RAW_S16_40M` |
| `chd_signal_state_t` | `CHD_SIG_UNKNOWN`, `CHD_SIG_STANDARD_TBC_LOCKED`, `CHD_SIG_STANDARD_TBC_UNLOCKED`, `CHD_SIG_STANDARD_RAW`, `CHD_SIG_NONSTANDARD_TBC_LOCKED`, `CHD_SIG_NONSTANDARD_TBC_UNLOCKED`, `CHD_SIG_NONSTANDARD_RAW` |
| `chd_frame_layout_t` | `CHD_FRAME_LAYOUT_UNKNOWN`, `CHD_FRAME_LAYOUT_FIELD_RASTER`, `CHD_FRAME_LAYOUT_FRAME_NATIVE` |
| `chd_plane_t` | `CHD_PLANE_Y`, `CHD_PLANE_CB`, `CHD_PLANE_CR`, `CHD_PLANE_R`, `CHD_PLANE_G`, `CHD_PLANE_B` |
| `chd_pixel_format_t` | `CHD_PIXEL_YUV444P16`, `CHD_PIXEL_YUV444PS`, `CHD_PIXEL_RGB48`, `CHD_PIXEL_RGBS`, `CHD_PIXEL_GRAY16`, `CHD_PIXEL_GRAYS`, `CHD_PIXEL_YUV440P16`, `CHD_PIXEL_YUV440PS` |
| `chd_chroma_row_component_t` | `CHD_CHROMA_ROW_DB`, `CHD_CHROMA_ROW_DR` |
| `chd_chroma_ident_mechanism_t` | `CHD_CHROMA_IDENT_PORCH`, `CHD_CHROMA_IDENT_BOTTLES`, `CHD_CHROMA_IDENT_CONTENT`, `CHD_CHROMA_IDENT_MANUAL` |
| `chd_clamp_t` | `CHD_CLAMP_NONE`, `CHD_CLAMP_LEGAL_RGB_SDR`, `CHD_CLAMP_LEGAL_RGB_HDR`, `CHD_CLAMP_LEGAL_YCBCR_BT601` |

`chd_frame_layout_t` is the container addressing of a CVBS data file:
`FIELD_RASTER` for the fixed padded `field_width x field_height` blocks every
`.tbc` uses, `FRAME_NATIVE` for the CVBS specification's exact frame-addressed
totals. See [file formats](file-formats.md#container-layouts) for what each
layout means and how it is detected.

### chd_video_params_t

Caller-supplied parameters used to override or supply metadata when opening a
source (see [`chd_video_open_composite`](#chd_video_open_composite)). The
first three identify the capture when no sidecar is found: `standard`,
`encoding`, and `signal_state` are required there. When an ld-decode sidecar
is present, a non-zero `standard` re-declares the colour standard over the
sidecar's, for captures whose sidecar cannot express it (a vhs-decode ME-SECAM
sidecar says `PAL`); the declared standard must keep the capture's line
standard, so a 525-line capture cannot be re-declared 625-line or vice
versa. `CHD_STD_SECAM` works the same way for CVBS sources: it selects the
byte-compatible 625/50 `PAL` preset for geometry (the CVBS specification has
no SECAM preset yet; see [file formats](file-formats.md#shape-of-the-format))
and re-declares the opened source SECAM, requiring a `PAL` preset when a
`.meta` sidecar is present. The remaining three merge over sidecar metadata,
because the CVBS
`.meta` schema does not carry them: `layout` (`CHD_FRAME_LAYOUT_UNKNOWN`
means auto-detect), `is_subcarrier_locked` (set to mark an encoder-style
subcarrier-locked field raster; field rasters default to line-locked), and
`is_second_field_first` (set to declare a field-swapped capture, where the
temporally-first field of each stored pair is not the interlace first field).

```c
typedef struct chd_video_params {
    chd_video_standard_t  standard;
    chd_sample_encoding_t encoding;
    chd_signal_state_t    signal_state;
    chd_frame_layout_t    layout;
    int      is_subcarrier_locked;
    int      is_second_field_first;
} chd_video_params_t;
```

### chd_video_info_t

Read-back description of an opened video, filled by
[`chd_video_get_info`](#chd_video_get_info). Superset of the params struct:
adds the derived `fsc_hz` subcarrier frequency, the decodable `num_frames`
count, and `samples_per_frame`, the standard's native frame total (PAL
709,379; NTSC 477,750; PAL-M 477,225) regardless of container layout, while
`field_width` and `field_height` describe the served field raster. `layout`
is always the resolved value, never `CHD_FRAME_LAYOUT_UNKNOWN`. For
frame-native sources, `active_video_start` / `active_video_end` follow the
[horizontal alignment](file-formats.md#container-layouts) measured from the
capture's own sync at open time.

```c
typedef struct chd_video_info {
    chd_video_standard_t  standard;
    chd_sample_encoding_t encoding;
    chd_signal_state_t    signal_state;
    chd_frame_layout_t    layout;
    int32_t  field_width;
    int32_t  field_height;
    int32_t  samples_per_frame;
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

### chd_plane_info_t

```c
typedef struct chd_plane_info {
    int32_t width;
    int32_t height;
    int32_t first_frame_row;
} chd_plane_info_t;
```

Per-plane geometry, filled by
[`chd_frame_get_plane_info`](#chd_frame_get_plane_info). Full-height planes
report the frame dimensions and `first_frame_row` 0. The 4:4:0 chroma planes
report their subsampled height and the output frame row their first row was
decoded from; see [4:4:0 output](#440-output).

### chd_chroma_ident_report_t

```c
typedef struct chd_chroma_ident_report {
    chd_chroma_ident_mechanism_t mechanism;
    double confidence;
    double field_confidence[2];
    chd_chroma_row_component_t first_row_component;
} chd_chroma_ident_report_t;
```

Per-frame Db/Dr ident summary for line-sequential (SECAM) decodes, filled by
[`chd_frame_get_chroma_ident`](#chd_frame_get_chroma_ident): which mechanism
decided the per-line component identity, the fraction of measured lines that
agreed with the majority lattice (overall and per field, in frame order;
`1.0` for `manual`), and the component of the frame's first output row.

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
chd_status_t chd_video_open_composite(const char *path,
                                      const char *sidecar_path_or_null,
                                      const chd_video_params_t *override_or_null,
                                      chd_video_t **out);
```

Open a single-file composite capture: an ld-decode `.tbc` or a CVBS
`.composite`. The sidecar flavour is detected automatically.

- `sidecar_path_or_null`: `NULL` auto-locates the sidecar next to the data
  file: an ld-decode `<path>.db` (SQLite) / `<path>.json`, else a CVBS
  `<basename>.meta`. A `const char *` is an explicit path to a `.db`, `.json`,
  or `.meta` sidecar.
- `override_or_null`: `NULL` means all parameters come from the sidecar. When
  no sidecar is found, the override is mandatory and must set `standard`,
  `encoding`, and `signal_state`; the open fails otherwise. When a sidecar is
  present, `layout`, `is_subcarrier_locked`, and `is_second_field_first` are
  read from the override, and a non-zero `standard` re-declares the colour
  standard (see [chd_video_params_t](#chd_video_params_t)). See the
  [which-fields-to-set matrix](integration-guide.md#which-fields-to-set).

CVBS opens also measure signal properties the `.meta` sidecar cannot express:
the horizontal alignment of frame-native rows, and for NTSC each field's
four-field sequence position from its colour burst. See
[file formats](file-formats.md#container-layouts) for both.

### chd_video_open_yc

```c
chd_status_t chd_video_open_yc(const char *luma_path,
                               const char *chroma_path,
                               const char *sidecar_path_or_null,
                               const chd_video_params_t *override_or_null,
                               chd_video_t **out);
```

Open a dual-file Y/C capture: a CVBS `.y` + `.c` pair, or a vhs-decode luma
`.tbc` + chroma `.tbc` pair. Sidecar resolution and flavour detection follow
[`chd_video_open_composite`](#chd_video_open_composite); the
`sidecar_path_or_null` applies to the luma plane, and the chroma plane uses its
own sidecar if present, else falls back to the luma sidecar (vhs-decode writes a
single shared `<base>.tbc.json` for the pair).

For a vhs-decode pair the two planes are decoded separately and merged: the
luma plane is decoded with the Mono kind for Y and the chroma plane with the
configured colour kind for Cb/Cr. A CVBS `.y`/`.c` pair instead reconstructs a
composite from the centred-chroma `.c` and decodes it in one pass.

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
chd_status_t chd_video_add_extra_source_composite(chd_video_t *v,
                                                  const char *path,
                                                  const char *sidecar_path_or_null);
chd_status_t chd_video_add_extra_source_yc(chd_video_t *v,
                                           const char *luma_path,
                                           const char *chroma_path,
                                           const char *sidecar_path_or_null);
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
| `CHD_DEC_SECAM`                                    | SECAM line-sequential FM chroma (4:4:0 output). |

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
| `CHD_OPT_COMB_ADAPT_THRESHOLD`      | f64  | Adaptive 3D candidate threshold; `CHD_DEC_NTSC_3D` only.                                                                     |
| `CHD_OPT_COMB_CHROMA_WEIGHT`        | f64  | Adaptive 3D chroma penalty weight; `CHD_DEC_NTSC_3D` only.                                                                   |
| `CHD_OPT_COMB_SHOW_MAP`             | bool | Overlay the adaptive 3D decision map; `CHD_DEC_NTSC_3D` only.                                                                |
| `CHD_OPT_CHROMA_FILTER`             | str  | `"compat"` (default), `"equiband_wide"` (NTSC), `"equiband"`, `"color_under"`, `"wideband_i_ssb"` (NTSC), `"equiband_vsb"` (PAL). See [Chroma filter](#chroma-filter).      |
| `CHD_OPT_CHROMA_UPPER_SIDEBAND_HZ`  | f64  | Upper-sideband room +X above fSC; `"equiband_vsb"` only. See [Chroma filter](#chroma-filter).                                |
| `CHD_OPT_CHROMA_IDENT_MODE`         | str  | `"auto"` (default), `"porch"`, `"bottles"`, or `"manual"`; `CHD_DEC_SECAM` only. See [SECAM line identification](#secam-line-identification). |
| `CHD_OPT_CHROMA_IDENT_MANUAL`       | str  | `"db_first"` or `"dr_first"`; required iff `chroma_ident_mode` is `"manual"`.                                                |
| `CHD_OPT_CHROMA_CLICK_NR_LEVEL`     | f64  | SECAM FM click concealment, `0.0`–`1.0` (default `1.0`; `0.0` bypasses the stage). See [SECAM click concealment](#secam-click-concealment). |
| `CHD_OPT_CHROMA_CLICK_ENV_DIP_DB`   | f64  | Expert absolute override of the adaptive envelope-dip threshold (dB); needs `chroma_click_nr_level` > 0.                     |
| `CHD_OPT_CHROMA_CLICK_FREQ_OVERSHOOT` | f64 | Expert absolute override of the adaptive deviation-overshoot threshold (max-deviation multiples); needs `chroma_click_nr_level` > 0. |
| `CHD_OPT_TRANSFORM_THRESHOLD`       | f64  | Transform-decoder threshold.                                                                                                 |
| `CHD_OPT_TRANSFORM_THRESHOLDS_FILE` | str  | Per-bin thresholds file.                                                                                                     |
| `CHD_OPT_FIRST_ACTIVE_FIELD_LINE`   | i32  | First active field line (inclusive).                                                                                         |
| `CHD_OPT_LAST_ACTIVE_FIELD_LINE`    | i32  | Last active field line (inclusive).                                                                                          |
| `CHD_OPT_FIRST_ACTIVE_FRAME_LINE`   | i32  | First active frame line (inclusive).                                                                                         |
| `CHD_OPT_LAST_ACTIVE_FRAME_LINE`    | i32  | Last active frame line (inclusive — the line is included in the output).                                                     |
| `CHD_OPT_NN_INPUT_MAGNITUDE_SCALE`  | f64  | nnTransform3D input magnitude scale.                                                                                         |
| `CHD_OPT_NN_CHROMA_BANDPASS`        | bool | ldzeug2 luma-sep chroma bandpass.                                                                                            |
| `CHD_OPT_OUTPUT_FORMAT`             | str  | `"yuv444p16"`, `"yuv444ps"`, `"rgb48"`, `"rgbs"`, `"gray16"`, `"grays"`, `"yuv440p16"`, or `"yuv440ps"` (the 4:4:0 pair is SECAM-only; see [4:4:0 output](#440-output)). |
| `CHD_OPT_OUTPUT_CLAMP`              | str  | `"none"` (default), `"legal_rgb_sdr"`, `"legal_rgb_hdr"`, or `"legal_ycbcr_bt601"`. See [Output clamping](#output-clamping). |
| `CHD_OPT_COLOR_DIFFERENCE_PRECISION` | str | `"classic"` or `"modern"` (default). Precision of the luma matrix coefficients. See [Colour conversion precision](#colour-conversion-precision).   |
| `CHD_OPT_BROADCAST_SCALING_PRECISION` | str | `"classic"`, `"modern"`, or `"scientific"` (default). Precision of the U/V reduction factors. See [Colour conversion precision](#colour-conversion-precision). |
| `CHD_OPT_OUTPUT_Y4M_HEADERS`        | bool | Emit Y4M stream headers.                                                                                                     |
| `CHD_OPT_THREAD_COUNT`              | i32  | Worker threads (`0` = auto).                                                                                                 |

### SECAM line identification { #secam-line-identification }

`CHD_OPT_CHROMA_IDENT_MODE` selects how the SECAM decoder resolves each
line's Db/Dr identity. Whatever the mode, per-line decisions feed a
strict-alternation majority fit per field, so single-line measurement errors
self-heal, and the result is reported per frame through
[`chd_frame_get_chroma_ident`](#chd_frame_get_chroma_ident).

- `"auto"` (default): back-porch reference-carrier measurement, preferred
  when enough lines measure cleanly; field-ident bottles cross-check it when
  present, take over when the porch is blanked, and content statistics are
  the last fallback.
- `"porch"`: line identification only, no fallback. The reported confidence
  still shows when this was a bad idea.
- `"bottles"`: the vertical-interval ident trapezoids only, for sources with
  blanked porches but intact vertical intervals.
- `"manual"`: no measurement; a fixed lattice anchored by
  `CHD_OPT_CHROMA_IDENT_MANUAL`, which names the component of the first
  active line of the first field of frame 0. The deterministic four-field
  alternation (Rec. ITU-R BR.469) extends it across the capture. For
  pathological captures and deterministic re-decodes.

The porch measurement doubles as per-field carrier calibration: the decoder
clusters the measured per-line reference carriers into the two undeviated
subcarriers and discriminates against the measured pair, absorbing converter
offsets (an ME-SECAM deck's free-running conversion arithmetic) without
assuming absolute carrier positions. The calibration also recentres the
chroma band and the inverse HF pre-correction bell on the measured pair:
a converter offset arises after encoding and translates the whole FM block,
bell shaping included, so an inverse left at nominal would sit on the wrong
centre (measured on an ME-SECAM capture with carriers +108 kHz off nominal:
colour-difference overshoot at large bar transitions drops from roughly
twice the step to a few percent once the inverse follows the block). When
the porch pair is unmeasurable, the nominal 4.25/4.40625 MHz subcarriers
apply. Opening a 625-line capture
whose measured porch signature contradicts its declared standard (a PAL
declaration over an alternating SECAM carrier pair, or the reverse) logs a
warning; the declaration always wins.

### SECAM click concealment { #secam-click-concealment }

FM clicks ("SECAM fire") on low-SNR tape are not fixable by a better
discriminator formula, so the decoder conceals them after demodulation,
enabled by default at full level. Detection flags discriminator samples
where the analytic envelope collapses below a threshold or the instantaneous
deviation exits the BT.1700 Part C Table 4 maxima; concealment interpolates
across narrow spans and substitutes the previous same-component line for
spans too wide to interpolate, before de-emphasis. `chroma_click_nr_level = 0`
bypasses the stage entirely.

Independent of the concealment stage, the demodulated deviation is always
clamped to the Table 4 maxima (D'B −350/+506 kHz, D'R −506/+350 kHz) before
de-emphasis. The transmitter clips the pre-corrected signal to those bounds,
so nothing beyond them is signal; the rail turns any click the concealment
stage left (or all of them, when bypassed) into a bounded flat-top instead
of an unbounded spike.

The thresholds come from a frozen formula composing the level with a
per-field chroma noise-floor estimate, measured deterministically from the
same back-porch windows used for ident and calibration (the median absolute
deviation of the per-line porch frequencies about their component's
carrier). With `level` in `0.0`–`1.0` and `noise` in Hz:

- envelope dip: `12 - 6*level` dB below the row's median analytic envelope;
- deviation overshoot: `max(2.6 - 1.6*level, 1.15) + 6*noise/506000` in
  multiples of the per-component maximum deviation. The floor keeps a
  transmitter limiter flat-top riding exactly at the Table 4 bounds from
  flagging on its own ripple; the deviation rail already bounds everything
  beneath the detection threshold.

Same capture and same level give bit-identical output; across captures the
effective thresholds adapt through the noise term. The endpoints were
calibrated with a swept-level study on the synthetic Table 4 generator. The
expert overrides replace either threshold with an absolute value for
batch-comparable decodes; `chd_decoder_get_chroma_click_thresholds` reports
the values actually applied to the most recent decode, so any decode is
auditable after the fact. Concealed spans are reported through
[`chd_decoder_get_dropout_spans`](#chd_decoder_get_dropout_spans) with
`CHD_DROPOUT_ORIGIN_DECODER_CONCEALMENT`, consistent with 4:4:0's
every-row-is-real honesty contract: consumers see exactly which chroma
samples are concealed rather than genuine.

### Chroma filter { #chroma-filter }

`CHD_OPT_CHROMA_FILTER` selects how the comb (NTSC: `CHD_DEC_NTSC_*`,
`CHD_DEC_NN_TRANSFORM3D`) and PalColour (PAL/PAL-M: `CHD_DEC_PAL_2D`,
`CHD_DEC_TRANSFORM_*`) decoders band-limit the demodulated chroma. There are two
things you can set:

- **The mode** (`CHD_OPT_CHROMA_FILTER`): what to do with the band. Pass the
  legacy/compat width, the standards equiband width, a narrow colour-under width,
  or *recover* the clipped vestigial sideband. A small closed set, shared across
  systems where applicable.
- **The upper-sideband cutoff** (`CHD_OPT_CHROMA_UPPER_SIDEBAND_HZ`): the
  frequency +X (Hz above the subcarrier) where the channel clipped the upper
  sideband. Only the PAL recovery mode `equiband_vsb` reads it; every other mode
  ignores it (and setting it there is rejected).

The shared, standards-clean modes are `equiband` (1.3 MHz, SMPTE ST 170 Annex
A.4 / ITU-R BT.1700) and the narrower `color_under` (~0.5 MHz, VHS/S-VHS), both
valid on either system. `compat` is the portable, **system-resolved** legacy
default (and what an unset option resolves to): it reproduces each decoder's
current behaviour, either ~2.2 MHz on NTSC (identical to `equiband_wide`) or the
1.1/0.93 dot-pattern-tuned ~1.18 MHz on PAL, so a caller can pin "today's decode"
without memorising a per-system token. The two recovery modes are named by method
because the algorithms differ, and they split by system: `wideband_i_ssb` (NTSC,
Hilbert single-sideband) and `equiband_vsb` (PAL, amplitude EQ).

| Mode             | NTSC                          | PAL / PAL-M                   | Upper-sideband +X |
|------------------|-------------------------------|-------------------------------|-------------------|
| `compat`         | ~2.2 MHz (≡ `equiband_wide`)  | 1.18 MHz (legacy default)     | n/a               |
| `equiband_wide`  | ~2.2 MHz                      | *invalid (use `compat`)*      | n/a               |
| `equiband`       | 1.3 MHz                       | 1.3 MHz                       | n/a               |
| `color_under`    | ~0.5 MHz                      | ~0.5 MHz                      | n/a               |
| `wideband_i_ssb` | wideband-I + Hilbert SSB      | *invalid*                     | fixed (built-in)  |
| `equiband_vsb`   | *invalid*                     | equiband + vestigial EQ       | required          |

Only `equiband_vsb` takes the upper-sideband cutoff as an option; `wideband_i_ssb`
has its geometry fixed by the built-in NTSC-1953 reconstruction filters (I to
1.3 MHz, Q to 0.6 MHz; set the asymmetry shape with
[`chd_decoder_set_chroma_sideband_calib`](#chd_decoder_set_chroma_sideband_calib)
instead), and the symmetric modes have no vestige to recover. Invalid
`(mode, system)` combinations are rejected at [`chd_decoder_commit`](#chd_decoder_commit)
with `CHD_E_INVALID_ARG`. The default (no option) and `compat` are
**byte-identical to the decoder's previous behaviour** on both systems; the
narrower modes and the recovery modes are opt-in only.

#### The equiband modes { #chroma-filter-equiband }

| Token            | Meaning                                                                                                                                                                                                                                                                            |
|------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `equiband`       | 1.3 MHz on both U and V; the nominal equiband U/V bandwidth of SMPTE ST 170 Annex A.4 / ITU-R BT.1700, and the same ~1.3 MHz baseband mask both systems apply before modulation. The right match for LaserDisc/LaserVision and anything from a modern equiband encoder.            |
| `color_under`    | ~0.5 MHz on both U and V; matches the surviving chroma bandwidth of VHS/S-VHS colour-under recordings, which take chroma off at ±0.5 MHz and re-modulate it symmetric-DSB (IEC 60774-1 §6.2; S-VHS shares the same chroma path). The colour-under process erases any vestigial/wideband-I structure, so a symmetric ~0.5 MHz low-pass is both necessary and sufficient (a wider filter only integrates colour-under noise). Symmetric, so it needs no phase compensation. |
| `equiband_wide`  | NTSC only. The widest equiband setting: a single ~2.2 MHz low-pass on both U and V. Keeps the most chroma detail, along with whatever cross-colour the comb let through. The looser-than-standards `ld-chroma-decoder` legacy default, which `compat` resolves to on NTSC. PAL has no wider-than-equiband option (its legacy default sits *below* 1.3 MHz and is reached via `compat`). |

#### NTSC recovery: `wideband_i_ssb` { #chroma-filter-wideband-i-ssb }

The NTSC-1953 asymmetric split (1.3 MHz on I, 0.6 MHz on Q) plus single-sideband
reconstruction. NTSC-1953 transmits wideband I above
~0.6 MHz lower-sideband-only; this mode Hilbert-transforms the resulting
crosstalk off the Q channel and back onto I, restoring the ~0.6 to 1.3 MHz I
detail to full amplitude. That is the closest match to the NTSC-1953 signal
definition, and the digital realization of the wideband recovery EG 27 §5.9
attributes to early NTSC receivers (lower-sideband recovery summed with the
quadrature-demodulated lows). The Hilbert filter rejects ≥36 dB through Q's
entire spec allocation (≤0.6 MHz), so real Q content does not paint onto I; both
I and Q are also equalized against the decoder's known chroma-prefilter droop,
restoring the band-edge response the other modes re-attenuate (~1.5 dB at
1.0 MHz). Only useful on sources that actually carry wideband I; on equiband
material it adds I-channel noise from whatever real energy sits in the 0.6 to
1.3 MHz region of Q.

`wideband_i_ssb` operates on the burst-locked I/Q channels, so it implies
`CHD_OPT_PHASE_COMPENSATION=1` unless the caller explicitly sets it to `0`, in
which case the demodulated planes stay on the U/V grid and it degrades to the
symmetric `equiband` response. Its geometry is fixed by the built-in NTSC-1953
reconstruction filters (I to 1.3 MHz, Q to 0.6 MHz, the SSB Hilbert recovering the
I detail between Q's 0.6 MHz limit and 1.3 MHz), so it takes no numeric +X cutoff
(it rejects one). To refine the vestigial-rolloff shape for a particular source,
attach a measured profile via
[`chd_decoder_set_chroma_sideband_calib`](#chd_decoder_set_chroma_sideband_calib). To find out whether a
source actually carries lower-sideband wideband I before choosing it, measure it
with [`chd_chroma_sideband_calibrate`](#chd_chroma_sideband_calibrate).

#### PAL recovery: `equiband_vsb` { #chroma-filter-equiband-vsb }

PAL transmits both U and V equiband (DSB to ~1.3 MHz), then the channel clips
the **upper** sideband at the video-band edge, leaving the band from +X up to
~1.3 MHz lower-sideband-only (recovered at half amplitude by synchronous
demodulation). Unlike NTSC, PAL's line-alternating V already cancels the U/V
quadrature crosstalk, so recovery reduces to a pure **amplitude EQ**: unity below
+X, ramping to +6 dB (×2) at the 1.3 MHz ceiling, restoring the vestige to full
amplitude. The bulk chroma (below +X) is untouched.

`equiband_vsb` **requires** `CHD_OPT_CHROMA_UPPER_SIDEBAND_HZ` (there is no blind
PAL β estimator and no single baked default); commit rejects it otherwise. +X is
the upper-sideband room above the subcarrier, in Hz, from the source's known
geometry:

| System(s)                       | fSC (MHz) | +X (Hz)  |
|---------------------------------|-----------|----------|
| I/PAL                           | 4.4336    | 1066000  |
| B, B1, D, D1, G, H, K / PAL     | 4.4336    | 570000   |
| M/PAL                           | 3.5756    | 600000   |
| N/PAL (Argentina, 3.582 fSC)    | 3.582     | 620000   |

Use it on clean sources (LaserDisc, studio); it lifts vestige noise as well as
signal, so a noisy off-air/tape capture is better served by `color_under`
(discard the vestige). +X must be in (0, 1.3 MHz), and is only meaningful with
`equiband_vsb`; supplying it with any other mode is rejected at commit.

### chd_chroma_sideband_calibrate

```c
typedef struct chd_chroma_sideband_calib {
    double  beta_plateau;       /* 0 = symmetric/DSB source, 1 = full lower-sideband */
    double  edge_center_hz;
    double  edge_width_hz;
    double  coherence;          /* mean I/Q coherence over 0.70-1.00 MHz, 0..1 */
    double  fit_rms;            /* weighted residual of the model fit */
    int64_t lines_accumulated;
    int32_t is_wideband_i;      /* 1 = classified as carrying lower-sideband wideband I */
    int32_t _pad;
    void *reserved[4];
} chd_chroma_sideband_calib_t;

chd_status_t chd_chroma_sideband_calibrate(chd_video_t *v,
                                    int64_t first_frame, int64_t num_frames,
                                    chd_chroma_sideband_calib_t *out);
```

Measure the source's chroma sideband asymmetry, a provenance diagnostic for
NTSC-1953-era material. NTSC-1953 transmitted wideband I above ~0.6 MHz
lower-sideband-only; whether a given capture preserved that (and the exact
shape of the channel's vestigial rolloff) depends on its provenance chain
(studio-direct vs off-air, transmitter and tuner filtering, dub generation),
not on the standard. This pass demodulates frames `[first_frame, first_frame +
num_frames)` (`num_frames <= 0` = through the last frame) with a burst-locked
2D comb, accumulates the quadrature cross-spectrum between the demodulated I
and Q planes over every active line, and fits the per-frequency
sideband-asymmetry profile `beta(f) = (b−a)/(a+b)` (`a`/`b` the upper/lower
sideband gains) as a raised-cosine ramp from 0 to `beta_plateau` across
`edge_center_hz ± edge_width_hz/2`.

The estimator reads the *imaginary* part of the I·Q cross-spectrum:
lower-sideband crosstalk is a Hilbert (90°) relationship, while picture-driven
I/Q correlation and burst-phase error are in-phase, so the dominant biases are
rejected structurally. `coherence` is the magnitude-squared I/Q coherence over
0.70–1.00 MHz, where real Q is zero by construction and a wideband-I source
must show a near-deterministic relationship; it acts as the confidence gate
behind `is_wideband_i` (coherence ≥ 0.5 and plateau ≥ 0.25). Expect
`is_wideband_i = 0` with plateau ≈ 0 on equiband material (LaserDisc, modern
encoders) and on studio-direct tape that never passed a channel edge.

This is pure measurement: no decoder state is created or modified. Returns
`CHD_E_DECODER_INCOMPATIBLE` for non-NTSC or non-4fSC sources and
`CHD_E_OUT_OF_RANGE` for a bad frame range. Cost is one 1024-point FFT per
line, single-threaded, far cheaper than decoding the same range. Not safe
concurrently with decoding the same `chd_video_t`. To have the decode consume
the result, attach it with
[`chd_decoder_set_chroma_sideband_calib`](#chd_decoder_set_chroma_sideband_calib).

### chd_decoder_set_chroma_sideband_calib

```c
chd_status_t chd_decoder_set_chroma_sideband_calib(chd_decoder_t *d,
                                      const chd_chroma_sideband_calib_t *calib);
```

Attach a β profile to a decoder before [`chd_decoder_commit`](#chd_decoder_commit).
With `chroma_filter="wideband_i_ssb"`, commit synthesizes a pair of
correction filters from the profile and the reconstruction applies them:

- **Transition-strip I equalization**: direct-path gain `1 + β(f)·(1 − W(f))`
  (`W` the built-in Hilbert skirt), restoring full-amplitude I through the
  vestigial 0.4–0.7 MHz transition where the upper sideband is partially
  attenuated. The gain blends exactly into the skirt's vestige-blind region
  (→ 1 where the skirt is fully on) and is identity at β = 0.
- **Q crosstalk nulling**: subtracts `β·ĥ(I)` (Hilbert of the clean,
  q-free I observation, scaled by the profile) from the Q plane ahead of its
  low-pass, removing the hue-transient ghost that asymmetric-sideband I detail
  paints onto Q, the artifact period receivers displayed.

The natural source of the profile is [`chd_chroma_sideband_calibrate`](#chd_chroma_sideband_calibrate)
on the same capture; a caller-constructed preset is also accepted (an explicit
decision: never apply a nominal preset to material of unknown provenance, as
assuming a vestige on a symmetric-sideband source *creates* artifacts).

Fallback semantics: the profile's classification gate is honoured. A NULL
`calib` clears; `is_wideband_i == 0` or `beta_plateau == 0` is accepted but
inert (β ≡ 0), producing output bit-identical to having no profile, so
"didn't calibrate", "calibrated a DSB source", and "estimate wasn't trusted"
all converge on the same decode. An *active* profile combined with any
`chroma_filter` other than `"wideband_i_ssb"` is rejected at commit.
`beta_plateau` must be in `[0, 1]`; an active profile additionally requires
`edge_center_hz` in `(0, 1.6 MHz]` and `edge_width_hz` in `(0, 1.2 MHz]`.

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

### 4:4:0 output

The `yuv440p16` / `yuv440ps` output formats carry line-sequential (SECAM)
chroma honestly: the Cb and Cr planes are full width but hold only the rows
that were really decoded, one plane row per decoded line, with no vertical
interpolation. SECAM transmits one colour-difference component per line, and
because the second field of a 625-line frame sits an odd line count after the
first, the components pair up in interlaced frame-row order
(`Db, Dr, Dr, Db, Db, ...`). Consequences:

- The two chroma planes' heights differ by at most one and together cover
  every active row.
- Chroma plane row `k` is the `k`-th row of that component in frame order;
  its luma row is **not** a fixed-step lattice. Use
  [`chd_frame_chroma_row_component`](#chd_frame_chroma_row_component) to map
  frame rows to components (and therefore plane rows to frame rows), and
  [`chd_frame_get_plane_info`](#chd_frame_get_plane_info) for each plane's
  height and first frame row.
- The mapping is per-frame, not per-format: a given frame row's component
  flips frame to frame (the 625-line count is odd, giving the four-field
  ident cycle of Rec. ITU-R BR.469).

SECAM sources decode only to these formats or to luma-only `gray16`/`grays`;
`chd_decoder_commit` rejects full-height chroma and RGB formats because any
line-repeat or resample decision belongs to the consuming application.
`padding_multiple` > 1 and `output_y4m_headers` are likewise rejected for
4:4:0 output.

### chd_frame_get_plane_info

```c
chd_status_t chd_frame_get_plane_info(const chd_frame_t *f, chd_plane_t p,
                                      chd_plane_info_t *out);
```

Fill `*out` with plane `p`'s [geometry](#chd_plane_info_t). Valid for every
pixel format, so consumers can size per-plane buffers unconditionally; the
4:4:0 formats are the reason to call it.

### chd_frame_chroma_row_component

```c
chd_status_t chd_frame_chroma_row_component(const chd_frame_t *f, int32_t frame_row,
                                            chd_chroma_row_component_t *out);
```

Report which colour-difference component the chroma decoded at output frame
row `frame_row` carries (`CHD_CHROMA_ROW_DB` or `CHD_CHROMA_ROW_DR`).
Line-sequential (4:4:0) frames only: returns `CHD_E_UNSUPPORTED` for other
frames and `CHD_E_OUT_OF_RANGE` for rows outside the output frame.

### chd_frame_get_chroma_ident

```c
chd_status_t chd_frame_get_chroma_ident(const chd_frame_t *f,
                                        chd_chroma_ident_report_t *out);
```

Fill `*out` with the frame's [Db/Dr ident summary](#chd_chroma_ident_report_t).
Line-sequential (4:4:0) frames only: returns `CHD_E_UNSUPPORTED` otherwise.
Archival consumers can use the confidence fraction to flag suspect colour
framing without touching the generic frame info.

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
(Y/Cb/Cr for `CHD_PIXEL_YUV444P16` and `CHD_PIXEL_YUV440P16`, R/G/B for
`CHD_PIXEL_RGB48`, or a single Y plane for `CHD_PIXEL_GRAY16`). For float
frames use [`chd_frame_get_plane_float`](#chd_frame_get_plane_float).

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
- `CHD_PIXEL_YUV440PS` exposes the same signals as `CHD_PIXEL_YUV444PS` with
  subsampled Cb/Cr planes; see [4:4:0 output](#440-output).

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
| `legal_ycbcr_bt601` | Clamp to values that map to ITU-R BT.601-7 §2.5.3 video-allowed codes `[1.00d, 254.75d]` scaled to output bit-depth. Useful before quantizing for downstream SD-SDI or DV transport. In RGBS, it clamps components to bounds achievable from BT.601-legal Y′CbCr (`R′∈[-0.863, +1.884]`, `G′∈[-0.667, +1.690]`, `B′∈[-1.073, +2.093]` at the default `modern` colour-difference precision; the box is projected through the selected matrix, so it shifts slightly under `classic`). Doesn't affect integer RGB which is already stricter. |

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

## Colour conversion precision { #colour-conversion-precision }

Getting from a decoder's composite-domain Y/U/V to R′G′B′ or E′Y/E′Cb/E′Cr
takes two independent sets of constants. They come from different standards,
and both have been published at more than one precision, so each gets its own
option. The defaults reproduce `ld-chroma-decoder`, so you only need these if
you are chasing a specific standard's arithmetic.

Careful: the literature spells both families with a K subscripted R and B. They
are not the same constants.

### Colour-difference precision

`CHD_OPT_COLOR_DIFFERENCE_PRECISION` picks the precision of the luma matrix
coefficients, the ones that say what fraction of E′Y each primary carries.

| Token              | Luma equation                             | IEC 23091-2/ITU-T H.273 MatrixCoefficients |
|--------------------|-------------------------------------------|--------------------------------------------|
| `classic`          | `E′Y = 0.30 E′R + 0.59 E′G + 0.11 E′B`    | `4` (NTSC-1953, FCC 47 §73.682)            |
| `modern` (default) | `E′Y = 0.299 E′R + 0.587 E′G + 0.114 E′B` | `5` (625-line), `6` (525-line)             |

`classic` is the NTSC-1953 equation. `modern` from ITU-R BT.470, SMPTE ST 170,
and ITU-R BT.1700.

If you decode with `classic`, tag E′Y/E′Cb/E′Cr and Y′Cb′Cr′ results with
`MatrixCoefficients` code point `4` instead of `5`or `6`.

These coefficients reach the output in two places:

- The **E′Cb / E′Cr** scaling, through the full-excursion colour-difference
  spans `2(1 - KB)` and `2(1 - KR)`. Under `classic` those are `1.78` and
  `1.40` rather than `1.772` and `1.402`.
- The **green** row of the U/V → R′G′B′ matrix.

### Broadcast scaling precision

`CHD_OPT_BROADCAST_SCALING_PRECISION` picks the precision of reduction factors
applied to the two color differences before they modulate the subcarrier,
keeping the composite signal's excursion inside broadcast-safe transmission
range.

```
U = uReduction × (E′B - E′Y)
V = vReduction × (E′R - E′Y)
```

| Token                  | `uReduction`         | `vReduction`         | Source                                                |
|------------------------|----------------------|----------------------|-------------------------------------------------------|
| `classic`              | `0.493`              | `0.877`              | ITU-R BT.470-6 §2.5, ITU-R BT.1700 item 9             |
| `modern`               | `0.492111`           | `0.877283`           | SMPTE ST 170 Annex A.3, eq 4 and 5                    |
| `scientific` (default) | `0.4921110411224836` | `0.8772832199381787` | The closed forms ST 170's trailing ellipses stand for |

`classic` and `modern` are not roundings of the same number. SMPTE ST 170 Annex
A.3 records that the 1953 derivation of the reduction factors used a blue luma
coefficient of `0.115` instead of the correct `0.114`, and `0.493`/`0.877` are
the result. ST 170 redoes the derivation from the correct luma matrix, which is
where `0.492111`/`0.877283` come from. So `classic` is a real difference: about
`0.18%` on `uReduction`. `modern` and `scientific` agree to roughly `1e-7`, far
below a 16-bit quantum, so that pair is a numerical no-op in most cases.

Unlike color-difference precision, these factors reach *every* row of the
U/V → R′G′B′ matrix, plus both chroma scalings.

For SECAM this option is inert by construction. SECAM scaling follows
BT.470/BT.1700 standards:
`D′B = 1.505 (E′B - E′Y)`  
`D′R = -1.902 (E′R - E′Y)`

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

### chd_decoder_get_chroma_click_thresholds

```c
chd_status_t chd_decoder_get_chroma_click_thresholds(const chd_decoder_t *d,
                                                     double *env_dip_db,
                                                     double *freq_overshoot);
```

Return the effective [SECAM click-concealment](#secam-click-concealment)
thresholds applied to the **most recent** decode on this decoder: the
envelope-dip depth in dB and the deviation-overshoot multiple, after the
adaptive formula or the expert overrides. `CHD_E_UNSUPPORTED` before any
decode has run with `chroma_click_nr_level` > 0.

## Dropout detection

The functions above *conceal* dropouts during a decode; these *expose* the
flagged regions without running the chroma decoder, so a consumer can build its
own visualisation (for example a mask clip marking damaged areas). Dropouts are
detected upstream and stored in the source metadata, so these work regardless of
whether concealment is enabled — and pair naturally with a
[`CHD_DEC_NONE`](#chd_decoder_create) decoder to skip chroma decoding entirely.

```c
typedef enum chd_dropout_origin {
    CHD_DROPOUT_ORIGIN_SOURCE_METADATA     = 0,
    CHD_DROPOUT_ORIGIN_DECODER_CONCEALMENT = 8
} chd_dropout_origin_t;

typedef struct chd_dropout_span {
    int32_t y;        /* active-output row */
    int32_t x_start;  /* half-open [x_start, x_end) within the active width */
    int32_t x_end;
    chd_dropout_origin_t origin;
} chd_dropout_span_t;
```

`origin` distinguishes upstream-flagged regions (source metadata) from
samples the decoder itself detected and concealed (SECAM
[click concealment](#secam-click-concealment)). One enumeration path,
filterable by origin; the enum is numeric-gapped per family so future
origins can slot in.

### chd_dropout_detect_mode_t

```c
typedef enum chd_dropout_detect_mode {
    CHD_DROPOUT_DETECTED    = 0,
    CHD_DROPOUT_OVERCORRECT = 1
} chd_dropout_detect_mode_t;
```

Selects which regions the queries below report (mutually exclusive):

| Value | Reports |
|-------|---------|
| `CHD_DROPOUT_DETECTED` | The raw regions flagged in the source metadata. |
| `CHD_DROPOUT_OVERCORRECT` | The detected regions widened by the overcorrect margin (±24 samples, clamped to the active picture) — the footprint that overcorrect-mode concealment would touch. Independent of the configured [dropout options](#chd_decoder_set_dropout); reporting this footprint does not require `overcorrect` to be enabled. |

Both modes read source metadata only — no replacement search, no chroma
decode. Decoder-detected concealment spans are the exception: they exist only
once a frame has been decoded with `chroma_click_nr_level` > 0, after which
both modes include them for that frame.

### chd_decoder_get_dropout_spans

```c
chd_status_t chd_decoder_get_dropout_spans(chd_decoder_t *d, int64_t frame_index,
                                           chd_dropout_detect_mode_t mode,
                                           chd_dropout_span_t **out_spans,
                                           size_t *out_count);
void         chd_dropout_spans_free(chd_dropout_span_t *spans);
```

Return the dropout regions for one frame selected by `mode`, mapped into the
committed [output framing](#chd_output_info_t): each span's `y`, `x_start`, and
`x_end` index the same coordinate space as
[`chd_frame_get_plane`](#chd_frame_get_plane) for a frame from the same committed
decoder (interlace weave, crop, padding, and field order applied; spans clipped
to the active picture, sorted by `y` then `x_start`). On `CHD_OK`, `*out_spans`
is a newly-allocated array of `*out_count` spans the caller releases with
`chd_dropout_spans_free`; a frame with no dropouts yields `*out_count == 0` and
`*out_spans == NULL`. An out-of-range index returns `CHD_E_OUT_OF_RANGE`; an
unknown `mode` returns `CHD_E_INVALID_ARG`. Requires a prior
[`chd_decoder_commit`](#chd_decoder_commit).

### chd_decode_dropout_mask

```c
chd_status_t chd_decode_dropout_mask(chd_decoder_t *d, int64_t frame_index,
                                     chd_dropout_detect_mode_t mode,
                                     chd_frame_t **out);
```

Rasterise the `mode`-selected regions into a single-plane [frame](#frames)
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

### Backends

```c
typedef enum chd_nn_backend {
    CHD_NN_BACKEND_AUTO  = 0,   /* best across all backends; inferred from artifact */

    CHD_NN_ORT_AUTO      = 10,  /* ONNX Runtime, per-OS EP fallback chain */
    CHD_NN_ORT_CPU       = 11,
    CHD_NN_ORT_CUDA      = 12,
    CHD_NN_ORT_TENSORRT  = 13,
    CHD_NN_ORT_COREML    = 14,  /* ONNX Runtime CoreML execution provider */
    CHD_NN_ORT_DIRECTML  = 15,
    CHD_NN_ORT_MIGRAPHX  = 16,

    CHD_NN_COREML        = 20   /* native CoreML .mlpackage via MLModel */
} chd_nn_backend_t;
```

A backend names the runtime that runs inference. The family is encoded in the
name: the `CHD_NN_ORT_*` values select an ONNX Runtime execution provider;
values with no `ORT_` infix (e.g. `CHD_NN_COREML`) are native, non-ORT backends.

- `CHD_NN_BACKEND_AUTO` (the default) picks the best backend for the model
  *artifact*: a `.onnx` loads through ONNX Runtime with the per-OS EP auto chain
  (`CHD_NN_ORT_AUTO`); a `.mlpackage`/`.mlmodelc` loads through `CHD_NN_COREML`.
- `CHD_NN_ORT_AUTO` forces ONNX Runtime and walks its per-OS provider chain
  (Windows: TensorRT → CUDA → DirectML → CPU; Linux: CUDA → MIGraphX → CPU;
  macOS: CoreML → CPU), attaching the first that succeeds.
- A specific `CHD_NN_ORT_*` value pins that one EP (no fallback; load fails with
  `CHD_E_NN_BACKEND_UNAVAILABLE` if it can't attach).
- `CHD_NN_COREML` forces the native CoreML backend (requires a `.mlpackage`).

`CHD_NN_ORT_COREML` (the ORT CoreML EP) and `CHD_NN_COREML` (native) are distinct
and distinguishable after load via
[`chd_nn_model_get_active_backend`](#chd_nn_model_get_active_backend).

### CoreML compute units

```c
typedef enum chd_nn_coreml_compute {
    CHD_NN_COREML_CPU_AND_GPU = 0,  /* default: CPU + GPU, no ANE */
    CHD_NN_COREML_ALL         = 1,  /* CPU + GPU + Apple Neural Engine */
    CHD_NN_COREML_CPU_ONLY    = 2   /* CPU only */
} chd_nn_coreml_compute_t;
```

Applies to the native `CHD_NN_COREML` backend only (ignored by every ORT
backend). The default `CHD_NN_COREML_CPU_AND_GPU` is required for nnTransform3D
(the ANE cannot run its 3D convolution); `CHD_NN_COREML_ALL` lets CoreML use the
ANE for ANE-friendly models.

### Session options

```c
typedef struct chd_nn_session_opts {
    chd_nn_backend_t backend;            /* AUTO unless caller pins */
    int32_t device_id;                   /* 0 unless multi-GPU */
    int     enable_graph_optim;          /* default 1 */
    int     enable_mem_pattern;          /* default 1 */
    int32_t inter_op_threads;            /* default 1 */
    int32_t intra_op_threads;            /* default 1 */
    const char *engine_cache_dir;        /* see below */
    chd_nn_coreml_compute_t coreml_compute; /* native CoreML only */
    void *reserved[4];                   /* zero-initialised; do not repurpose */
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

Fill `*out` with default session options (`CHD_NN_BACKEND_AUTO`, the defaults
noted above, `CHD_NN_COREML_CPU_AND_GPU`, zeroed reserved fields). Always
initialise via this function rather than by hand, so new fields pick up correct
defaults.

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

Load a model from a file on disk (`chd_nn_model_load_from_file`) or from an
in-memory buffer (`chd_nn_model_load_from_memory`), for callers that embed the
model as a compiled-in byte array and want no filesystem dependency. The buffer
is consumed during the call and need not outlive it.

`opts.backend` selects the runtime (see [Backends](#backends)). The default
`CHD_NN_BACKEND_AUTO` infers it from the artifact: a `.onnx` loads through ONNX
Runtime; a `.mlpackage`/`.mlmodelc` loads through the native CoreML backend. A
pinned backend forces that path and requires the matching artifact (a native
backend pinned against a `.onnx` fails to load, and vice versa).

The in-memory loader works with any backend that can ingest a serialized model
buffer. ONNX Runtime can, so the `CHD_NN_ORT_*` backends and
`CHD_NN_BACKEND_AUTO` (which resolves to ONNX Runtime here) all load from
memory. Pinning `CHD_NN_COREML` returns `CHD_E_INVALID_ARG`: a `.mlpackage` is
a multi-file on-disk bundle with no in-memory load API, so use
[`chd_nn_model_load_from_file`](#chd_nn_model_load_from_file) instead. This is
a per-backend limitation.

`opts_or_null` of `NULL` uses [defaults](#chd_nn_session_opts_default). On
`CHD_E_NN_BACKEND_UNAVAILABLE` the pinned backend isn't available in this
build/host (e.g. `CHD_NN_COREML` on a non-Apple build, or one configured with
`-Dwith_coreml=disabled` — detect at runtime with `chd_has_feature("coreml")`);
`CHD_E_NN_MODEL_LOAD` indicates a bad or unreadable model. The native CoreML
`.mlpackage` is produced offline from the ONNX model with `coremltools` (see
`scripts/convert_coreml.py`) and is not shipped with the library. Remember
[`chd_shutdown`](#chd_shutdown) before exit once any model has been loaded.

### chd_nn_model_get_active_backend

```c
chd_status_t chd_nn_model_get_active_backend(const chd_nn_model_t *m,
                                             chd_nn_backend_t *out);
```

Report the backend actually selected for a loaded model. Useful after
`CHD_NN_BACKEND_AUTO` / `CHD_NN_ORT_AUTO` (which resolve to a concrete value),
and to distinguish the native `CHD_NN_COREML` backend from the ORT CoreML EP
(`CHD_NN_ORT_COREML`).

### chd_nn_backend_is_available

```c
int chd_nn_backend_is_available(chd_nn_backend_t b);   /* 1 = yes, 0 = no */
```

Query whether a backend can be used on this build and host without attempting a
model load. The AUTO sentinels report available (they always resolve to at least
CPU); `CHD_NN_COREML` tracks the `coreml` build feature.

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