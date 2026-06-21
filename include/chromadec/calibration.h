/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CHROMADEC_CALIBRATION_H
#define CHROMADEC_CALIBRATION_H

#include <stdint.h>

#include <chromadec/errors.h>
#include <chromadec/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Measured NTSC-1953 chroma sideband-asymmetry profile. beta(f) ramps 0 →
 * beta_plateau (raised-cosine) across edge_center_hz ± edge_width_hz/2. */
typedef struct chd_chroma_sideband_calib {
    double  beta_plateau;       /* 0 = symmetric/DSB source, 1 = full lower-sideband */
    double  edge_center_hz;
    double  edge_width_hz;
    double  coherence;          /* mean I/Q coherence over 0.70-1.00 MHz, 0..1 */
    double  fit_rms;            /* weighted residual of the model fit */
    int64_t lines_accumulated;
    int32_t is_wideband_i;      /* 1 = classified as carrying lower-sideband wideband I */
    int32_t _pad;

    /* Reserved for future ABI extensions; zero-initialise the struct (e.g.
     * `chd_chroma_sideband_calib_t c = {0}`). See docs/abi-stability.md. */
    void *reserved[4];
} chd_chroma_sideband_calib_t;

/* Measure the source's chroma sideband asymmetry over frames [first_frame,
 * first_frame + num_frames) (num_frames <= 0 means through the last frame).
 * NTSC 4fSC sources only. Pure measurement: no decoder state is created or
 * modified. Not safe concurrently with decoding the same video. */
chd_status_t chd_chroma_sideband_calibrate(chd_video_t *v,
                                    int64_t first_frame, int64_t num_frames,
                                    chd_chroma_sideband_calib_t *out);

/* Attach a measured (or preset) profile to a decoder; with
 * chroma_filter="wideband_i_ssb" the reconstruction then corrects the
 * vestigial transition band (I equalization + Q crosstalk nulling). An
 * unclassified profile (is_wideband_i == 0) or beta_plateau == 0 is inert;
 * NULL clears. Call before chd_decoder_commit. */
chd_status_t chd_decoder_set_chroma_sideband_calib(chd_decoder_t *d,
                                      const chd_chroma_sideband_calib_t *calib);

#ifdef __cplusplus
}
#endif
#endif
