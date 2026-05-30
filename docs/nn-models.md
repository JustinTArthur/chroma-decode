# Neural-network models

The neural decoder kinds load an external **ONNX** model at runtime. libchromadec
ships no weights of its own; you supply a model file and attach it to the
decoder. NN support exists only when the library was built with it
(`chd_has_feature("nn")` returns 1).

Two model families are supported, both originating as NTSC composite-video
research projects:

- **nnTransform3D** (the `chroma_net` model), by asdfqazsnbb.
- **ldzeug2** (color-CNN and luma-separation models), by jsaowji.

## Applicability: NTSC only

!!! warning "These models are for 525-line NTSC, not PAL"
    Every model below was trained and designed for **525-line NTSC**
    (NTSC-1953 / SMPTE 170M and close relatives). They are **not** applicable to
    PAL, and that includes 525-line **PAL-M**. They exist precisely because NTSC
    never had a frequency-domain ("transform") decoder the way PAL has Transform
    PAL; the neural decoders fill that gap for NTSC.

Because of this, [`CHD_DEC_AUTO`](api-reference.md#chd_decoder_create) never
resolves to a neural decoder (it picks `CHD_DEC_NTSC_2D` / `CHD_DEC_PAL_2D` by
standard). You must select a neural kind explicitly, and only for NTSC content.

## Loading and attaching a model

Load with [`chd_nn_model_load`](api-reference.md#chd_nn_model_load), then attach
to a neural decoder with
[`chd_decoder_set_nn_model`](api-reference.md#chd_decoder_set_nn_model) before
`commit`. The model is borrowed, so keep it alive for the decoder's lifetime,
and call [`chd_shutdown`](api-reference.md#chd_shutdown) once before exit. See
the [integration guide](integration-guide.md#neural-decoders) for the full
sequence.

### Execution providers

The provider is chosen via
[`chd_nn_session_opts_t`](api-reference.md#session-options). With
`CHD_NN_EP_AUTO` the library tries a platform-specific chain and uses the first
that loads:

| Platform | Auto chain |
|---|---|
| Windows | TensorRT, CUDA, DirectML, CPU |
| macOS | CoreML, CPU |
| Linux / other | CUDA, MIGraphX, CPU |

Pinning a specific provider tries only that one and returns
`CHD_E_NN_PROVIDER_UNAVAILABLE` if it is not present. Probe ahead of time with
[`chd_nn_provider_is_available`](api-reference.md#chd_nn_provider_is_available),
and read the provider actually chosen with
[`chd_nn_model_get_active_provider`](api-reference.md#chd_nn_model_get_active_provider).

!!! note "CoreML covers only part of the graph"
    On macOS, the 3D-convolution operations in `chroma_net` are not all
    supported by the CoreML EP; those nodes fall back to CPU regardless of the
    provider chain. The model still runs correctly, just without full CoreML
    acceleration.

For TensorRT and MIGraphX, set `engine_cache_dir` to avoid recompiling the
engine on every load (the first compile costs 15-30 s). See
[session options](api-reference.md#session-options).

## nnTransform3D (`chroma_net`)

**Decoder kind:** `CHD_DEC_NN_TRANSFORM3D`. **Model file:** `chroma_net.onnx`.

In the designer's words, nnTransform3D "uses a neural network to process the
amplitude spectrum after performing a 3D FFT (16x16x4) for the Y/C separation."
It replaces the classic NTSC-3D decoder: each 16×16×4 block of the composite
signal is transformed, and a small CNN predicts a separation mask in the
frequency domain.

### Tensor interface

The ONNX graph (opset 11; `Conv` / `LeakyRelu` / `Add` / `Sigmoid`) is:

| | Shape | Dtype | Meaning |
|---|---|---|---|
| input | `[N, 2, 4, 16, 16]` | float32 | Per-block amplitude spectrum; channel 0 is the spectrum, channel 1 a reference (point-reflected) copy. `N` = number of blocks (batched). |
| output | `[N, 1, 4, 16, 16]` | float32 | Single-channel separation mask (sigmoid, 0..1). |

### Model generation and the magnitude scale

What matters when you load a `chroma_net` model is its **generation**, which
fixes the input magnitude scale:

| Generation | Compute precision | `CHD_OPT_NN_INPUT_MAGNITUDE_SCALE` |
|---|---|---|
| pre-v2 (the original "v1" line) | FP32 | `1.0` |
| v2 | FP16 | `128.0` |

The [`CHD_OPT_NN_INPUT_MAGNITUDE_SCALE`](api-reference.md#option-registry) option
exists specifically to support the v2 FP16 flow. Per the author: "Since the v2
model uses FP16 computation, you need to divide the amplitude spectrum by 128 at
the input to prevent overflow." Set `1.0` for any pre-v2 model and `128.0` for
v2; a mismatched scale produces garbage.

All generations share the same ONNX interface and architecture, and the weights
are stored as FP32 either way (the v2 "FP16" is a runtime-compute choice, not the
stored precision), so a model file does **not** announce its generation. Match
the scale to the release the weights came from. More than one pre-v2 weight
revision exists (the original release plus at least one later retraining), but
they all use scale `1.0`; only v2 changed the regime.

### Provenance and training

nnTransform3D was trained on 87 interlaced NTSC videos (10 frames each),
software-encoded with `ld-chroma-encoder`, deliberately chosen for heavy motion
and high-frequency detail, with no film source material. The canonical sources
of truth for the model are the author's own harnesses and notes (the v1
modified-ld-chroma-decoder harness, the v2 standalone CUDA harness `main.cu`,
and the author's release quotes); downstream ports such as tbc-tools infer their
behaviour from those same sources, as does this library.

## ldzeug2 models

The ldzeug2 project targets composite video from NTSC(-J) laserdiscs. Its models
come in two groups; the canonical source of truth is the ldzeug2 project itself.

### Color CNN

**Decoder kind:** `CHD_DEC_LDZEUG_COLOR_CNN`. A full colour-decoding network.

| | Shape | Dtype | Meaning |
|---|---|---|---|
| input | `[N, 3, H, W]` | float32 | Three per-field planes: the composite samples, plus synthesized I-carrier (cosine) and Q-carrier (sine) reference planes keyed to the field's subcarrier phase. |
| output | `[N, 3, H, W]` | float32 | Decoded colour. |

Published variants include `color_cnn` v1 and v2 and a denoising variant.

### Luma separation

**Decoder kinds:** `CHD_DEC_LDZEUG_LUMA_SEP` (per field) and
`CHD_DEC_LDZEUG_LUMA_SEP_FRAME` (per frame). A grayscale Y/C separator.

| | Shape | Dtype | Meaning |
|---|---|---|---|
| input | `[N, 1, H, W]` | float32 | Single grayscale (composite) plane. |
| output | `[N, 1, H, W]` | float32 | Separated luma. |

The per-field model is, in the project's own assessment, "about as good as 3D"
with no ghosting on fast motion but less detail on stationary areas; the
per-frame model is the alternative. The
[`CHD_OPT_NN_CHROMA_BANDPASS`](api-reference.md#option-registry) option applies
to the luma-separation kinds.

## Model-to-decoder summary

| Decoder kind | Model | Input → output | Magnitude scale |
|---|---|---|---|
| `CHD_DEC_NN_TRANSFORM3D` | `chroma_net` (v1 or v2) | `[N,2,4,16,16]` → `[N,1,4,16,16]` | `1.0` (v1) / `128.0` (v2) |
| `CHD_DEC_LDZEUG_COLOR_CNN` | ldzeug2 `color_cnn` | `[N,3,H,W]` → `[N,3,H,W]` | n/a |
| `CHD_DEC_LDZEUG_LUMA_SEP` | ldzeug2 `luma_sep` (field) | `[N,1,H,W]` → `[N,1,H,W]` | n/a |
| `CHD_DEC_LDZEUG_LUMA_SEP_FRAME` | ldzeug2 `luma_sep` (frame) | `[N,1,H,W]` → `[N,1,H,W]` | n/a |

## Obtaining the models

Model weights are distributed by their upstream projects (nnTransform3D by
asdfqazsnbb; ldzeug2 releases by jsaowji), not bundled with libchromadec. Both
sets are released by their authors without usage restrictions and are generally
treated as public domain; confirm the upstream terms for your own use.

<!-- Specific download locations and canonical filenames to be added. -->