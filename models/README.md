# Models

This directory is **intentionally empty in the repo**. libchromadec does not
ship NN model weights — they have separate licensing and are large.

## Where models live in practice

Consumers (e.g. vapoursynth-analog) bundle their own model files and pass
absolute paths to `chd_nn_model_load_from_file(model_path, ...)` at runtime.
Consumers that compile the model into their binary instead can hand the bytes
straight to `chd_nn_model_load_from_memory(data, size, ...)` with no file on
disk.

## Recommended layout for consumers

When packaging this library together with model weights, the convention is:

```
<install-prefix>/share/chromadec/models/
  nntransform3d/
    v1.onnx
    v2.onnx
  ldzeug/
    color_cnn_v1.onnx
    color_cnn_v2.onnx
    luma_sep_v1.onnx
    luma_sep_frame_v1.onnx
```

The library itself does not search for or auto-discover models — the caller
is responsible for resolving the path and passing it explicitly.

## Per-model magnitude scale

Some models expect inputs at a specific magnitude scale. Set with
`chd_decoder_set_option_f64(d, CHD_OPT_NN_INPUT_MAGNITUDE_SCALE, scale)`.

Known values:

| Decoder | Model variant | `nn_input_magnitude_scale` |
|---|---|---|
| nnTransform3D | v1 | 1.0 |
| nnTransform3D | v2 | 128.0 |
| ldzeug2 color_cnn | all | 1.0 |
| ldzeug2 luma_sep | all | 1.0 |

This is a property of the trained weights, not the library.

## Attribution

See [attribution.md](attribution.md) for the original model authors.
