// SPDX-License-Identifier: GPL-3.0-or-later
//
// RAII wrapper around one Ort::Session — the internal payload of a
// chd_nn_model_t handle.
//
// Ort::Session is thread-safe; multiple worker threads call
// session->Run(...) concurrently with their own per-call input/output
// tensors. The session is built once and shared across the decoder pool;
// switching models means loading a new chd_nn_model_t and calling
// chd_decoder_set_nn_model again — the old session goes away by RAII once
// all decoders release their handles.

#ifndef CHD_NN_ORT_SESSION_H
#define CHD_NN_ORT_SESSION_H

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <onnxruntime_cxx_api.h>

#include <chromadec/nn.h>

#include "provider_select.h"

namespace chd::nn {

// Options the C ABI gives us when loading a model. Mirrors
// chd_nn_session_opts_t but in C++ form, with defaults already applied.
struct SessionOptions {
    chd_nn_provider_t requestedProvider = CHD_NN_EP_AUTO;
    int32_t           deviceId          = 0;
    bool              enableGraphOptim  = true;
    bool              enableMemPattern  = true;
    int32_t           interOpThreads    = 0;   // 0 = ORT default
    int32_t           intraOpThreads    = 1;   // 1 = avoid oversubscription
    // Cache directory for compiled EP engines. nullopt = caller passed
    // NULL on the C ABI -> auto-pick a per-user cache dir. Empty string
    // = caller passed "" -> disable caching entirely.
    std::optional<std::string> engineCacheDir;
};

class OrtSession {
public:
    // Load an ONNX model from `modelPath`, attaching execution providers
    // per `opts.requestedProvider` (using the auto chain when AUTO).
    // Throws std::runtime_error on failure with a detailed message.
    OrtSession(const std::string &modelPath, const SessionOptions &opts);

    OrtSession(const OrtSession &) = delete;
    OrtSession &operator=(const OrtSession &) = delete;

    // Access the underlying Ort::Session for inference. Thread-safe to
    // call Run() concurrently per ORT docs.
    Ort::Session &session() { return *session_; }

    // The provider ORT actually ended up using (after fallback). May be
    // CPU even when the caller requested CUDA, if AUTO was selected and
    // CUDA wasn't available.
    chd_nn_provider_t activeProvider() const { return activeProvider_; }

    // Best-effort accessor for input/output metadata. Empty until lazily
    // populated on first call.
    const std::vector<std::string> &inputNames();
    const std::vector<std::string> &outputNames();

private:
    std::unique_ptr<Ort::Session> session_;
    chd_nn_provider_t             activeProvider_ = CHD_NN_EP_CPU;
    std::vector<std::string>      inputNames_;
    std::vector<std::string>      outputNames_;
    bool                          ioNamesCached_ = false;
};

}  // namespace chd::nn

#endif  // CHD_NN_ORT_SESSION_H
