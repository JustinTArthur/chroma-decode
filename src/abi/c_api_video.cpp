// SPDX-License-Identifier: GPL-3.0-or-later
#include <chromadec/video.h>

#include <filesystem>
#include <memory>
#include <string>

#include "../common/error_state.h"
#include "../format/sample_encoding.h"
#include "../format/signal_state.h"
#include "../format/video_standards.h"
#include "../metadata/core.h"
#include "../metadata/cvbs_metadata_sqlite.h"
#include "../metadata/ld_metadata_sqlite.h"
#include "../reader/cvbs_composite_source.h"
#include "../reader/cvbs_yc_source.h"
#include "../reader/source.h"
#include "../reader/tbc_source.h"
#include "handles.h"

namespace fs = std::filesystem;

namespace {

chd_status_t set_error(const std::string &msg) {
    chd::detail::set_last_error(msg);
    return CHD_E_INVALID_ARG;
}

chd_video_standard_t toAbiStandard(chd::metadata::VideoSystem system) {
    switch (system) {
        case chd::metadata::PAL:   return CHD_STD_PAL;
        case chd::metadata::NTSC:  return CHD_STD_NTSC;
        case chd::metadata::PAL_M: return CHD_STD_PAL_M;
    }
    return CHD_STD_UNKNOWN;
}

chd_sample_encoding_t toAbiEncoding(chd::format::SampleEncoding encoding) {
    switch (encoding) {
        case chd::format::SampleEncoding::CVBS_U10_4FSC:   return CHD_ENC_CVBS_U10_4FSC;
        case chd::format::SampleEncoding::CVBS_U16_4FSC:   return CHD_ENC_CVBS_U16_4FSC;
        case chd::format::SampleEncoding::RAW_S16_28M:     return CHD_ENC_RAW_S16_28M;
        case chd::format::SampleEncoding::RAW_S16_40M:     return CHD_ENC_RAW_S16_40M;
        case chd::format::SampleEncoding::CVBS_TPG21_4FSC: return CHD_ENC_CVBS_TPG21_4FSC;
    }
    return CHD_ENC_UNKNOWN;
}

chd_signal_state_t toAbiSignalState(chd::format::SignalState state) {
    switch (state) {
        case chd::format::SignalState::STANDARD_TBC_LOCKED:      return CHD_SIG_STANDARD_TBC_LOCKED;
        case chd::format::SignalState::STANDARD_TBC_UNLOCKED:    return CHD_SIG_STANDARD_TBC_UNLOCKED;
        case chd::format::SignalState::STANDARD_RAW:             return CHD_SIG_STANDARD_RAW;
        case chd::format::SignalState::NONSTANDARD_TBC_LOCKED:   return CHD_SIG_NONSTANDARD_TBC_LOCKED;
        case chd::format::SignalState::NONSTANDARD_TBC_UNLOCKED: return CHD_SIG_NONSTANDARD_TBC_UNLOCKED;
        case chd::format::SignalState::NONSTANDARD_RAW:          return CHD_SIG_NONSTANDARD_RAW;
    }
    return CHD_SIG_UNKNOWN;
}

std::string resolveSidecar(const std::string &tbcPath, const char *sidecarOrNull) {
    if (sidecarOrNull != nullptr) return sidecarOrNull;
    // Auto-locate: <tbcPath>.db wins over <tbcPath>.json.
    const std::string dbPath = tbcPath + ".db";
    if (fs::exists(dbPath)) return dbPath;
    return tbcPath + ".json";  // fallback; opening will fail informatively
}

// Strip extension and append ".meta" for sidecar auto-location. The CVBS
// spec's file naming convention pairs `<basename>.composite` (or
// `<basename>.y` + `<basename>.c`) with `<basename>.meta`.
std::string defaultMetaPath(const std::string &dataPath) {
    const fs::path p(dataPath);
    fs::path stem = p;
    stem.replace_extension("");
    stem += ".meta";
    return stem.string();
}

// Resolution chain for CVBS open functions:
//   metaPathOrNull → auto-located <basename>.meta → override_or_null →
//   CHD_E_METADATA_MISSING.
//
// The first available source wins. The returned preset triple is what the
// CvbsCompositeSource / CvbsYcSource constructor needs; blackLevelOverride
// and other metadata fields are returned alongside for later use.
struct ResolvedCvbsParams {
    const chd::format::VideoStandardPreset *videoStandard;
    chd::format::SampleEncoding             sampleEncoding;
    chd::format::SignalState                signalState;
    std::optional<chd::metadata::CvbsMetadata> meta;  // present when sidecar found
};

chd_status_t resolveCvbsParams(const std::string &dataPath,
                               const char *metaPathOrNull,
                               const chd_video_params_t *overrideOrNull,
                               ResolvedCvbsParams *out) {
    // Try explicit or auto-located sidecar first.
    std::string sidecar;
    if (metaPathOrNull != nullptr) {
        sidecar = metaPathOrNull;
    } else {
        sidecar = defaultMetaPath(dataPath);
        if (!fs::exists(sidecar)) sidecar.clear();
    }
    if (!sidecar.empty()) {
        if (!fs::exists(sidecar)) {
            return set_error("CVBS metadata sidecar not found: " + sidecar);
        }
        auto parsed = chd::metadata::readCvbsMetadata(sidecar);
        if (!parsed) {
            // detail already populated by reader
            return CHD_E_METADATA_CORRUPT;
        }
        out->videoStandard  = parsed->videoStandard;
        out->sampleEncoding = parsed->sampleEncoding;
        out->signalState    = parsed->signalState;
        out->meta           = std::move(parsed);
        return CHD_OK;
    }

    // No sidecar — fall back to caller-supplied overrides.
    if (overrideOrNull == nullptr) {
        chd::detail::set_last_error(
            "CVBS open: no metadata sidecar and no chd_video_params_t override");
        return CHD_E_METADATA_MISSING;
    }

    // Translate ABI standard enum back into a format preset.
    const chd::format::VideoStandardPreset *standard = nullptr;
    switch (overrideOrNull->standard) {
        case CHD_STD_PAL:   standard = &chd::format::getVideoStandard(chd::format::VideoStandard::PAL);   break;
        case CHD_STD_NTSC:  standard = &chd::format::getVideoStandard(chd::format::VideoStandard::NTSC);  break;
        case CHD_STD_PAL_M: standard = &chd::format::getVideoStandard(chd::format::VideoStandard::PAL_M); break;
        default:
            return set_error("CVBS open: chd_video_params_t.standard unset or unknown");
    }

    chd::format::SampleEncoding encoding;
    switch (overrideOrNull->encoding) {
        case CHD_ENC_CVBS_U10_4FSC:   encoding = chd::format::SampleEncoding::CVBS_U10_4FSC;   break;
        case CHD_ENC_CVBS_U16_4FSC:   encoding = chd::format::SampleEncoding::CVBS_U16_4FSC;   break;
        case CHD_ENC_CVBS_TPG21_4FSC: encoding = chd::format::SampleEncoding::CVBS_TPG21_4FSC; break;
        case CHD_ENC_RAW_S16_28M:     encoding = chd::format::SampleEncoding::RAW_S16_28M;     break;
        case CHD_ENC_RAW_S16_40M:     encoding = chd::format::SampleEncoding::RAW_S16_40M;     break;
        case CHD_ENC_CVBS_U16_4FSC:
            // The TBC layout is equivalent to CVBS_U16_4FSC for sample
            // conversion purposes (10-bit × 64 in u16). Honour the override
            // by mapping to that encoding.
            encoding = chd::format::SampleEncoding::CVBS_U16_4FSC;
            break;
        default:
            return set_error("CVBS open: chd_video_params_t.encoding unset or unknown");
    }

    chd::format::SignalState state;
    switch (overrideOrNull->signal_state) {
        case CHD_SIG_STANDARD_TBC_LOCKED:      state = chd::format::SignalState::STANDARD_TBC_LOCKED;      break;
        case CHD_SIG_STANDARD_TBC_UNLOCKED:    state = chd::format::SignalState::STANDARD_TBC_UNLOCKED;    break;
        case CHD_SIG_STANDARD_RAW:             state = chd::format::SignalState::STANDARD_RAW;             break;
        case CHD_SIG_NONSTANDARD_TBC_LOCKED:   state = chd::format::SignalState::NONSTANDARD_TBC_LOCKED;   break;
        case CHD_SIG_NONSTANDARD_TBC_UNLOCKED: state = chd::format::SignalState::NONSTANDARD_TBC_UNLOCKED; break;
        case CHD_SIG_NONSTANDARD_RAW:          state = chd::format::SignalState::NONSTANDARD_RAW;          break;
        default:
            return set_error("CVBS open: chd_video_params_t.signal_state unset or unknown");
    }

    out->videoStandard  = standard;
    out->sampleEncoding = encoding;
    out->signalState    = state;
    out->meta           = std::nullopt;
    return CHD_OK;
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
    auto tbc = std::make_unique<chd::reader::TbcSource>();
    if (!tbc->open(tbcPath, vp.fieldWidth * vp.fieldHeight, vp.fieldWidth)) {
        return set_error("chd_video_open_composite: failed to open TBC sample file");
    }
    tbc->bindVideoParameters(vp);
    handle->source = std::move(tbc);

    *out = handle.release();
    return CHD_OK;
}

chd_status_t chd_video_open_yc(const char *y_path, const char *c_path,
                               const char *meta_path_or_null,
                               const chd_video_params_t *override_or_null,
                               chd_video_t **out) {
    if (y_path == nullptr || c_path == nullptr || out == nullptr) {
        return set_error("chd_video_open_yc: null argument");
    }
    *out = nullptr;
    if (!fs::exists(y_path) || !fs::exists(c_path)) {
        return set_error("chd_video_open_yc: y/c file does not exist");
    }

    ResolvedCvbsParams resolved{};
    const chd_status_t rc = resolveCvbsParams(y_path, meta_path_or_null,
                                              override_or_null, &resolved);
    if (rc != CHD_OK) return rc;

    auto src = std::make_unique<chd::reader::CvbsYcSource>();
    if (!src->open(y_path, c_path, *resolved.videoStandard,
                   resolved.sampleEncoding, resolved.signalState)) {
        return set_error("chd_video_open_yc: failed to open y/c files");
    }

    auto handle = std::make_unique<chd_video>();
    handle->tbcPath = y_path;
    handle->source  = std::move(src);
    *out = handle.release();
    return CHD_OK;
}

void chd_video_free(chd_video_t *v) {
    delete v;
}

chd_status_t chd_video_get_info(const chd_video_t *v, chd_video_info_t *out) {
    if (v == nullptr || out == nullptr || v->source == nullptr) {
        return set_error("chd_video_get_info: null argument");
    }
    const auto &vp = v->source->parameters();

    out->standard               = toAbiStandard(vp.system);
    // TBC sources report the on-disk format label so consumers can
    // distinguish them from native CVBS-spec inputs; CVBS sources report the
    // preset they were opened with. Metadata presence is the discriminator
    // (TBC opens populate it from the sqlite/json sidecar; CVBS opens don't).
    out->encoding               = (v->metadata != nullptr)
                                      ? CHD_ENC_CVBS_U16_4FSC
                                      : toAbiEncoding(v->source->sampleEncoding());
    out->signal_state           = toAbiSignalState(v->source->signalState());
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
    // Frame count: TBC uses metadata; CVBS sources derive from file
    // size / field count.
    if (v->metadata != nullptr) {
        out->num_frames         = const_cast<chd_video_t *>(v)->metadata->getNumberOfFrames();
        out->is_first_field_first =
            const_cast<chd_video_t *>(v)->metadata->getIsFirstFieldFirst() ? 1 : 0;
    } else {
        const int32_t nf = v->source->getNumberOfAvailableFields();
        out->num_frames = nf >= 0 ? (nf / 2) : 0;
        // CVBS spec doesn't carry a field-order flag in the core schema;
        // default to first-field-first (the common convention).
        out->is_first_field_first = 1;
    }
    out->is_widescreen          = vp.isWidescreen ? 1 : 0;
    out->is_subcarrier_locked   = vp.isSubcarrierLocked ? 1 : 0;
    return CHD_OK;
}

chd_status_t chd_video_add_extra_source_composite(chd_video_t *v, const char *tbc_path,
                                                  const char *) {
    if (v == nullptr || tbc_path == nullptr) {
        return set_error("chd_video_add_extra_source_composite: null argument");
    }
    if (v->metadata == nullptr) {
        return set_error("chd_video_add_extra_source_composite: primary source has no TBC metadata");
    }
    auto src = std::make_unique<chd::reader::TbcSource>();
    const auto &vp = v->metadata->getVideoParameters();
    if (!src->open(tbc_path, vp.fieldWidth * vp.fieldHeight, vp.fieldWidth)) {
        return set_error("chd_video_add_extra_source_composite: failed to open extra TBC source");
    }
    src->bindVideoParameters(vp);
    v->extraSources.push_back(std::move(src));
    return CHD_OK;
}

chd_status_t chd_video_add_extra_source_yc(chd_video_t *v, const char *y_path,
                                           const char *c_path,
                                           const char *meta_path_or_null) {
    if (v == nullptr || y_path == nullptr || c_path == nullptr) {
        return set_error("chd_video_add_extra_source_yc: null argument");
    }
    if (!fs::exists(y_path) || !fs::exists(c_path)) {
        return set_error("chd_video_add_extra_source_yc: y/c file does not exist");
    }
    ResolvedCvbsParams resolved{};
    const chd_status_t rc = resolveCvbsParams(y_path, meta_path_or_null, nullptr, &resolved);
    if (rc != CHD_OK) return rc;
    auto src = std::make_unique<chd::reader::CvbsYcSource>();
    if (!src->open(y_path, c_path, *resolved.videoStandard, resolved.sampleEncoding,
                   resolved.signalState)) {
        return set_error("chd_video_add_extra_source_yc: open failed");
    }
    v->extraSources.push_back(std::move(src));
    return CHD_OK;
}

}  // extern "C"
