/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CHROMADEC_NN_H
#define CHROMADEC_NN_H

#include <chromadec/errors.h>
#include <chromadec/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum chd_nn_provider {
    CHD_NN_EP_AUTO     = 0,   /* platform default chain */
    CHD_NN_EP_CPU      = 1,
    CHD_NN_EP_CUDA     = 2,
    CHD_NN_EP_TENSORRT = 3,
    CHD_NN_EP_COREML   = 4,
    CHD_NN_EP_DIRECTML = 5,
    CHD_NN_EP_MIGRAPHX = 6
} chd_nn_provider_t;

typedef struct chd_nn_session_opts {
    chd_nn_provider_t provider;     /* AUTO unless caller pins */
    int32_t device_id;              /* 0 unless multi-GPU */
    int     enable_graph_optim;     /* default 1 */
    int     enable_mem_pattern;     /* default 1 */
    /* Default 1 — our DecoderPool already parallelises across frames; setting
     * intra-op > 1 oversubscribes CPU. */
    int32_t inter_op_threads;
    int32_t intra_op_threads;

    /* Optional directory for caching compiled EP engines/binaries (TensorRT
     * engine plans, MIGraphX compiled models). Avoids the 15-30 s graph
     * compile that runs on the first inference call.
     *   NULL  → library auto-picks a per-user cache dir:
     *             Linux  : $XDG_CACHE_HOME/chromadec or $HOME/.cache/chromadec
     *             macOS  : $HOME/Library/Caches/chromadec
     *             Windows: %LOCALAPPDATA%/chromadec
     *           Directory is created if absent. Auto-pick is the default.
     *   ""    → caching disabled (recompile every load, useful for CI /
     *           hermetic tests / cold-start benchmarking).
     *   path  → use this absolute path (created if it doesn't exist).
     * Currently honoured by the TensorRT and MIGraphX EPs; the CUDA EP
     * uses its own internal PTX cache that isn't configurable here. */
    const char *engine_cache_dir;

    /* Reserved for future ABI extensions. Initialised to zero by
     * chd_nn_session_opts_default(); set to zero by callers building the
     * struct manually. Adding fields here keeps consumers source-compatible
     * across minor versions. */
    void *reserved[4];
} chd_nn_session_opts_t;

void chd_nn_session_opts_default(chd_nn_session_opts_t *out);

/* Load an ONNX model from a file on disk. `model_path` is read by the
 * ONNX Runtime; the file only needs to outlive this call. */
chd_status_t chd_nn_model_load_from_file(const char *model_path,
                                         const chd_nn_session_opts_t *opts_or_null,
                                         chd_nn_model_t **out);

/* Load an ONNX model from an in-memory buffer — for callers that embed the
 * model as a compiled-in byte array and want no filesystem dependency.
 * `model_data` points to `model_size` bytes of serialized ONNX. The bytes
 * are consumed during this call and need not outlive it. */
chd_status_t chd_nn_model_load_from_memory(const void *model_data,
                                           size_t model_size,
                                           const chd_nn_session_opts_t *opts_or_null,
                                           chd_nn_model_t **out);

void chd_nn_model_free(chd_nn_model_t *m);

chd_status_t chd_nn_model_get_active_provider(const chd_nn_model_t *m,
                                               chd_nn_provider_t *out);

int chd_nn_provider_is_available(chd_nn_provider_t p);

#ifdef __cplusplus
}
#endif
#endif
