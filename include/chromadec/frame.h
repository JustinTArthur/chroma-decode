/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CHROMADEC_FRAME_H
#define CHROMADEC_FRAME_H

#include <chromadec/errors.h>
#include <chromadec/types.h>

#ifdef __cplusplus
extern "C" {
#endif

chd_status_t chd_frame_get_info(const chd_frame_t *f, chd_frame_info_t *out);

/* Zero-copy borrow of a 16-bit plane (integer pixel formats). Caller does NOT
 * free the pointer; it is owned by the frame and invalid after chd_frame_free.
 * Stride in bytes. */
chd_status_t chd_frame_get_plane(const chd_frame_t *f, chd_plane_t p,
                                  const void **out_data,
                                  ptrdiff_t *out_stride_bytes);

/* Zero-copy borrow of a float plane, same ownership/lifetime rules as
 * chd_frame_get_plane. Valid for the float pixel formats:
 *   CHD_PIXEL_YUV444PS: planes Y / Cb / Cr expose E'Y (black=0.0, white=1.0)
 *     and E'Cb/E'Cr (centred at 0.0, ±0.5).
 *   CHD_PIXEL_GRAYS: plane Y exposes E'Y only.
 *   CHD_PIXEL_RGBS: planes R / G / B expose E'R, E'G, E'B (black=0.0,
 *     white=1.0). Computed directly from the decoder's component signals; no
 *     intermediate integer quantization.
 * These are the normalized BT.601/H.273 signals that the integer formats
 * quantize. */
chd_status_t chd_frame_get_plane_float(const chd_frame_t *f, chd_plane_t p,
                                        const float **out_data,
                                        ptrdiff_t *out_stride_bytes);

void         chd_frame_free(chd_frame_t *f);

#ifdef __cplusplus
}
#endif
#endif
