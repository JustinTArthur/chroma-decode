/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CHROMADEC_TYPES_H
#define CHROMADEC_TYPES_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct chd_video    chd_video_t;
typedef struct chd_decoder  chd_decoder_t;
typedef struct chd_frame    chd_frame_t;
typedef struct chd_nn_model chd_nn_model_t;
typedef struct chd_cancel   chd_cancel_t;

typedef enum chd_video_standard {
    CHD_STD_UNKNOWN = 0,
    CHD_STD_NTSC,
    CHD_STD_PAL,
    CHD_STD_PAL_M
} chd_video_standard_t;

typedef enum chd_sample_encoding {
    CHD_ENC_UNKNOWN = 0,
    CHD_ENC_CVBS_U10_4FSC,
    CHD_ENC_CVBS_U16_4FSC,
    CHD_ENC_CVBS_TPG21_4FSC,
    CHD_ENC_CVBS_S16_FSC,
    CHD_ENC_RAW_S16_28M,
    CHD_ENC_RAW_S16_40M
} chd_sample_encoding_t;

typedef enum chd_signal_state {
    CHD_SIG_UNKNOWN = 0,
    CHD_SIG_STANDARD_TBC_LOCKED,
    CHD_SIG_STANDARD_TBC_UNLOCKED,
    CHD_SIG_STANDARD_RAW,
    CHD_SIG_NONSTANDARD_TBC_LOCKED,
    CHD_SIG_NONSTANDARD_TBC_UNLOCKED,
    CHD_SIG_NONSTANDARD_RAW
} chd_signal_state_t;

typedef enum chd_frame_layout {
    CHD_FRAME_LAYOUT_UNKNOWN = 0,
    CHD_FRAME_LAYOUT_FIELD_RASTER,
    CHD_FRAME_LAYOUT_FRAME_NATIVE
} chd_frame_layout_t;

typedef enum chd_plane {
    CHD_PLANE_Y  = 0,
    CHD_PLANE_CB = 1,
    CHD_PLANE_CR = 2,
    CHD_PLANE_R  = 3,
    CHD_PLANE_G  = 4,
    CHD_PLANE_B  = 5
} chd_plane_t;

typedef enum chd_pixel_format {
    CHD_PIXEL_YUV444P16 = 0,
    CHD_PIXEL_YUV444PS  = 1,
    CHD_PIXEL_RGB48     = 2,
    CHD_PIXEL_RGBS      = 3,
    CHD_PIXEL_GRAY16    = 4,
    CHD_PIXEL_GRAYS     = 5
} chd_pixel_format_t;

typedef enum chd_clamp {
    CHD_CLAMP_NONE               = 0,
    CHD_CLAMP_LEGAL_RGB_SDR      = 1,
    CHD_CLAMP_LEGAL_RGB_HDR      = 2,
    CHD_CLAMP_LEGAL_YCBCR_BT601  = 3
} chd_clamp_t;

typedef struct chd_video_params {
    chd_video_standard_t  standard;
    chd_sample_encoding_t encoding;
    chd_signal_state_t    signal_state;
    chd_frame_layout_t    layout;
    int      is_subcarrier_locked;
    int      is_second_field_first;
} chd_video_params_t;

typedef struct chd_video_info {
    chd_video_standard_t  standard;
    chd_sample_encoding_t encoding;
    chd_signal_state_t    signal_state;
    chd_frame_layout_t    layout;
    int32_t  field_width;
    int32_t  field_height;
    int32_t  samples_per_frame;
    double   sample_rate_hz;
    double   fsc_hz;
    int32_t  active_video_start;
    int32_t  active_video_end;
    int32_t  first_active_frame_line;
    int32_t  last_active_frame_line;
    int32_t  black_16b_ire;
    int32_t  white_16b_ire;
    int32_t  blanking_16b_ire;
    int64_t  num_frames;
    int      is_widescreen;
    int      is_subcarrier_locked;
    int      is_first_field_first;
} chd_video_info_t;

typedef struct chd_frame_info {
    chd_pixel_format_t format;
    int32_t  width;
    int32_t  height;
    int32_t  num_planes;
    int64_t  frame_index;
} chd_frame_info_t;

/* Committed output framing, reported by chd_decoder_get_output_info. width and
 * height are the active picture region after crop and padding — the same
 * dimensions chd_decode_frame produces and the coordinate space dropout spans
 * are expressed in. */
typedef struct chd_output_info {
    chd_pixel_format_t format;
    int32_t  width;
    int32_t  height;
    int32_t  num_planes;
    int64_t  num_frames;
} chd_output_info_t;

#ifdef __cplusplus
}
#endif
#endif
