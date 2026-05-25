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

// Synthesize an LdDecodeMetaData from an ISource that doesn't carry one
// (CVBS sources, whose .meta sidecar covers VideoParameters but not the
// per-field Field rows that SourceField::loadFields + the multi-source
// DropoutCorrector both consume). Generates alternating is-first-field
// metadata matching the ld-decode convention so 1-based frame
// number → field number translation works.
std::unique_ptr<chd::metadata::LdDecodeMetaData>
synthesizeMetadata(const chd::reader::ISource &src) {
    auto meta = std::make_unique<chd::metadata::LdDecodeMetaData>();
    meta->setVideoParameters(src.parameters());
    meta->setIsFirstFieldFirst(true);

    const int32_t nf = src.getNumberOfAvailableFields();
    if (nf <= 0) return meta;

    for (int32_t i = 0; i < nf; i++) {
        chd::metadata::LdDecodeMetaData::Field f;
        f.seqNo = i + 1;
        f.isFirstField = (i % 2 == 0);
        meta->appendField(f);
    }
    return meta;
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

// A CVBS-spec sidecar uses the `.meta` extension (cvbs_file table); an
// ld-decode sidecar is `.db` (sqlite) or `.json`. The flavour decides which
// reader + metadata path an input takes.
bool isCvbsSidecar(const std::string &path) {
    const std::string suffix = ".meta";
    return path.size() >= suffix.size()
        && path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
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

// Resolve the sidecar for a data file and report its flavour. With an
// explicit sidecar, the flavour follows its extension. Auto-location order:
// the ld-decode `<path>.db` then `<path>.json`, then the CVBS
// `<basename>.meta`. `found` is false when nothing was located (the caller
// then needs a chd_video_params_t override).
struct SidecarResolution {
    std::string path;
    bool        isCvbs = false;
    bool        found  = false;
};

SidecarResolution resolveSidecarFlavour(const std::string &dataPath,
                                        const char *sidecarOrNull) {
    SidecarResolution r;
    if (sidecarOrNull != nullptr) {
        r.path  = sidecarOrNull;
        r.found = fs::exists(r.path);
        r.isCvbs = isCvbsSidecar(r.path);
        return r;
    }
    const std::string db   = dataPath + ".db";
    const std::string json = dataPath + ".json";
    const std::string meta = defaultMetaPath(dataPath);
    if (fs::exists(db))        { r.path = db;   r.found = true; r.isCvbs = false; }
    else if (fs::exists(json)) { r.path = json; r.found = true; r.isCvbs = false; }
    else if (fs::exists(meta)) { r.path = meta; r.found = true; r.isCvbs = true; }
    return r;
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

// One opened composite-shaped source plus the metadata the decode path needs.
// For ld-decode inputs `metadata` carries the real per-field rows; for CVBS
// inputs it is synthesized from the source parameters + field count.
struct OpenedSource {
    std::unique_ptr<chd::reader::ISource> source;
    std::unique_ptr<chd::metadata::LdDecodeMetaData> metadata;
    bool metadataSynthesized = false;
};

// Open a single composite source (ld-decode `.tbc` or CVBS `.composite`),
// auto-detecting the sidecar flavour. `fn` is the caller name for error
// prefixes; `path` must already exist (callers check).
chd_status_t openCompositeSource(const std::string &fn, const std::string &path,
                                 const char *sidecarOrNull,
                                 const chd_video_params_t *overrideOrNull,
                                 OpenedSource *out) {
    const SidecarResolution sc = resolveSidecarFlavour(path, sidecarOrNull);
    if (sidecarOrNull != nullptr && !sc.found) {
        return set_error(fn + ": sidecar file does not exist: " + sc.path);
    }

    if (sc.found && !sc.isCvbs) {
        // ld-decode path: real per-field metadata + TbcSource.
        auto metadata = std::make_unique<chd::metadata::LdDecodeMetaData>();
        try {
            if (!metadata->read(sc.path)) {
                return set_error(fn + ": failed to read sidecar metadata");
            }
        } catch (const std::exception &e) {
            return set_error(fn + ": " + e.what());
        }
        const auto &vp = metadata->getVideoParameters();
        auto src = std::make_unique<chd::reader::TbcSource>();
        if (!src->open(path, vp.fieldWidth * vp.fieldHeight, vp.fieldWidth)) {
            return set_error(fn + ": failed to open sample file");
        }
        src->bindVideoParameters(vp);
        out->source = std::move(src);
        out->metadata = std::move(metadata);
        out->metadataSynthesized = false;
        return CHD_OK;
    }

    // CVBS path: preset triple from the `.meta` sidecar or the override.
    ResolvedCvbsParams resolved{};
    const chd_status_t rc = resolveCvbsParams(
        path, sc.found ? sc.path.c_str() : nullptr, overrideOrNull, &resolved);
    if (rc != CHD_OK) return rc;
    auto src = std::make_unique<chd::reader::CvbsCompositeSource>();
    if (!src->open(path, *resolved.videoStandard, resolved.sampleEncoding,
                   resolved.signalState)) {
        return set_error(fn + ": failed to open sample file");
    }
    out->source = std::move(src);
    out->metadata = synthesizeMetadata(*out->source);
    out->metadataSynthesized = true;
    return CHD_OK;
}

// Open one composite extra source and append it to `dst`.
chd_status_t addCompositeExtra(const std::string &fn,
                               std::vector<chd_video_extra> &dst,
                               const std::string &path, const char *sidecarOrNull) {
    if (!fs::exists(path)) {
        return set_error(fn + ": file does not exist: " + path);
    }
    OpenedSource opened;
    const chd_status_t rc = openCompositeSource(fn, path, sidecarOrNull, nullptr, &opened);
    if (rc != CHD_OK) return rc;
    chd_video_extra extra;
    extra.source = std::move(opened.source);
    extra.metadata = std::move(opened.metadata);
    extra.metadataSynthesized = opened.metadataSynthesized;
    dst.push_back(std::move(extra));
    return CHD_OK;
}

}  // namespace

extern "C" {

chd_status_t chd_video_open_composite(const char *path,
                                      const char *sidecar_path_or_null,
                                      const chd_video_params_t *override_or_null,
                                      chd_video_t **out) {
    if (path == nullptr || out == nullptr) {
        return set_error("chd_video_open_composite: null argument");
    }
    *out = nullptr;
    if (!fs::exists(path)) {
        return set_error(std::string("chd_video_open_composite: file does not exist: ") + path);
    }

    OpenedSource opened;
    const chd_status_t rc = openCompositeSource(
        "chd_video_open_composite", path, sidecar_path_or_null, override_or_null, &opened);
    if (rc != CHD_OK) return rc;

    auto handle = std::make_unique<chd_video>();
    handle->primaryPath        = path;
    handle->source             = std::move(opened.source);
    handle->metadata           = std::move(opened.metadata);
    handle->metadataSynthesized = opened.metadataSynthesized;
    *out = handle.release();
    return CHD_OK;
}

chd_status_t chd_video_open_yc(const char *luma_path, const char *chroma_path,
                               const char *sidecar_path_or_null,
                               const chd_video_params_t *override_or_null,
                               chd_video_t **out) {
    if (luma_path == nullptr || chroma_path == nullptr || out == nullptr) {
        return set_error("chd_video_open_yc: null argument");
    }
    *out = nullptr;
    if (!fs::exists(luma_path) || !fs::exists(chroma_path)) {
        return set_error("chd_video_open_yc: luma/chroma file does not exist");
    }

    const SidecarResolution sc = resolveSidecarFlavour(luma_path, sidecar_path_or_null);
    // A CVBS `.y`/`.c` pair (a `.meta` sidecar, or no sidecar + override) reads
    // through a single CvbsYcSource that reconstructs a composite from the
    // centred-chroma `.c`. A vhs-decode luma.tbc + chroma.tbc pair instead
    // decodes each plane separately and merges (set up below).
    const bool decodeMerge = sc.found && !sc.isCvbs;

    auto handle = std::make_unique<chd_video>();
    handle->primaryPath = luma_path;

    if (!decodeMerge) {
        ResolvedCvbsParams resolved{};
        const chd_status_t rc = resolveCvbsParams(
            luma_path, sc.found ? sc.path.c_str() : nullptr, override_or_null, &resolved);
        if (rc != CHD_OK) return rc;
        auto src = std::make_unique<chd::reader::CvbsYcSource>();
        if (!src->open(luma_path, chroma_path, *resolved.videoStandard,
                       resolved.sampleEncoding, resolved.signalState)) {
            return set_error("chd_video_open_yc: failed to open y/c files");
        }
        handle->metadata = synthesizeMetadata(*src);
        handle->metadataSynthesized = true;
        handle->source  = std::move(src);
        *out = handle.release();
        return CHD_OK;
    }

    OpenedSource luma;
    chd_status_t rc = openCompositeSource(
        "chd_video_open_yc (luma)", luma_path, sidecar_path_or_null, override_or_null, &luma);
    if (rc != CHD_OK) return rc;

    // Prefer the chroma plane's own sidecar, but fall back to the luma sidecar:
    // vhs-decode writes one shared `<base>.tbc.json` for the pair, not a
    // separate `<base>_chroma.tbc.json`. The shared sidecar describes the
    // (identical) geometry of both planes.
    const SidecarResolution chromaSc = resolveSidecarFlavour(chroma_path, nullptr);
    const char *chromaSidecar = chromaSc.found ? nullptr : sc.path.c_str();
    OpenedSource chroma;
    rc = openCompositeSource(
        "chd_video_open_yc (chroma)", chroma_path, chromaSidecar, override_or_null, &chroma);
    if (rc != CHD_OK) return rc;

    // The two planes come from one capture; require matching geometry and
    // frame count so the decoded Y and U/V line up.
    const auto &lvp = luma.source->parameters();
    const auto &cvp = chroma.source->parameters();
    if (lvp.fieldWidth != cvp.fieldWidth || lvp.fieldHeight != cvp.fieldHeight) {
        return set_error("chd_video_open_yc: luma and chroma have mismatched dimensions");
    }
    if (luma.metadata->getNumberOfFrames() != chroma.metadata->getNumberOfFrames()) {
        return set_error("chd_video_open_yc: luma and chroma have different frame counts");
    }

    handle->source             = std::move(luma.source);
    handle->metadata           = std::move(luma.metadata);
    handle->metadataSynthesized = luma.metadataSynthesized;
    handle->chromaSource       = std::move(chroma.source);
    handle->chromaMetadata     = std::move(chroma.metadata);
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
    // Report the source's actual sample encoding. An ld-decode `.tbc` reads as
    // CVBS_U16_4FSC (its on-disk layout); CVBS sources report the preset they
    // were opened with.
    out->encoding               = toAbiEncoding(v->source->sampleEncoding());
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
    // Both TBC and (synthesized) CVBS metadata expose frame count + field
    // order through the same accessors.
    out->num_frames         = const_cast<chd_video_t *>(v)->metadata->getNumberOfFrames();
    out->is_first_field_first =
        const_cast<chd_video_t *>(v)->metadata->getIsFirstFieldFirst() ? 1 : 0;
    out->is_widescreen          = vp.isWidescreen ? 1 : 0;
    out->is_subcarrier_locked   = vp.isSubcarrierLocked ? 1 : 0;
    return CHD_OK;
}

chd_status_t chd_video_add_extra_source_composite(chd_video_t *v, const char *path,
                                                  const char *sidecar_path_or_null) {
    if (v == nullptr || path == nullptr) {
        return set_error("chd_video_add_extra_source_composite: null argument");
    }
    return addCompositeExtra("chd_video_add_extra_source_composite",
                             v->extraSources, path, sidecar_path_or_null);
}

chd_status_t chd_video_add_extra_source_yc(chd_video_t *v, const char *luma_path,
                                           const char *chroma_path,
                                           const char *sidecar_path_or_null) {
    if (v == nullptr || luma_path == nullptr || chroma_path == nullptr) {
        return set_error("chd_video_add_extra_source_yc: null argument");
    }
    if (!fs::exists(luma_path) || !fs::exists(chroma_path)) {
        return set_error("chd_video_add_extra_source_yc: luma/chroma file does not exist");
    }

    const SidecarResolution sc = resolveSidecarFlavour(luma_path, sidecar_path_or_null);
    if (sc.found && !sc.isCvbs) {
        // vhs-decode pair: a luma extra corrects the luma plane and a chroma
        // extra corrects the separately-decoded chroma plane.
        chd_status_t rc = addCompositeExtra(
            "chd_video_add_extra_source_yc (luma)", v->extraSources,
            luma_path, sidecar_path_or_null);
        if (rc != CHD_OK) return rc;
        // Chroma plane falls back to the shared luma sidecar (vhs-decode writes
        // one `.tbc.json` per pair).
        const SidecarResolution chromaSc = resolveSidecarFlavour(chroma_path, nullptr);
        const char *chromaSidecar = chromaSc.found ? nullptr : sc.path.c_str();
        return addCompositeExtra(
            "chd_video_add_extra_source_yc (chroma)", v->chromaExtraSources,
            chroma_path, chromaSidecar);
    }

    // CVBS `.y`/`.c` pair: one CvbsYcSource extra, matching the primary.
    ResolvedCvbsParams resolved{};
    const chd_status_t rc = resolveCvbsParams(
        luma_path, sc.found ? sc.path.c_str() : nullptr, nullptr, &resolved);
    if (rc != CHD_OK) return rc;
    auto src = std::make_unique<chd::reader::CvbsYcSource>();
    if (!src->open(luma_path, chroma_path, *resolved.videoStandard,
                   resolved.sampleEncoding, resolved.signalState)) {
        return set_error("chd_video_add_extra_source_yc: open failed");
    }
    chd_video_extra extra;
    extra.metadata = synthesizeMetadata(*src);
    extra.metadataSynthesized = true;
    extra.source = std::move(src);
    v->extraSources.push_back(std::move(extra));
    return CHD_OK;
}

}  // extern "C"
