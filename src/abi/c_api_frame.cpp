// SPDX-License-Identifier: GPL-3.0-or-later
#include <chromadec/frame.h>

#include "../common/error_state.h"

extern "C" {

static chd_status_t not_yet(const char *what) {
    chd::detail::set_last_error(std::string("chromadec: ") + what + " is not implemented yet");
    return CHD_E_INTERNAL;
}

chd_status_t chd_frame_get_info(const chd_frame_t *, chd_frame_info_t *) {
    return not_yet("chd_frame_get_info");
}

chd_status_t chd_frame_get_plane(const chd_frame_t *, chd_plane_t,
                                  const void **, ptrdiff_t *) {
    return not_yet("chd_frame_get_plane");
}

chd_status_t chd_frame_get_plane_float(const chd_frame_t *, chd_plane_t,
                                        const float **, ptrdiff_t *) {
    return not_yet("chd_frame_get_plane_float");
}

chd_status_t chd_frame_copy_plane_float(const chd_frame_t *, chd_plane_t,
                                         float *, ptrdiff_t) {
    return not_yet("chd_frame_copy_plane_float");
}

void chd_frame_free(chd_frame_t *) {}

}  // extern "C"
