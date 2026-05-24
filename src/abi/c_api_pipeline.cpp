// SPDX-License-Identifier: GPL-3.0-or-later
#include <chromadec/pipeline.h>

#include "../common/error_state.h"

extern "C" {

static chd_status_t not_yet(const char *what) {
    chd::detail::set_last_error(std::string("chromadec: ") + what + " is not implemented yet");
    return CHD_E_INTERNAL;
}

chd_status_t chd_cancel_create(chd_cancel_t **) {
    return not_yet("chd_cancel_create");
}

void chd_cancel_request(chd_cancel_t *) {}

int chd_cancel_is_requested(const chd_cancel_t *) {
    return 0;
}

void chd_cancel_free(chd_cancel_t *) {}

}  // extern "C"
