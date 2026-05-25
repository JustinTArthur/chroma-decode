// SPDX-License-Identifier: GPL-3.0-or-later
//
// Concrete layouts for the opaque ABI handle types declared in
// <chromadec/types.h>. The public header forward-declares
// `struct chd_video`, `struct chd_decoder`, etc.; this header gives
// those structs their internal C++ definition. Internal code in
// src/abi/*.cpp includes this to translate between the C ABI and the
// internal C++ classes.
//
// Public callers never see these definitions — they only ever hold
// pointers to the forward-declared opaque types.

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <chromadec/decoder.h>
#include <chromadec/dropout.h>
#include <chromadec/frame.h>

#include "../decoders/decoder_base.h"
#include "../decoders/registry.h"
#include "../metadata/core.h"
#include "../output/output_writer.h"
#include "../reader/source.h"

// chd_nn_model is only populated when the build has NN support (the C ABI
// rejects loads cleanly when with_nn=false). Forward-declare the wrapper so
// translation units that don't pull in ORT headers can still compile.
#if defined(CHD_WITH_NN)
#include "../nn/ort_session.h"
#endif

// One extra source for multi-source dropout correction. Each extra
// carries its own ISource AND its own LdDecodeMetaData — the multi-source
// DropoutCorrector::correctFrame overload needs per-source Field metadata
// (dropouts + bPSNR) which the source itself doesn't carry. For TBC
// extras the metadata is loaded from the .db sidecar; for CVBS extras
// it's synthesized from the source's parameters() + field count.
struct chd_video_extra {
    std::unique_ptr<chd::reader::ISource> source;
    std::unique_ptr<chd::metadata::LdDecodeMetaData> metadata;
    bool metadataSynthesized = false;
};

extern "C" {
struct chd_video {
    // Primary source. `metadata` is always non-null after a successful
    // chd_video_open_*; metadataSynthesized==false means it was loaded
    // from a TBC sqlite/json sidecar, true means it was synthesized in
    // memory from the CVBS source's ISource::parameters() + field count.
    // chd_video_get_info uses the flag to decide whether to report
    // CHD_ENC_CVBS_U16_4FSC or the actual CVBS sample encoding.
    std::unique_ptr<chd::metadata::LdDecodeMetaData> metadata;
    std::unique_ptr<chd::reader::ISource> source;
    std::string tbcPath;
    bool metadataSynthesized = false;
    std::vector<chd_video_extra> extraSources;
};

// Per-decoder state. The lifecycle has two phases:
//   1. Uncommitted: allocated by chd_decoder_create; options are stashed
//      into the optionMaps + nnModelPending; commit() has not run yet so
//      `decoder` and `committed` are unset.
//   2. Committed: chd_decoder_commit has built the concrete Decoder,
//      configured it, snapshotted the post-padding VideoParameters, and
//      wired up the OutputWriter. From here on, set_option_* + set_nn_model
//      reject changes (commit is one-shot per the public header).
struct chd_decoder {
    chd_video_t *video = nullptr;        // non-owning back-pointer
    chd_decoder_kind_t kind = CHD_DEC_AUTO;

    // Pending options stashed by chd_decoder_set_option_*. Drained on
    // commit; left around afterwards in case future surfaces want
    // to inspect what the caller asked for.
    chd::decoders::registry::OptionMaps optionMaps;
#if defined(CHD_WITH_NN)
    std::shared_ptr<chd::nn::OrtSession> nnModelPending;
#endif

    bool dropoutOptsSet = false;
    chd_dropout_opts_t dropoutOpts{};

    // Per-frame dropout stats from the most recent chd_decode_frame call.
    // Guarded by decodeMutex when multiple threads call chd_decode_frame
    // concurrently on the same handle (the API permits this).
    chd_dropout_stats_t lastDropoutStats{};

    // Set by commit. Once true, the fields below are populated and the
    // decode entry points work.
    bool committed = false;

    chd_decoder_kind_t resolvedKind = CHD_DEC_AUTO;
    std::unique_ptr<chd::decoders::Decoder> decoder;

    // Configured pipeline state. videoParameters is the post-padding
    // copy from OutputWriter::updateConfiguration (which mutates the
    // active-region bounds when padding > 1).
    chd::metadata::LdDecodeMetaData::VideoParameters videoParameters{};
    chd::output::OutputWriter outputWriter;
    chd::output::OutputWriter::Configuration outputConfig{};
    chd_pixel_format_t outputPixelFormat = CHD_PIXEL_YUV444P16;
    int32_t threadCount = 0;

    int32_t lookBehind = 0;
    int32_t lookAhead  = 0;

    // Serialises commit() vs chd_decode_frame, and serialises updates to
    // lastDropoutStats. The Decoder subclasses themselves are NOT
    // documented as thread-safe (each owns mutable per-frame state), so
    // chd_decode_frame holds this around the actual decodeFrames call too.
    std::mutex decodeMutex;
};

// chd_frame owns the rendered pixel data for one decoded frame. The format
// field follows the configured output pixel format from chd_decoder_commit;
// for u16 formats `u16Plane` holds the OutputWriter convert() output, and
// for CHD_PIXEL_YUV444_FLOAT the three `floatPlane`s hold contiguous
// float planes converted directly from the decoder's ComponentFrame.
struct chd_frame {
    chd_frame_info_t info{};
    chd_pixel_format_t format = CHD_PIXEL_YUV444P16;

    // YUV444P16 / RGB48 / GRAY16: packed/planar as written by
    // chd::output::OutputWriter::convert. activeWidth/outputHeight match
    // info.width / info.height (post-padding).
    chd::output::OutputFrame u16Plane;

    // YUV444_FLOAT: three contiguous planes, each width*height floats.
    std::vector<float> floatPlane[3];

    int32_t activeWidth  = 0;
    int32_t outputHeight = 0;
};

struct chd_cancel {
    std::atomic<bool> requested{false};
};

#if defined(CHD_WITH_NN)
struct chd_nn_model {
    std::shared_ptr<chd::nn::OrtSession> session;
};
#else
struct chd_nn_model {
    // Placeholder so the opaque-handle type still has a definition when NN
    // is disabled at build time. Any call into chd_nn_* returns
    // CHD_E_INTERNAL via the c_api_nn.cpp stubs.
    int unused;
};
#endif
}  // extern "C"
