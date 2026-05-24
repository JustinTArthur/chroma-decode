// SPDX-License-Identifier: GPL-3.0-or-later
#include <chromadec/video.h>

#include <filesystem>
#include <memory>
#include <string>

#include "../common/error_state.h"
#include "../metadata/core.h"
#include "../metadata/ld_metadata_sqlite.h"
#include "../reader/tbc_source.h"
#include "handles.h"

namespace fs = std::filesystem;

namespace {

chd_status_t set_error(const std::string &msg) {
    chd::detail::set_last_error(msg);
    return CHD_E_INVALID_ARG;
}

chd_status_t not_yet(const char *what) {
    chd::detail::set_last_error(std::string("chromadec: ") + what + " is not implemented yet");
    return CHD_E_INTERNAL;
}

chd_video_standard_t toAbiStandard(chd::metadata::VideoSystem system) {
    switch (system) {
        case chd::metadata::PAL:   return CHD_STD_PAL;
        case chd::metadata::NTSC:  return CHD_STD_NTSC;
        case chd::metadata::PAL_M: return CHD_STD_PAL_M;
    }
    return CHD_STD_UNKNOWN;
}

std::string resolveSidecar(const std::string &tbcPath, const char *sidecarOrNull) {
    if (sidecarOrNull != nullptr) return sidecarOrNull;
    // Auto-locate: <tbcPath>.db wins over <tbcPath>.json.
    const std::string dbPath = tbcPath + ".db";
    if (fs::exists(dbPath)) return dbPath;
    return tbcPath + ".json";  // fallback; opening will fail informatively
}

}  // namespace

extern "C" {

chd_status_t chd_video_open_composite(const char *tbc_path,
                                      const char *sidecar_path_or_null,
                                      const chd_video_params_t *,
                                      chd_video_t **out) {
    if (tbc_path == nullptr || out == nullptr) {
        return set_error("chd_video_open_composite: null argument");
    }
    *out = nullptr;

    const std::string tbcPath = tbc_path;
    if (!fs::exists(tbcPath)) {
        return set_error("chd_video_open_composite: TBC file does not exist: " + tbcPath);
    }

    const std::string sidecarPath = resolveSidecar(tbcPath, sidecar_path_or_null);
    if (!fs::exists(sidecarPath)) {
        return set_error("chd_video_open_composite: sidecar file does not exist: " + sidecarPath);
    }

    auto handle = std::make_unique<chd_video>();
    handle->tbcPath = tbcPath;
    handle->metadata = std::make_unique<chd::metadata::LdDecodeMetaData>();

    try {
        if (!handle->metadata->read(sidecarPath)) {
            return set_error("chd_video_open_composite: failed to read sidecar metadata");
        }
    } catch (const std::exception &e) {
        return set_error(std::string("chd_video_open_composite: ") + e.what());
    }

    const auto &vp = handle->metadata->getVideoParameters();
    handle->source = std::make_unique<chd::reader::SourceVideo>();
    if (!handle->source->open(tbcPath, vp.fieldWidth * vp.fieldHeight, vp.fieldWidth)) {
        return set_error("chd_video_open_composite: failed to open TBC sample file");
    }

    *out = handle.release();
    return CHD_OK;
}

chd_status_t chd_video_open_yc(const char *, const char *, const char *,
                               const chd_video_params_t *,
                               chd_video_t **) {
    return not_yet("chd_video_open_yc");
}

void chd_video_free(chd_video_t *v) {
    delete v;
}

chd_status_t chd_video_get_info(const chd_video_t *v, chd_video_info_t *out) {
    if (v == nullptr || out == nullptr || v->metadata == nullptr) {
        return set_error("chd_video_get_info: null argument");
    }
    const auto &vp = const_cast<chd_video_t *>(v)->metadata->getVideoParameters();

    out->standard               = toAbiStandard(vp.system);
    out->encoding               = CHD_ENC_CVBS_U16_4FSC;
    out->signal_state           = vp.isSubcarrierLocked
                                      ? CHD_SIG_STANDARD_TBC_LOCKED
                                      : CHD_SIG_STANDARD_TBC_UNLOCKED;
    out->field_width            = vp.fieldWidth;
    out->field_height           = vp.fieldHeight;
    out->sample_rate_hz         = vp.sampleRate;
    out->fsc_hz                 = vp.fSC;
    out->active_video_start     = vp.activeVideoStart;
    out->active_video_end       = vp.activeVideoEnd;
    out->first_active_frame_line = vp.firstActiveFrameLine;
    out->last_active_frame_line  = vp.lastActiveFrameLine;
    out->black_16b_ire          = vp.black16bIre;
    out->white_16b_ire          = vp.white16bIre;
    out->blanking_16b_ire       = vp.blanking16bIre;
    out->num_frames             = const_cast<chd_video_t *>(v)->metadata->getNumberOfFrames();
    out->is_widescreen          = vp.isWidescreen ? 1 : 0;
    out->is_subcarrier_locked   = vp.isSubcarrierLocked ? 1 : 0;
    out->is_first_field_first   = const_cast<chd_video_t *>(v)->metadata->getIsFirstFieldFirst() ? 1 : 0;
    return CHD_OK;
}

chd_status_t chd_video_add_extra_source_composite(chd_video_t *v, const char *tbc_path,
                                                  const char *) {
    if (v == nullptr || tbc_path == nullptr) {
        return set_error("chd_video_add_extra_source_composite: null argument");
    }
    auto src = std::make_unique<chd::reader::SourceVideo>();
    const auto &vp = v->metadata->getVideoParameters();
    if (!src->open(tbc_path, vp.fieldWidth * vp.fieldHeight, vp.fieldWidth)) {
        return set_error("chd_video_add_extra_source_composite: failed to open extra TBC source");
    }
    v->extraSources.push_back(std::move(src));
    return CHD_OK;
}

chd_status_t chd_video_add_extra_source_yc(chd_video_t *, const char *, const char *, const char *) {
    return not_yet("chd_video_add_extra_source_yc");
}

}  // extern "C"
