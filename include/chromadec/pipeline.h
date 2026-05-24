/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CHROMADEC_PIPELINE_H
#define CHROMADEC_PIPELINE_H

#include <chromadec/errors.h>
#include <chromadec/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Threading: the only public knob is the `thread_count` decoder option
 * (see decoder.h). Pools are per-chd_decoder_t — N decoders × T threads each
 * can produce N×T total workers in your process. */

/* Cancellation handles. Optional. Pass NULL to async decode for no cancel. */
chd_status_t chd_cancel_create(chd_cancel_t **out);
void         chd_cancel_request(chd_cancel_t *c);
int          chd_cancel_is_requested(const chd_cancel_t *c);
void         chd_cancel_free(chd_cancel_t *c);

#ifdef __cplusplus
}
#endif
#endif
