# CI workflows

| Workflow | Purpose | Status |
|---|---|---|
| `build.yml` | Linux + macOS skeleton build; verifies `chd_*` symbol surface and that no internal symbols leak | Active |
| Windows | MSVC build, ORT 1.26.0 from Microsoft release tarball | Not yet — follow-up |
| ABI check | `abi-compliance-checker` against previous release `.so` | Not yet — added at first tagged release |
| ASan / UBSan / TSan | Sanitizer matrix on Linux | Not yet |
| Encode-orc fixture generator | Clone, build, and run encode-orc; produce test `.tbc` + `.tbc.db` fixtures for the integration test job | Not yet |
| CUDA self-hosted | nnTransform3D CUDA FFT path validation | Not yet |

ONNX Runtime is downloaded from Microsoft's official GitHub releases on every
platform. macOS releases include the CoreML
execution provider; do not use the Homebrew `onnxruntime` formula, which
historically has shipped without CoreML.

All Python tooling in CI runs inside a fresh per-job virtual environment.
