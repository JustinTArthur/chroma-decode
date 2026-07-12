/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CHROMADEC_FRAME_H
#define CHROMADEC_FRAME_H

#include <chromadec/errors.h>
#include <chromadec/types.h>

#ifdef __cplusplus
extern "C" {
#endif

chd_status_t chd_frame_get_info(const chd_frame_t *f, chd_frame_info_t *out);

/* Per-plane geometry. Valid for every pixel format; the 4:4:0 formats are
 * the reason to call it (their chroma planes are shorter than the frame and
 * differ from each other by up to one row). */
chd_status_t chd_frame_get_plane_info(const chd_frame_t *f, chd_plane_t p,
                                      chd_plane_info_t *out);

/* Which colour-difference component the chroma decoded at output frame row
 * `frame_row` carries. Line-sequential (4:4:0) frames only:
 * CHD_E_UNSUPPORTED otherwise. Per-frame, not per-format: a given row's
 * component changes frame to frame (the 625-line count is odd). */
chd_status_t chd_frame_chroma_row_component(const chd_frame_t *f, int32_t frame_row,
                                            chd_chroma_row_component_t *out);

/* Per-frame Db/Dr ident summary for line-sequential (4:4:0) frames;
 * CHD_E_UNSUPPORTED otherwise. */
chd_status_t chd_frame_get_chroma_ident(const chd_frame_t *f,
                                        chd_chroma_ident_report_t *out);

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
