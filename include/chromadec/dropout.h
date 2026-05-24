/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CHROMADEC_DROPOUT_H
#define CHROMADEC_DROPOUT_H

#include <chromadec/errors.h>
#include <chromadec/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct chd_dropout_opts {
    int enabled;
    int overcorrect;        /* extend dropout boundaries by ±24 samples */
    int intra_field_only;   /* skip cross-field replacement candidates */
    /* Extra sources are added separately via chd_video_add_extra_source_*. */
} chd_dropout_opts_t;

typedef struct chd_dropout_stats {
    int32_t corrected;
    int32_t failed;
    int64_t total_distance;
} chd_dropout_stats_t;

chd_status_t chd_decoder_set_dropout(chd_decoder_t *d, const chd_dropout_opts_t *opts);

/* Stats from the most recent chd_decode_frame call on this decoder. */
chd_status_t chd_decoder_get_last_dropout_stats(const chd_decoder_t *d,
                                                 chd_dropout_stats_t *out);

#ifdef __cplusplus
}
#endif
#endif
