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

#include <future>
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

// Synthesize an LdDecodeMetaData from an ISource that doesn't carry one
// (CVBS primary sources). SourceField::loadFields needs Field structs and
// frame-number → field-number translation; we generate alternating
// is-first-field metadata matching the legacy convention.
std::unique_ptr<chd::metadata::LdDecodeMetaData> synthesizeMetadata(const chd::reader::ISource &src) {
    auto meta = std::make_unique<chd::metadata::LdDecodeMetaData>();
    meta->setVideoParameters(src.parameters());
    meta->setIsFirstFieldFirst(true);

    const int32_t nf = src.getNumberOfAvailableFields();
    if (nf <= 0) return meta;

    for (int32_t i = 0; i < nf; i++) {
        chd::metadata::LdDecodeMetaData::Field f;
        f.seqNo = i + 1;
        f.isFirstField = (i % 2 == 0);  // matches the test fixture convention
        meta->appendField(f);
    }
    return meta;
}

// Translate the string form of the output_format option to an
// OutputWriter::PixelFormat. Returns -1 on unknown name. The "yuv444_float"
// alias maps to YUV444P16 here because the float-output path bypasses
// OutputWriter::convert entirely (we render direct from the ComponentFrame);
// OutputWriter still needs a valid format to size headers / padding.
int parseOutputFormat(const std::string &name) {
    if (name == "yuv444p16" || name == "YUV444P16") return chd::output::OutputWriter::YUV444P16;
    if (name == "rgb48"     || name == "RGB48")     return chd::output::OutputWriter::RGB48;
    if (name == "gray16"    || name == "GRAY16")    return chd::output::OutputWriter::GRAY16;
    if (name == "yuv444_float" || name == "YUV444_FLOAT") return chd::output::OutputWriter::YUV444P16;
    return -1;
}

chd_pixel_format_t parseChdPixelFormat(const std::string &name) {
    if (name == "yuv444p16" || name == "YUV444P16") return CHD_PIXEL_YUV444P16;
    if (name == "rgb48"     || name == "RGB48")     return CHD_PIXEL_RGB48;
    if (name == "gray16"    || name == "GRAY16")    return CHD_PIXEL_GRAY16;
    if (name == "yuv444_float" || name == "YUV444_FLOAT") return CHD_PIXEL_YUV444_FLOAT;
    return CHD_PIXEL_YUV444P16;
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

// Resolve a metadata pointer for this video: TBC sources own one, CVBS
// sources synthesize one at commit time and stash it on the video so
// subsequent decoder commits against the same video share it.
chd::metadata::LdDecodeMetaData *resolveMetadata(chd_decoder *d) {
    if (d->video->metadata != nullptr) return d->video->metadata.get();
    if (d->video->source != nullptr) {
        d->video->metadata = synthesizeMetadata(*d->video->source);
    }
    return d->video->metadata.get();
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

// Convert ComponentFrame's Y/U/V double samples directly into three
// contiguous float planes covering the active region. Used for
// CHD_PIXEL_YUV444_FLOAT, which bypasses OutputWriter::convert.
//
// The mapping matches chd_frame_copy_plane_float's docstring: Y maps to
// [0.0..1.0] black-to-white; U/V are centred at 0.0 with ±0.5 range.
void componentFrameToFloatPlanes(const chd::output::ComponentFrame &cf,
                                  const chd::metadata::LdDecodeMetaData::VideoParameters &vp,
                                  int32_t activeWidth, int32_t outputHeight,
                                  std::vector<float> *outPlanes) {
    const int32_t firstLine = vp.firstActiveFrameLine;
    const int32_t startX    = vp.activeVideoStart;
    const double  yOff      = vp.black16bIre;
    const double  yRange    = vp.white16bIre - vp.black16bIre;

    for (int32_t p = 0; p < 3; p++) {
        outPlanes[p].assign(static_cast<size_t>(activeWidth) * outputHeight, 0.0f);
    }

    for (int32_t y = 0; y < outputHeight; y++) {
        const int32_t srcLine = firstLine + y;
        if (srcLine < 0 || srcLine >= cf.getHeight()) continue;
        const double *inY = cf.y(srcLine) + startX;
        const double *inU = cf.u(srcLine) + startX;
        const double *inV = cf.v(srcLine) + startX;
        float *outY = outPlanes[0].data() + static_cast<size_t>(y) * activeWidth;
        float *outU = outPlanes[1].data() + static_cast<size_t>(y) * activeWidth;
        float *outV = outPlanes[2].data() + static_cast<size_t>(y) * activeWidth;
        for (int32_t x = 0; x < activeWidth; x++) {
            outY[x] = static_cast<float>((inY[x] - yOff) / yRange);
            outU[x] = static_cast<float>(inU[x] / yRange);
            outV[x] = static_cast<float>(inV[x] / yRange);
        }
    }
}

chd_status_t decodeFrameLocked(chd_decoder_t *d, int64_t frame_index, chd_frame_t **out) {
    auto *meta = resolveMetadata(d);
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

    // Apply dropout correction if requested. Single-source for now; extra
    // sources land in a follow-up that wires sources[i] + their metadata
    // through the multi-source DropoutCorrector::correctFrame overload.
    chd::dropout::DropoutCorrectionStats stats;
    if (d->dropoutOptsSet && d->dropoutOpts.enabled != 0) {
        chd::dropout::DropoutCorrector corrector(d->videoParameters);
        corrector.correctFrame(fields[startIndex], fields[startIndex + 1],
                               d->dropoutOpts.overcorrect != 0,
                               d->dropoutOpts.intra_field_only != 0,
                               &stats);
    }

    std::vector<chd::output::ComponentFrame> componentFrames(1);
    try {
        d->decoder->decodeFrames(fields, startIndex, endIndex, componentFrames);
    } catch (const std::exception &e) {
        chd::detail::set_last_error(std::string("chd_decode_frame: decoder threw: ") + e.what());
        return CHD_E_INTERNAL;
    }

    auto frame = std::make_unique<chd_frame>();
    frame->format = d->outputPixelFormat;

    const int32_t activeWidth =
        d->videoParameters.activeVideoEnd - d->videoParameters.activeVideoStart;

    if (frame->format == CHD_PIXEL_YUV444_FLOAT) {
        const int32_t outputHeight =
            d->videoParameters.lastActiveFrameLine - d->videoParameters.firstActiveFrameLine;
        componentFrameToFloatPlanes(componentFrames[0], d->videoParameters,
                                    activeWidth, outputHeight, frame->floatPlane);
        frame->activeWidth = activeWidth;
        frame->outputHeight = outputHeight;
        frame->info.format = CHD_PIXEL_YUV444_FLOAT;
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

    d->lastDropoutStats.corrected      = stats.corrected;
    d->lastDropoutStats.failed         = stats.failed;
    d->lastDropoutStats.total_distance = stats.totalDistance;

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
    std::lock_guard<std::mutex> lock(d->decodeMutex);
    if (d->committed) return CHD_OK;  // idempotent: re-commit is a no-op

    if (d->video == nullptr || d->video->source == nullptr) {
        chd::detail::set_last_error("chd_decoder_commit: video has no source");
        return CHD_E_INVALID_ARG;
    }

    const chd::metadata::VideoSystem system = d->video->source->parameters().system;
    const chd_decoder_kind_t resolved = chd::decoders::registry::resolveAuto(d->kind, system);

    auto decoder = chd::decoders::registry::build(resolved, d->optionMaps);
    if (!decoder) {
        chd::detail::set_last_error("chd_decoder_commit: unknown or unsupported decoder kind");
        return CHD_E_DECODER_UNKNOWN;
    }

#if defined(CHD_WITH_NN)
    if (d->nnModelPending) {
        if (!chd::decoders::registry::applyNnModel(resolved, *decoder, d->nnModelPending)) {
            chd::detail::set_last_error(
                "chd_decoder_commit: NN model bound to a non-NN decoder kind");
            return CHD_E_DECODER_INCOMPATIBLE;
        }
    } else if (chd::decoders::registry::kindUsesNn(resolved)) {
        chd::detail::set_last_error(
            "chd_decoder_commit: NN decoder kind requires chd_decoder_set_nn_model first");
        return CHD_E_NN_MODEL_LOAD;
    }
#endif

    chd::output::OutputWriter::Configuration outCfg;
    outCfg.paddingAmount = 8;
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

    chd::metadata::LdDecodeMetaData *meta = resolveMetadata(d);
    if (meta == nullptr) {
        chd::detail::set_last_error("chd_decoder_commit: failed to resolve video metadata");
        return CHD_E_METADATA_MISSING;
    }
    chd::metadata::LdDecodeMetaData::VideoParameters vp = meta->getVideoParameters();
    applyLineOverrides(vp, d->optionMaps);

    // OutputWriter::updateConfiguration mutates vp (active region padding);
    // the decoder configures against the same post-padding vp.
    d->outputWriter.updateConfiguration(vp, outCfg);

    if (!decoder->configure(vp)) {
        chd::detail::set_last_error(
            "chd_decoder_commit: decoder rejected input (incompatible video standard?)");
        return CHD_E_DECODER_INCOMPATIBLE;
    }

    // The metadata's VideoParameters stay at their pre-padding values:
    // SourceField::loadFields only reads `isSubcarrierLocked` from it
    // (which padding doesn't touch), and DecoderPool follows the same
    // convention of not writing back the post-padding vp. Keeping the
    // metadata pristine lets multiple decoders share one chd_video with
    // independent padding choices.

    d->resolvedKind = resolved;
    d->decoder = std::move(decoder);
    d->videoParameters = vp;
    d->outputConfig = outCfg;
    d->outputPixelFormat = abiFormat;
    d->lookBehind = d->decoder->getLookBehind();
    d->lookAhead  = d->decoder->getLookAhead();

    {
        auto it = d->optionMaps.i32.find(CHD_OPT_THREAD_COUNT);
        d->threadCount = (it != d->optionMaps.i32.end()) ? it->second : 0;
        if (d->threadCount <= 0) {
            const unsigned hc = std::thread::hardware_concurrency();
            d->threadCount = hc == 0 ? 2 : static_cast<int32_t>(hc);
        }
    }

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
    // Decoder subclasses keep mutable per-call state (FrameBuffer,
    // TransformPal buffers, OutputWriter line scratch) that isn't
    // documented as reentrant. Serialise here; chd_decode_frames_async
    // serialises through the same mutex, so async currently provides API
    // surface rather than true parallelism. Per-worker decoder instances
    // are a follow-up.
    std::lock_guard<std::mutex> lock(d->decodeMutex);
    return decodeFrameLocked(d, frame_index, out);
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

    std::vector<std::future<void>> tasks;
    tasks.reserve(n);
    for (size_t i = 0; i < n; i++) {
        const int64_t idx = indices[i];
        tasks.emplace_back(std::async(std::launch::async, [d, idx, cb, user, cancel]() {
            if (cancel != nullptr && cancel->requested.load(std::memory_order_acquire)) {
                cb(user, CHD_E_CANCELLED, idx, nullptr);
                return;
            }
            chd_frame_t *frame = nullptr;
            chd_status_t rc;
            {
                std::lock_guard<std::mutex> lock(d->decodeMutex);
                if (cancel != nullptr && cancel->requested.load(std::memory_order_acquire)) {
                    cb(user, CHD_E_CANCELLED, idx, nullptr);
                    return;
                }
                rc = decodeFrameLocked(d, idx, &frame);
            }
            cb(user, rc, idx, frame);
        }));
    }
    for (auto &t : tasks) t.get();
    return CHD_OK;
}

}  // extern "C"
