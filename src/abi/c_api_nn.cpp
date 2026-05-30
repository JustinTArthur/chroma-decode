// SPDX-License-Identifier: GPL-3.0-or-later
#include <chromadec/nn.h>

#include <cstring>
#include <memory>
#include <string>

#include "../common/error_state.h"

#if defined(CHD_WITH_NN)
#include "../nn/ort_session.h"
#include "../nn/provider_select.h"
#include "handles.h"

namespace {

// Translate the C option struct into the internal C++ form, applying the
// "NULL means defaults" contract. Shared by both model-load entry points.
// Kept outside extern "C" since it returns a C++ type.
chd::nn::SessionOptions map_session_opts(const chd_nn_session_opts_t *opts_or_null) {
    chd::nn::SessionOptions opts;
    if (opts_or_null != nullptr) {
        opts.requestedProvider = opts_or_null->provider;
        opts.deviceId          = opts_or_null->device_id;
        opts.enableGraphOptim  = opts_or_null->enable_graph_optim != 0;
        opts.enableMemPattern  = opts_or_null->enable_mem_pattern != 0;
        opts.interOpThreads    = opts_or_null->inter_op_threads;
        opts.intraOpThreads    = opts_or_null->intra_op_threads;
        if (opts_or_null->engine_cache_dir != nullptr) {
            opts.engineCacheDir = std::string(opts_or_null->engine_cache_dir);
        }
    }
    return opts;
}

}  // namespace
#endif

extern "C" {

void chd_nn_session_opts_default(chd_nn_session_opts_t *out) {
    if (!out) return;
    out->provider = CHD_NN_EP_AUTO;
    out->device_id = 0;
    out->enable_graph_optim = 1;
    out->enable_mem_pattern = 1;
    out->inter_op_threads = 0;
    /* Default 1: the library's DecoderPool already parallelises across frames;
     * intra-op > 1 oversubscribes CPU. */
    out->intra_op_threads = 1;
    /* NULL → auto-pick per-user cache dir; see chromadec/nn.h. */
    out->engine_cache_dir = nullptr;
    for (size_t i = 0; i < sizeof(out->reserved)/sizeof(out->reserved[0]); ++i) {
        out->reserved[i] = nullptr;
    }
}

#if defined(CHD_WITH_NN)

chd_status_t chd_nn_model_load_from_file(const char *model_path,
                                         const chd_nn_session_opts_t *opts_or_null,
                                         chd_nn_model_t **out) {
    if (model_path == nullptr || out == nullptr) {
        chd::detail::set_last_error("chd_nn_model_load_from_file: null argument");
        return CHD_E_INVALID_ARG;
    }
    *out = nullptr;

    chd::nn::SessionOptions opts = map_session_opts(opts_or_null);

    try {
        auto session = std::make_shared<chd::nn::OrtSession>(model_path, opts);
        auto handle = std::make_unique<chd_nn_model>();
        handle->session = std::move(session);
        *out = handle.release();
        return CHD_OK;
    } catch (const std::exception &e) {
        chd::detail::set_last_error(std::string("chd_nn_model_load_from_file: ") + e.what());
        return CHD_E_NN_MODEL_LOAD;
    }
}

chd_status_t chd_nn_model_load_from_memory(const void *model_data,
                                           size_t model_size,
                                           const chd_nn_session_opts_t *opts_or_null,
                                           chd_nn_model_t **out) {
    if (model_data == nullptr || model_size == 0 || out == nullptr) {
        chd::detail::set_last_error("chd_nn_model_load_from_memory: null or empty argument");
        return CHD_E_INVALID_ARG;
    }
    *out = nullptr;

    chd::nn::SessionOptions opts = map_session_opts(opts_or_null);

    try {
        auto session = std::make_shared<chd::nn::OrtSession>(model_data, model_size, opts);
        auto handle = std::make_unique<chd_nn_model>();
        handle->session = std::move(session);
        *out = handle.release();
        return CHD_OK;
    } catch (const std::exception &e) {
        chd::detail::set_last_error(std::string("chd_nn_model_load_from_memory: ") + e.what());
        return CHD_E_NN_MODEL_LOAD;
    }
}

void chd_nn_model_free(chd_nn_model_t *m) {
    delete m;
}

chd_status_t chd_nn_model_get_active_provider(const chd_nn_model_t *m,
                                               chd_nn_provider_t *out) {
    if (m == nullptr || out == nullptr || m->session == nullptr) {
        chd::detail::set_last_error("chd_nn_model_get_active_provider: null argument");
        return CHD_E_INVALID_ARG;
    }
    *out = m->session->activeProvider();
    return CHD_OK;
}

int chd_nn_provider_is_available(chd_nn_provider_t p) {
    return chd::nn::providerIsAvailable(p) ? 1 : 0;
}

#else  /* !CHD_WITH_NN */

static chd_status_t nn_disabled(const char *what) {
    chd::detail::set_last_error(std::string("chromadec: ") + what +
                                " requires a build with with_nn=true");
    return CHD_E_INTERNAL;
}

chd_status_t chd_nn_model_load_from_file(const char *, const chd_nn_session_opts_t *,
                                         chd_nn_model_t **) {
    return nn_disabled("chd_nn_model_load_from_file");
}

chd_status_t chd_nn_model_load_from_memory(const void *, size_t,
                                           const chd_nn_session_opts_t *,
                                           chd_nn_model_t **) {
    return nn_disabled("chd_nn_model_load_from_memory");
}

void chd_nn_model_free(chd_nn_model_t *) {}

chd_status_t chd_nn_model_get_active_provider(const chd_nn_model_t *,
                                               chd_nn_provider_t *) {
    return nn_disabled("chd_nn_model_get_active_provider");
}

int chd_nn_provider_is_available(chd_nn_provider_t) {
    return 0;
}

#endif  /* CHD_WITH_NN */

}  // extern "C"
