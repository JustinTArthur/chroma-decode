// SPDX-License-Identifier: GPL-3.0-or-later
#include <chromadec/nn.h>

#include "../common/error_state.h"

extern "C" {

static chd_status_t not_yet(const char *what) {
    chd::detail::set_last_error(std::string("chromadec: ") + what + " is not implemented yet");
    return CHD_E_INTERNAL;
}

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
}

chd_status_t chd_nn_model_load(const char *, const chd_nn_session_opts_t *,
                                chd_nn_model_t **) {
    return not_yet("chd_nn_model_load");
}

void chd_nn_model_free(chd_nn_model_t *) {}

chd_status_t chd_nn_model_get_active_provider(const chd_nn_model_t *,
                                               chd_nn_provider_t *) {
    return not_yet("chd_nn_model_get_active_provider");
}

int chd_nn_provider_is_available(chd_nn_provider_t) {
    /* No NN providers wired yet. */
    return 0;
}

}  // extern "C"
