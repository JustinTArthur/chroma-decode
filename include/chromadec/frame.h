/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CHROMADEC_FRAME_H
#define CHROMADEC_FRAME_H

#include <chromadec/errors.h>
#include <chromadec/types.h>

#ifdef __cplusplus
extern "C" {
#endif

chd_status_t chd_frame_get_info(const chd_frame_t *f, chd_frame_info_t *out);

/* Read-only plane access. Caller does NOT free the pointer. Stride in bytes. */
chd_status_t chd_frame_get_plane(const chd_frame_t *f, chd_plane_t p,
                                  const void **out_data,
                                  ptrdiff_t *out_stride_bytes);

/* Zero-copy float planes from CHD_PIXEL_YUV444_FLOAT frames. */
chd_status_t chd_frame_get_plane_float(const chd_frame_t *f, chd_plane_t p,
                                        const float **out_data,
                                        ptrdiff_t *out_stride_bytes);

/* Convert any pixel format to float planes mapping black=0.0 white=1.0 for Y,
 * chroma centred at 0.0 with ±0.5 range. Caller-supplied dst buffer. */
chd_status_t chd_frame_copy_plane_float(const chd_frame_t *f, chd_plane_t p,
                                         float *dst, ptrdiff_t dst_stride_bytes);

void         chd_frame_free(chd_frame_t *f);

#ifdef __cplusplus
}
#endif
#endif
