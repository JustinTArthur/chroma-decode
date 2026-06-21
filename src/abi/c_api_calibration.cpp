// SPDX-License-Identifier: GPL-3.0-or-later
//
// chd_chroma_sideband_calibrate: the C ABI entry for the NTSC-1953 sideband-
// asymmetry calibration pass. Drives a private 2D comb with phase
// compensation over the requested frame range, feeding the burst-locked
// demodulated planes to a BetaAccumulator instead of reconstructing
// output, then reports the fitted β profile, coherence, and source
// classification. Single-threaded by design — calibration is a cheap
// measurement pass (one 1024-point FFT per line).

#include <chromadec/calibration.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "handles.h"

#include "../common/error_state.h"
#include "../decoders/comb/beta_calibration.h"
#include "../decoders/comb/comb.h"
#include "../decoders/source_field.h"
#include "../output/component_frame.h"

extern "C" chd_status_t chd_chroma_sideband_calibrate(chd_video_t *v,
                                                      int64_t first_frame, int64_t num_frames,
                                                      chd_chroma_sideband_calib_t *out)
{
    if (v == nullptr || out == nullptr) {
        chd::detail::set_last_error("chd_chroma_sideband_calibrate: NULL argument");
        return CHD_E_INVALID_ARG;
    }
    *out = chd_chroma_sideband_calib_t{};

    chd::metadata::LdDecodeMetaData *meta = v->metadata.get();
    if (meta == nullptr || v->source == nullptr) {
        chd::detail::set_last_error("chd_chroma_sideband_calibrate: missing metadata or source");
        return CHD_E_METADATA_MISSING;
    }

    const chd::metadata::LdDecodeMetaData::VideoParameters vp = meta->getVideoParameters();
    if (vp.system != chd::metadata::NTSC) {
        chd::detail::set_last_error("chd_chroma_sideband_calibrate: requires an NTSC source");
        return CHD_E_DECODER_INCOMPATIBLE;
    }
    if (std::fabs((vp.sampleRate / vp.fSC) - 4.0) > 1.0e-6) {
        chd::detail::set_last_error("chd_chroma_sideband_calibrate: requires 4fSC-sampled data");
        return CHD_E_DECODER_INCOMPATIBLE;
    }

    const int64_t totalFrames = meta->getNumberOfFrames();
    if (first_frame < 0 || first_frame >= totalFrames) {
        chd::detail::set_last_error("chd_chroma_sideband_calibrate: first_frame out of range");
        return CHD_E_OUT_OF_RANGE;
    }
    const int64_t count = (num_frames <= 0)
        ? totalFrames - first_frame
        : std::min(num_frames, totalFrames - first_frame);

    chd::decoders::comb::BetaAccumulator accumulator;
    chd::decoders::comb::Comb::Configuration cfg;
    cfg.dimensions = 2;
    cfg.adaptive = false;
    cfg.phaseCompensation = true;
    cfg.betaAccumulator = &accumulator;

    chd::decoders::comb::Comb comb;
    comb.updateConfiguration(vp, cfg);

    constexpr int64_t kBatchFrames = 8;
    for (int64_t done = 0; done < count; done += kBatchFrames) {
        const int32_t batch = static_cast<int32_t>(std::min(kBatchFrames, count - done));

        std::vector<chd::decoders::SourceField> fields;
        int32_t startIndex = 0, endIndex = 0;
        chd::decoders::SourceField::loadFields(
            *v->source, *meta,
            static_cast<int32_t>(first_frame + done) + 1, batch,
            /*lookBehindFrames=*/0, /*lookAheadFrames=*/0,
            fields, startIndex, endIndex);

        std::vector<chd::output::ComponentFrame> componentFrames(batch);
        comb.decodeFrames(fields, startIndex, endIndex, componentFrames);
    }

    const auto fit = accumulator.fit();
    out->beta_plateau      = fit.plateau;
    out->edge_center_hz    = fit.edgeCenterHz;
    out->edge_width_hz     = fit.edgeWidthHz;
    out->coherence         = fit.coherence;
    out->fit_rms           = fit.fitRms;
    out->lines_accumulated = fit.linesAccumulated;
    out->is_wideband_i     = fit.isWidebandI ? 1 : 0;
    return CHD_OK;
}

extern "C" chd_status_t chd_decoder_set_chroma_sideband_calib(
    chd_decoder_t *d, const chd_chroma_sideband_calib_t *calib)
{
    if (d == nullptr) {
        chd::detail::set_last_error("chd_decoder_set_chroma_sideband_calib: NULL decoder");
        return CHD_E_INVALID_ARG;
    }
    if (d->committed) {
        chd::detail::set_last_error("chd_decoder_set_chroma_sideband_calib: decoder already committed");
        return CHD_E_INVALID_ARG;
    }
    if (calib == nullptr) {
        d->optionMaps.sidebandCalib.reset();
        return CHD_OK;
    }
    // Kind gate: the profile rides the NTSC comb decoders only — the chroma
    // sideband asymmetry it encodes is the NTSC-1953 wideband-I signature, which
    // the PAL decoders never consume (their vestige recovery is amplitude EQ).
    if (!chd::decoders::registry::isNtscCombKind(d->kind)) {
        chd::detail::set_last_error(
            "chd_decoder_set_chroma_sideband_calib: profile not valid for this decoder kind");
        return CHD_E_INVALID_ARG;
    }
    if (!std::isfinite(calib->beta_plateau) || calib->beta_plateau < 0.0
        || calib->beta_plateau > 1.0) {
        chd::detail::set_last_error("chd_decoder_set_chroma_sideband_calib: beta_plateau must be in [0, 1]");
        return CHD_E_INVALID_ARG;
    }
    const bool active = calib->is_wideband_i != 0 && calib->beta_plateau > 0.0;
    if (active
        && (!std::isfinite(calib->edge_center_hz) || calib->edge_center_hz <= 0.0
            || calib->edge_center_hz > 1.6e6
            || !std::isfinite(calib->edge_width_hz) || calib->edge_width_hz <= 0.0
            || calib->edge_width_hz > 1.2e6)) {
        chd::detail::set_last_error(
            "chd_decoder_set_chroma_sideband_calib: active profile needs "
            "edge_center_hz in (0, 1.6 MHz] and edge_width_hz in (0, 1.2 MHz]");
        return CHD_E_INVALID_ARG;
    }
    d->optionMaps.sidebandCalib = *calib;
    return CHD_OK;
}
