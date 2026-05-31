// SPDX-License-Identifier: GPL-3.0-or-later
//
// chd_decoder lifecycle (set_option/set_nn_model/commit) +
// chd_decode_frame (sync, random-access) + chd_decode_frames_async.
//
// The handle stores caller options uncommitted until chd_decoder_commit,
// at which point the registry builds the concrete IDecoder, the
// OutputWriter is configured, and the decoder is configured against the
// post-padding VideoParameters. From that point chd_decode_frame is
// callable until chd_decoder_free.

#include <chromadec/decoder.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../common/error_state.h"
#include "../decoders/registry.h"
#include "../decoders/source_field.h"
#include "../dropout/dropout_corrector.h"
#include "../metadata/core.h"
#include "../output/component_frame.h"
#include "../reader/source.h"
#include "handles.h"

namespace {

chd_status_t set_arg_error(const char *what, const char *detail = nullptr) {
    std::string msg = std::string("chromadec: ") + what + ": null argument";
    if (detail != nullptr) msg += " (" + std::string(detail) + ")";
    chd::detail::set_last_error(msg);
    return CHD_E_INVALID_ARG;
}

// Translate the string form of the output_format option to an
// OutputWriter::PixelFormat. Returns -1 on unknown name. The float-output
// formats ("yuv444ps", "grays", "rgbs") bypass OutputWriter::convert entirely
// (we render direct from the ComponentFrame via OutputWriter::convertToFloat
// / convertToFloatRGB); OutputWriter still needs a valid integer format to
// size headers / padding, so each maps to the integer format with the
// matching plane layout.
int parseOutputFormat(const std::string &name) {
    if (name == "yuv444p16" || name == "YUV444P16") return chd::output::OutputWriter::YUV444P16;
    if (name == "rgb48"     || name == "RGB48")     return chd::output::OutputWriter::RGB48;
    if (name == "gray16"    || name == "GRAY16")    return chd::output::OutputWriter::GRAY16;
    if (name == "yuv444ps"  || name == "YUV444PS")  return chd::output::OutputWriter::YUV444P16;
    if (name == "rgbs"      || name == "RGBS")      return chd::output::OutputWriter::RGB48;
    if (name == "grays"     || name == "GRAYS")     return chd::output::OutputWriter::GRAY16;
    return -1;
}

chd_pixel_format_t parseChdPixelFormat(const std::string &name) {
    if (name == "yuv444p16" || name == "YUV444P16") return CHD_PIXEL_YUV444P16;
    if (name == "rgb48"     || name == "RGB48")     return CHD_PIXEL_RGB48;
    if (name == "gray16"    || name == "GRAY16")    return CHD_PIXEL_GRAY16;
    if (name == "yuv444ps"  || name == "YUV444PS")  return CHD_PIXEL_YUV444PS;
    if (name == "rgbs"      || name == "RGBS")      return CHD_PIXEL_RGBS;
    if (name == "grays"     || name == "GRAYS")     return CHD_PIXEL_GRAYS;
    return CHD_PIXEL_YUV444P16;
}

// Translate the string form of the output_clamp option to an
// OutputWriter::ClampMode. Returns -1 on unknown name.
int parseClampMode(const std::string &name) {
    if (name == "none" || name == "NONE")
        return chd::output::OutputWriter::CLAMP_NONE;
    if (name == "legal_rgb_sdr" || name == "LEGAL_RGB_SDR")
        return chd::output::OutputWriter::CLAMP_LEGAL_RGB_SDR;
    if (name == "legal_rgb_hdr" || name == "LEGAL_RGB_HDR")
        return chd::output::OutputWriter::CLAMP_LEGAL_RGB_HDR;
    if (name == "legal_ycbcr_bt601" || name == "LEGAL_YCBCR_BT601")
        return chd::output::OutputWriter::CLAMP_LEGAL_YCBCR_BT601;
    return -1;
}

// Apply caller-supplied first/last_active_*_line overrides to the
// VideoParameters before the decoder configures itself. Mirrors
// LineParameters::applyTo with the i32 option-map as the source.
void applyLineOverrides(chd::metadata::LdDecodeMetaData::VideoParameters &vp,
                        const chd::decoders::registry::OptionMaps &o) {
    auto get = [&](const char *k, int32_t fallback) -> int32_t {
        auto it = o.i32.find(k);
        return it == o.i32.end() ? fallback : it->second;
    };
    const int32_t a = get(CHD_OPT_FIRST_ACTIVE_FIELD_LINE, -1);
    const int32_t b = get(CHD_OPT_LAST_ACTIVE_FIELD_LINE,  -1);
    const int32_t c = get(CHD_OPT_FIRST_ACTIVE_FRAME_LINE, -1);
    const int32_t e = get(CHD_OPT_LAST_ACTIVE_FRAME_LINE,  -1);
    if (a >= 0) vp.firstActiveFieldLine = a;
    if (b >= 0) vp.lastActiveFieldLine  = b;
    if (c >= 0) vp.firstActiveFrameLine = c;
    if (e >= 0) vp.lastActiveFrameLine  = e;
}

template <typename T>
chd_status_t setOpt(chd_decoder_t *d, const char *name,
                    chd::decoders::registry::OptionType type, T value,
                    std::unordered_map<std::string, T> &dst) {
    if (d == nullptr || name == nullptr) return set_arg_error("chd_decoder_set_option");
    if (d->committed) {
        chd::detail::set_last_error("chd_decoder_set_option: decoder already committed");
        return CHD_E_INVALID_ARG;
    }
    if (!chd::decoders::registry::optionApplies(d->kind, name, type)) {
        chd::detail::set_last_error(std::string("chd_decoder_set_option: option \"")
                                    + name + "\" not valid for this decoder kind");
        return CHD_E_INVALID_ARG;
    }
    dst[name] = std::move(value);
    return CHD_OK;
}

// Decode one frame using the worker-`workerIdx` decoder. Caller MUST hold
// d->decoderMutexes[workerIdx] for the entire call — Decoder subclasses
// keep mutable per-call scratch that isn't reentrant. The shared
// OutputWriter is read-only after commit so workers don't need to
// serialise on it. lastDropoutStats is published under d->statsMutex.
chd_status_t decodeFrameLocked(chd_decoder_t *d, size_t workerIdx,
                               int64_t frame_index, chd_frame_t **out) {
    chd::metadata::LdDecodeMetaData *meta = d->video->metadata.get();
    if (meta == nullptr) {
        chd::detail::set_last_error("chd_decode_frame: missing metadata");
        return CHD_E_METADATA_MISSING;
    }
    const int32_t numFrames = meta->getNumberOfFrames();
    if (frame_index < 0 || frame_index >= numFrames) {
        chd::detail::set_last_error("chd_decode_frame: frame_index out of range");
        return CHD_E_OUT_OF_RANGE;
    }

    // Load the field window (lookbehind + this frame + lookahead) from the
    // primary source. frame_index is 0-based at the C ABI; SourceField is
    // 1-based internally.
    std::vector<chd::decoders::SourceField> fields;
    int32_t startIndex = 0;
    int32_t endIndex   = 0;
    const int32_t firstFrameNumber = static_cast<int32_t>(frame_index) + 1;
    chd::decoders::SourceField::loadFields(
        *d->video->source, *meta,
        firstFrameNumber, /*numFrames=*/1, d->lookBehind, d->lookAhead,
        fields, startIndex, endIndex);

    // Apply dropout correction if requested. Single-source if no extras
    // are attached; otherwise build a vector<ExtraSourceFrame> from each
    // chd_video_extra and use the multi-source DropoutCorrector overload.
    chd::dropout::DropoutCorrectionStats stats;
    if (d->dropoutOptsSet && d->dropoutOpts.enabled != 0) {
        chd::dropout::DropoutCorrector corrector(d->videoParameters);

        // Build the multi-source VBI alignment once. Its constructor scans
        // every source's field VBI, so it is cached on the decoder and shared
        // (read-only) across workers.
        if (!d->video->extraSources.empty()) {
            std::call_once(d->multiSourceAlignmentOnce, [&] {
                std::vector<chd::metadata::LdDecodeMetaData *> sources;
                sources.reserve(d->video->extraSources.size() + 1);
                sources.push_back(meta);  // primary capture is source 0
                for (auto &ex : d->video->extraSources) sources.push_back(ex.metadata.get());
                d->multiSourceAlignment =
                    std::make_unique<chd::dropout::MultiSourceAlignment>(std::move(sources));
            });
        }

        std::vector<chd::dropout::ExtraSourceFrame> extras;
        extras.reserve(d->video->extraSources.size());
        for (size_t i = 0; i < d->video->extraSources.size(); ++i) {
            auto &ex = d->video->extraSources[i];
            if (ex.metadata == nullptr || ex.source == nullptr) continue;
            const int32_t exFrames = ex.metadata->getNumberOfFrames();

            // Map the primary frame to the matching disc frame in this source
            // (by VBI CAV/CLV number where available, positionally otherwise).
            const int32_t exFrame = d->multiSourceAlignment->sourceFrameForPrimaryFrame(
                firstFrameNumber, static_cast<int32_t>(i) + 1);
            if (exFrame < 1 || exFrame > exFrames) {
                // This source does not cover the primary's frame — skip it for
                // this frame rather than padding with black.
                continue;
            }
            const int32_t exFirst  = ex.metadata->getFirstFieldNumber(exFrame);
            const int32_t exSecond = ex.metadata->getSecondFieldNumber(exFrame);

            chd::dropout::ExtraSourceFrame esf;
            esf.firstFieldData  = ex.source->getVideoField(exFirst);
            esf.secondFieldData = ex.source->getVideoField(exSecond);
            esf.firstFieldMeta  = ex.metadata->getField(exFirst);
            esf.secondFieldMeta = ex.metadata->getField(exSecond);
            esf.videoParams     = ex.metadata->getVideoParameters();
            // Quality: VITS bPSNR average; synthesized CVBS metadata leaves
            // VitsMetrics inUse=false / bPSNR=0, which is fine — the
            // corrector still treats it as a usable extra, just without a
            // quality-based tiebreaker.
            esf.quality = (esf.firstFieldMeta.vitsMetrics.bPSNR
                         + esf.secondFieldMeta.vitsMetrics.bPSNR) / 2.0;
            extras.push_back(std::move(esf));
        }

        if (extras.empty()) {
            corrector.correctFrame(fields[startIndex], fields[startIndex + 1],
                                   d->dropoutOpts.overcorrect != 0,
                                   d->dropoutOpts.intra_field_only != 0,
                                   &stats);
        } else {
            corrector.correctFrame(fields[startIndex], fields[startIndex + 1],
                                   extras,
                                   d->dropoutOpts.overcorrect != 0,
                                   d->dropoutOpts.intra_field_only != 0,
                                   &stats);
        }
    }

    std::vector<chd::output::ComponentFrame> componentFrames(1);
    try {
        d->decoders[workerIdx]->decodeFrames(fields, startIndex, endIndex, componentFrames);
    } catch (const std::exception &e) {
        chd::detail::set_last_error(std::string("chd_decode_frame: decoder threw: ") + e.what());
        return CHD_E_INTERNAL;
    }

    auto frame = std::make_unique<chd_frame>();
    frame->format = d->outputPixelFormat;

    const int32_t activeWidth =
        d->videoParameters.activeVideoEnd - d->videoParameters.activeVideoStart;

    if (frame->format == CHD_PIXEL_YUV444PS || frame->format == CHD_PIXEL_GRAYS) {
        // Honor any requested padding: the float path mirrors the integer path's
        // committed geometry (top/bottom blank lines), it does not crop tight.
        // GRAYS emits only the E'Y plane; YUV444PS also emits E'Cb/E'Cr.
        const bool includeChroma = (frame->format == CHD_PIXEL_YUV444PS);
        const int32_t outputHeight = d->outputWriter.getOutputHeight();
        d->outputWriter.convertToFloat(componentFrames[0], frame->floatPlane, includeChroma);
        frame->activeWidth = activeWidth;
        frame->outputHeight = outputHeight;
        frame->info.format = frame->format;
        frame->info.width  = activeWidth;
        frame->info.height = outputHeight;
        frame->info.num_planes = includeChroma ? 3 : 1;
    } else if (frame->format == CHD_PIXEL_RGBS) {
        // Direct ComponentFrame → R'G'B' float planes; no Y'CbCr integer
        // intermediate. Same geometry as the integer convert() path.
        const int32_t outputHeight = d->outputWriter.getOutputHeight();
        d->outputWriter.convertToFloatRGB(componentFrames[0], frame->floatPlane);
        frame->activeWidth = activeWidth;
        frame->outputHeight = outputHeight;
        frame->info.format = frame->format;
        frame->info.width  = activeWidth;
        frame->info.height = outputHeight;
        frame->info.num_planes = 3;
    } else {
        d->outputWriter.convert(componentFrames[0], frame->u16Plane);
        const int32_t planes =
            (d->outputConfig.pixelFormat == chd::output::OutputWriter::GRAY16) ? 1 : 3;
        const int32_t outputHeight = static_cast<int32_t>(
            frame->u16Plane.size() / static_cast<size_t>(activeWidth) / planes);
        frame->activeWidth  = activeWidth;
        frame->outputHeight = outputHeight;
        frame->info.format  = d->outputPixelFormat;
        frame->info.width   = activeWidth;
        frame->info.height  = outputHeight;
        frame->info.num_planes = planes;
    }
    frame->info.frame_index = frame_index;

    {
        std::lock_guard<std::mutex> sl(d->statsMutex);
        d->lastDropoutStats.corrected      = stats.corrected;
        d->lastDropoutStats.failed         = stats.failed;
        d->lastDropoutStats.total_distance = stats.totalDistance;
    }

    *out = frame.release();
    return CHD_OK;
}

}  // namespace

extern "C" {

// ── Lifecycle ────────────────────────────────────────────────────────────────

chd_status_t chd_decoder_create(chd_video_t *v, chd_decoder_kind_t kind, chd_decoder_t **out) {
    if (v == nullptr || out == nullptr) return set_arg_error("chd_decoder_create");
    auto handle = std::make_unique<chd_decoder>();
    handle->video = v;
    handle->kind  = kind;
    *out = handle.release();
    return CHD_OK;
}

void chd_decoder_free(chd_decoder_t *d) { delete d; }

// ── Options ──────────────────────────────────────────────────────────────────

chd_status_t chd_decoder_set_option_f64(chd_decoder_t *d, const char *name, double v) {
    return setOpt(d, name, chd::decoders::registry::OptionType::F64, v, d->optionMaps.f64);
}

chd_status_t chd_decoder_set_option_i32(chd_decoder_t *d, const char *name, int32_t v) {
    return setOpt(d, name, chd::decoders::registry::OptionType::I32, v, d->optionMaps.i32);
}

chd_status_t chd_decoder_set_option_bool(chd_decoder_t *d, const char *name, int v) {
    return setOpt<bool>(d, name, chd::decoders::registry::OptionType::Bool, v != 0,
                        d->optionMaps.boolean);
}

chd_status_t chd_decoder_set_option_str(chd_decoder_t *d, const char *name, const char *v) {
    if (v == nullptr) return set_arg_error("chd_decoder_set_option_str", "value");
    return setOpt<std::string>(d, name, chd::decoders::registry::OptionType::Str, v,
                               d->optionMaps.str);
}

chd_status_t chd_decoder_has_option(const chd_decoder_t *d, const char *name) {
    if (d == nullptr || name == nullptr) return set_arg_error("chd_decoder_has_option");
    using OT = chd::decoders::registry::OptionType;
    const bool any = chd::decoders::registry::optionApplies(d->kind, name, OT::F64)
                  || chd::decoders::registry::optionApplies(d->kind, name, OT::I32)
                  || chd::decoders::registry::optionApplies(d->kind, name, OT::Bool)
                  || chd::decoders::registry::optionApplies(d->kind, name, OT::Str);
    return any ? CHD_OK : CHD_E_INVALID_ARG;
}

chd_status_t chd_decoder_set_nn_model(chd_decoder_t *d, chd_nn_model_t *m) {
    if (d == nullptr) return set_arg_error("chd_decoder_set_nn_model");
    if (d->committed) {
        chd::detail::set_last_error("chd_decoder_set_nn_model: decoder already committed");
        return CHD_E_INVALID_ARG;
    }
#if defined(CHD_WITH_NN)
    if (!chd::decoders::registry::kindUsesNn(d->kind) && d->kind != CHD_DEC_AUTO) {
        chd::detail::set_last_error(
            "chd_decoder_set_nn_model: decoder kind does not accept an NN model");
        return CHD_E_INVALID_ARG;
    }
    d->nnModelPending = (m != nullptr) ? m->session : nullptr;
    return CHD_OK;
#else
    (void)m;
    chd::detail::set_last_error("chd_decoder_set_nn_model: library built without NN support");
    return CHD_E_INTERNAL;
#endif
}

// ── Commit ───────────────────────────────────────────────────────────────────

chd_status_t chd_decoder_commit(chd_decoder_t *d) {
    if (d == nullptr) return set_arg_error("chd_decoder_commit");
    if (d->committed) return CHD_OK;  // idempotent: re-commit is a no-op
    // Caller contract: chd_decoder_commit + chd_decoder_set_option_* +
    // chd_decode_frame must not be called concurrently with each other
    // (the public header documents this on chd_decoder_set_option_*).
    // No lifecycle mutex needed.

    if (d->video == nullptr || d->video->source == nullptr) {
        chd::detail::set_last_error("chd_decoder_commit: video has no source");
        return CHD_E_INVALID_ARG;
    }

    const chd::metadata::VideoSystem system = d->video->source->parameters().system;
    const chd_decoder_kind_t resolved = chd::decoders::registry::resolveAuto(d->kind, system);

    chd::output::OutputWriter::Configuration outCfg;
    // Default to no padding (1 = output the active region as-is). Consumers that
    // need codec-friendly dimensions can opt in via CHD_OPT_PADDING_MULTIPLE.
    outCfg.paddingAmount = 1;
    {
        auto it = d->optionMaps.i32.find(CHD_OPT_PADDING_MULTIPLE);
        if (it != d->optionMaps.i32.end()) outCfg.paddingAmount = it->second;
    }
    outCfg.outputY4m = false;
    {
        auto it = d->optionMaps.boolean.find(CHD_OPT_OUTPUT_Y4M_HEADERS);
        if (it != d->optionMaps.boolean.end()) outCfg.outputY4m = it->second;
    }

    chd_pixel_format_t abiFormat = CHD_PIXEL_YUV444P16;
    {
        auto it = d->optionMaps.str.find(CHD_OPT_OUTPUT_FORMAT);
        if (it != d->optionMaps.str.end()) {
            const int pf = parseOutputFormat(it->second);
            if (pf < 0) {
                chd::detail::set_last_error(
                    "chd_decoder_commit: unknown output_format \"" + it->second + "\"");
                return CHD_E_INVALID_ARG;
            }
            outCfg.pixelFormat = static_cast<chd::output::OutputWriter::PixelFormat>(pf);
            abiFormat = parseChdPixelFormat(it->second);
        } else {
            outCfg.pixelFormat = chd::output::OutputWriter::YUV444P16;
            abiFormat = CHD_PIXEL_YUV444P16;
        }
    }
    {
        auto it = d->optionMaps.str.find(CHD_OPT_OUTPUT_CLAMP);
        if (it != d->optionMaps.str.end()) {
            const int cm = parseClampMode(it->second);
            if (cm < 0) {
                chd::detail::set_last_error(
                    "chd_decoder_commit: unknown output_clamp \"" + it->second + "\"");
                return CHD_E_INVALID_ARG;
            }
            outCfg.clampMode = static_cast<chd::output::OutputWriter::ClampMode>(cm);
        }
    }

    chd::metadata::LdDecodeMetaData *meta = d->video->metadata.get();
    if (meta == nullptr) {
        chd::detail::set_last_error("chd_decoder_commit: failed to resolve video metadata");
        return CHD_E_METADATA_MISSING;
    }
    chd::metadata::LdDecodeMetaData::VideoParameters vp = meta->getVideoParameters();
    applyLineOverrides(vp, d->optionMaps);

    // Apply REVERSE_FIELD_ORDER. Matches upstream ld-chroma-decoder `-r`
    // flag: flips the metadata-wide isFirstFieldFirst flag, which
    // SourceField::loadFields consumes via getFirstFieldNumber /
    // getSecondFieldNumber to swap which physical field is treated as
    // "first" within each frame. Default (no option, or option=false)
    // restores still-frame default of first-field-first.
    {
        auto it = d->optionMaps.boolean.find(CHD_OPT_REVERSE_FIELD_ORDER);
        const bool reverse = (it != d->optionMaps.boolean.end()) && it->second;
        meta->setIsFirstFieldFirst(!reverse);
    }

    // OutputWriter::updateConfiguration mutates vp (active region padding);
    // every per-worker decoder configures against the same post-padding vp.
    d->outputWriter.updateConfiguration(vp, outCfg);

    if (d->kind == CHD_DEC_NONE) {
        // Geometry/metadata-only decoder: snapshot the committed framing but
        // build no chroma decode engines and require no NN model. The dropout
        // span/mask queries and chd_decoder_get_output_info read this state;
        // chd_decode_frame is rejected.
        d->resolvedKind      = CHD_DEC_NONE;
        d->videoParameters   = vp;
        d->outputConfig      = outCfg;
        d->outputPixelFormat = abiFormat;
        d->lookBehind        = 0;
        d->lookAhead         = 0;
        d->threadCount       = 0;
        d->committed         = true;
        return CHD_OK;
    }

    // Resolve worker count before building decoders.
    int32_t threadCount = 0;
    {
        auto it = d->optionMaps.i32.find(CHD_OPT_THREAD_COUNT);
        threadCount = (it != d->optionMaps.i32.end()) ? it->second : 0;
        if (threadCount <= 0) {
            const unsigned hc = std::thread::hardware_concurrency();
            threadCount = hc == 0 ? 2 : static_cast<int32_t>(hc);
        }
    }

    // Build threadCount decoder instances so chd_decode_frames_async
    // workers can each own one without locking. Configure each; bind the
    // shared NN session to each if applicable. NN kinds without a model
    // bound fail fast.
#if defined(CHD_WITH_NN)
    const bool needsNn = chd::decoders::registry::kindUsesNn(resolved);
    if (needsNn && !d->nnModelPending) {
        chd::detail::set_last_error(
            "chd_decoder_commit: NN decoder kind requires chd_decoder_set_nn_model first");
        return CHD_E_NN_MODEL_LOAD;
    }
#endif

    std::vector<std::unique_ptr<chd::decoders::Decoder>> built;
    std::vector<std::unique_ptr<std::mutex>>             builtMutexes;
    built.reserve(threadCount);
    builtMutexes.reserve(threadCount);
    for (int32_t w = 0; w < threadCount; w++) {
        auto inst = chd::decoders::registry::build(resolved, d->optionMaps);
        if (!inst) {
            chd::detail::set_last_error("chd_decoder_commit: unknown or unsupported decoder kind");
            return CHD_E_DECODER_UNKNOWN;
        }
#if defined(CHD_WITH_NN)
        if (d->nnModelPending) {
            if (!chd::decoders::registry::applyNnModel(resolved, *inst, d->nnModelPending)) {
                chd::detail::set_last_error(
                    "chd_decoder_commit: NN model bound to a non-NN decoder kind");
                return CHD_E_DECODER_INCOMPATIBLE;
            }
        }
#endif
        if (!inst->configure(vp)) {
            chd::detail::set_last_error(
                "chd_decoder_commit: decoder rejected input (incompatible video standard?)");
            return CHD_E_DECODER_INCOMPATIBLE;
        }
        built.push_back(std::move(inst));
        builtMutexes.push_back(std::make_unique<std::mutex>());
    }

    // The metadata's VideoParameters stay at their pre-padding values:
    // SourceField::loadFields only reads `isSubcarrierLocked` from it
    // (which padding doesn't touch), and DecoderPool follows the same
    // convention of not writing back the post-padding vp. Keeping the
    // metadata pristine lets multiple decoders share one chd_video with
    // independent padding choices.

    d->resolvedKind = resolved;
    d->decoders = std::move(built);
    d->decoderMutexes = std::move(builtMutexes);
    d->videoParameters = vp;
    d->outputConfig = outCfg;
    d->outputPixelFormat = abiFormat;
    d->lookBehind = d->decoders.front()->getLookBehind();
    d->lookAhead  = d->decoders.front()->getLookAhead();
    d->threadCount = threadCount;

    d->committed = true;
    return CHD_OK;
}

// ── Sync decode ─────────────────────────────────────────────────────────────

chd_status_t chd_decode_frame(chd_decoder_t *d, int64_t frame_index, chd_frame_t **out) {
    if (d == nullptr || out == nullptr) return set_arg_error("chd_decode_frame");
    *out = nullptr;
    if (!d->committed) {
        chd::detail::set_last_error("chd_decode_frame: chd_decoder_commit not called");
        return CHD_E_INVALID_ARG;
    }
    if (d->resolvedKind == CHD_DEC_NONE) {
        chd::detail::set_last_error(
            "chd_decode_frame: decoder kind is CHD_DEC_NONE (geometry/dropout only)");
        return CHD_E_DECODER_INCOMPATIBLE;
    }
    // Sync decode uses worker 0's decoder + its own mutex. Concurrent
    // chd_decode_frames_async calls can run on workers 1..N-1 against
    // different decoder instances; they don't contend with sync.
    std::lock_guard<std::mutex> lock(*d->decoderMutexes[0]);
    return decodeFrameLocked(d, /*workerIdx=*/0, frame_index, out);
}

// ── Async decode ─────────────────────────────────────────────────────────────

chd_status_t chd_decode_frames_async(chd_decoder_t *d,
                                      const int64_t *indices, size_t n,
                                      chd_frame_done_cb cb, void *user,
                                      chd_cancel_t *cancel) {
    if (d == nullptr || cb == nullptr) return set_arg_error("chd_decode_frames_async");
    if (n > 0 && indices == nullptr) return set_arg_error("chd_decode_frames_async", "indices");
    if (!d->committed) {
        chd::detail::set_last_error("chd_decode_frames_async: chd_decoder_commit not called");
        return CHD_E_INVALID_ARG;
    }
    if (d->resolvedKind == CHD_DEC_NONE) {
        chd::detail::set_last_error(
            "chd_decode_frames_async: decoder kind is CHD_DEC_NONE (geometry/dropout only)");
        return CHD_E_DECODER_INCOMPATIBLE;
    }
    if (n == 0) return CHD_OK;

    // W workers; each owns decoder[w] + decoderMutexes[w] for the duration
    // of the call. Workers race-pull next-index from `next` so unequal
    // frame costs balance automatically. The mutex per worker is needed
    // only because `chd_decode_frame` (sync) may run concurrently against
    // worker 0 — without sync contention, the mutex would just be the
    // worker's stack discipline.
    const size_t W = std::min<size_t>(n, d->decoders.size());
    std::atomic<size_t> next{0};
    std::vector<std::thread> workers;
    workers.reserve(W);
    for (size_t w = 0; w < W; w++) {
        workers.emplace_back([d, indices, n, cb, user, cancel, w, &next]() {
            std::lock_guard<std::mutex> lock(*d->decoderMutexes[w]);
            while (true) {
                const size_t i = next.fetch_add(1, std::memory_order_relaxed);
                if (i >= n) return;
                const int64_t idx = indices[i];
                if (cancel != nullptr && cancel->requested.load(std::memory_order_acquire)) {
                    cb(user, CHD_E_CANCELLED, idx, nullptr);
                    continue;
                }
                chd_frame_t *frame = nullptr;
                const chd_status_t rc = decodeFrameLocked(d, w, idx, &frame);
                cb(user, rc, idx, frame);
            }
        });
    }
    for (auto &t : workers) t.join();
    return CHD_OK;
}

}  // extern "C"
