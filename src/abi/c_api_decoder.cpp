// SPDX-License-Identifier: GPL-3.0-or-later
#include <chromadec/decoder.h>

#include <memory>

#include "../common/error_state.h"
#include "handles.h"

extern "C" {

static chd_status_t not_yet(const char *what) {
    chd::detail::set_last_error(std::string("chromadec: ") + what + " is not implemented yet");
    return CHD_E_INTERNAL;
}

// chd_decoder_create allocates a minimal handle that holds the
// caller-supplied decoder kind plus the dropout opts + last-frame stats
// surfaces. The actual decoder algorithm selection / configuration /
// per-frame execution still belongs to a follow-up (chd_decoder_commit /
// chd_decode_frame return CHD_E_INTERNAL). This split lets consumers
// already configure dropout options against a stable ABI even while
// they wait for the decode path.
chd_status_t chd_decoder_create(chd_video_t *v, chd_decoder_kind_t kind, chd_decoder_t **out) {
    if (v == nullptr || out == nullptr) {
        chd::detail::set_last_error("chd_decoder_create: null argument");
        return CHD_E_INVALID_ARG;
    }
    auto handle = std::make_unique<chd_decoder>();
    handle->video = v;
    handle->kind  = kind;
    *out = handle.release();
    return CHD_OK;
}

void chd_decoder_free(chd_decoder_t *d) { delete d; }

chd_status_t chd_decoder_set_option_f64(chd_decoder_t *, const char *, double) {
    return not_yet("chd_decoder_set_option_f64");
}

chd_status_t chd_decoder_set_option_i32(chd_decoder_t *, const char *, int32_t) {
    return not_yet("chd_decoder_set_option_i32");
}

chd_status_t chd_decoder_set_option_bool(chd_decoder_t *, const char *, int) {
    return not_yet("chd_decoder_set_option_bool");
}

chd_status_t chd_decoder_set_option_str(chd_decoder_t *, const char *, const char *) {
    return not_yet("chd_decoder_set_option_str");
}

chd_status_t chd_decoder_has_option(const chd_decoder_t *, const char *) {
    return not_yet("chd_decoder_has_option");
}

chd_status_t chd_decoder_set_nn_model(chd_decoder_t *, chd_nn_model_t *) {
    return not_yet("chd_decoder_set_nn_model");
}

chd_status_t chd_decoder_commit(chd_decoder_t *) {
    return not_yet("chd_decoder_commit");
}

chd_status_t chd_decode_frame(chd_decoder_t *, int64_t, chd_frame_t **) {
    return not_yet("chd_decode_frame");
}

chd_status_t chd_decode_frames_async(chd_decoder_t *, const int64_t *, size_t,
                                      chd_frame_done_cb, void *, chd_cancel_t *) {
    return not_yet("chd_decode_frames_async");
}

}  // extern "C"
