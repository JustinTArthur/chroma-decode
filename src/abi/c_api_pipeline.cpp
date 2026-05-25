// SPDX-License-Identifier: GPL-3.0-or-later
#include <chromadec/pipeline.h>

#include "../common/error_state.h"
#include "handles.h"

extern "C" {

chd_status_t chd_cancel_create(chd_cancel_t **out) {
    if (out == nullptr) {
        chd::detail::set_last_error("chd_cancel_create: null argument");
        return CHD_E_INVALID_ARG;
    }
    *out = new chd_cancel();
    return CHD_OK;
}

void chd_cancel_request(chd_cancel_t *c) {
    if (c == nullptr) return;
    c->requested.store(true, std::memory_order_release);
}

int chd_cancel_is_requested(const chd_cancel_t *c) {
    if (c == nullptr) return 0;
    return c->requested.load(std::memory_order_acquire) ? 1 : 0;
}

void chd_cancel_free(chd_cancel_t *c) {
    delete c;
}

}  // extern "C"
