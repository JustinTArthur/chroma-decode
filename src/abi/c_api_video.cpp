// SPDX-License-Identifier: GPL-3.0-or-later
#include <chromadec/video.h>

#include "../common/error_state.h"

extern "C" {

static chd_status_t not_yet(const char *what) {
    chd::detail::set_last_error(std::string("chromadec: ") + what + " is not implemented yet");
    return CHD_E_INTERNAL;
}

chd_status_t chd_video_open_composite(const char *, const char *,
                                      const chd_video_params_t *, chd_video_t **) {
    return not_yet("chd_video_open_composite");
}

chd_status_t chd_video_open_yc(const char *, const char *, const char *,
                               const chd_video_params_t *,
                               chd_video_t **) {
    return not_yet("chd_video_open_yc");
}

void chd_video_free(chd_video_t *) {}

chd_status_t chd_video_get_info(const chd_video_t *, chd_video_info_t *) {
    return not_yet("chd_video_get_info");
}

chd_status_t chd_video_add_extra_source_composite(chd_video_t *, const char *,
                                                  const char *) {
    return not_yet("chd_video_add_extra_source_composite");
}

chd_status_t chd_video_add_extra_source_yc(chd_video_t *, const char *, const char *, const char *) {
    return not_yet("chd_video_add_extra_source_yc");
}

}  // extern "C"
