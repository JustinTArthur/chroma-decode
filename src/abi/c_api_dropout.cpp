// SPDX-License-Identifier: GPL-3.0-or-later
#include <chromadec/dropout.h>

#include "../common/error_state.h"

extern "C" {

static chd_status_t not_yet(const char *what) {
    chd::detail::set_last_error(std::string("chromadec: ") + what + " is not implemented yet");
    return CHD_E_INTERNAL;
}

chd_status_t chd_decoder_set_dropout(chd_decoder_t *, const chd_dropout_opts_t *) {
    return not_yet("chd_decoder_set_dropout");
}

chd_status_t chd_decoder_get_last_dropout_stats(const chd_decoder_t *,
                                                 chd_dropout_stats_t *) {
    return not_yet("chd_decoder_get_last_dropout_stats");
}

}  // extern "C"
