// SPDX-License-Identifier: GPL-3.0-or-later
//
// Smoke test: instantiate the ldzeug2 NN decoders, bind real
// ONNX sessions, configure for NTSC, and verify the wiring. Like the
// nnTransform3D test, full per-frame decode validation against a golden
// reference is deferred to a follow-up.
//
// Skips gracefully when:
//   - the build was made without NN support
//   - the bundled model files aren't present at the expected paths.

#include <chromadec/version.h>
#include <chromadec/video.h>   // chd_shutdown

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

namespace fs = std::filesystem;

#if defined(__has_include)
#  if !__has_include(<onnxruntime_cxx_api.h>)
#    define CHD_TEST_NO_ONNXRUNTIME
#  endif
#endif

#ifndef CHD_TEST_NO_ONNXRUNTIME
#include "../../src/decoders/ldzeug/ldzeug_base.h"
#include "../../src/decoders/ldzeug/ldzeug_color_cnn.h"
#include "../../src/decoders/ldzeug/ldzeug_luma_sep.h"
#include "../../src/metadata/core.h"
#include "../../src/nn/ort_session.h"
#endif

namespace {

#define REQUIRE(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #cond "\n"; \
        return 1; \
    } \
} while (0)

#ifndef CHD_TEST_NO_ONNXRUNTIME

// jsaowji's bundled weights, mirrored on the user's machine per
// reference-repos memory:
//   color_cnn:        color_cnn_v2_alot.onnx (or _1031640 / _denoise variant)
//   luma_sep field:   luma_sep_2dgray_fields.onnx
//   luma_sep frame:   luma_sep_2d_frame_gray_gray_run2_latest.onnx
std::string ldzeugModelDir() {
    const char *home = std::getenv("HOME");
    if (home == nullptr) return {};
    return std::string(home) + "/Development/Analog Decoding Models/for ldzeug2";
}

std::string findFirst(const std::vector<std::string> &candidates) {
    for (const auto &c : candidates) {
        if (fs::exists(c)) return c;
    }
    return {};
}

chd::metadata::LdDecodeMetaData::VideoParameters makeNtscVp() {
    chd::metadata::LdDecodeMetaData::VideoParameters vp;
    vp.system = chd::metadata::NTSC;
    vp.fieldWidth  = 910;
    vp.fieldHeight = 263;
    vp.fSC = 315.0e6 / 88.0;
    vp.sampleRate = vp.fSC * 4.0;
    vp.activeVideoStart = 134;
    vp.activeVideoEnd   = 894;
    vp.firstActiveFrameLine = 40;
    vp.lastActiveFrameLine  = 525;
    vp.black16bIre    = 16128;
    vp.white16bIre    = 51200;
    vp.blanking16bIre = 15360;
    vp.isValid = true;
    return vp;
}

int testColorCnnConfigure() {
    using namespace chd::decoders::ldzeug;
    const std::string model = findFirst({
        ldzeugModelDir() + "/color_cnn_v2_alot.onnx",
        ldzeugModelDir() + "/color_cnn_1031640.onnx",
        ldzeugModelDir() + "/color_cnn_denoise_613928_ft22k.onnx",
    });
    if (model.empty()) {
        std::cout << "Skipping LdzeugColorCnn test (no color_cnn_*.onnx found).\n";
        return 0;
    }
    chd::nn::SessionOptions opts;
    std::shared_ptr<chd::nn::OrtSession> session;
    try {
        session = std::make_shared<chd::nn::OrtSession>(model, opts);
    } catch (const std::exception &e) {
        std::cerr << "FAIL: OrtSession(color_cnn) construction failed: " << e.what() << "\n";
        return 1;
    }
    REQUIRE(session != nullptr);

    LdzeugColorCnnDecoder decoder;
    decoder.setNnModel(session);
    decoder.setMode(LdzeugDecoderBase::Mode::Field);
    decoder.setChromaPhase(0.0);
    decoder.setChromaGain(1.0);

    REQUIRE(decoder.configure(makeNtscVp()));
    // ldzeug decoders are per-field/frame and need no temporal context.
    REQUIRE(decoder.getLookBehind() == 0);
    REQUIRE(decoder.getLookAhead()  == 0);

    std::cout << "LdzeugColorCnn bound from " << model << "\n";
    return 0;
}

int testLumaSepConfigure() {
    using namespace chd::decoders::ldzeug;
    const std::string fieldModel = ldzeugModelDir() + "/luma_sep_2dgray_fields.onnx";
    const std::string frameModel = ldzeugModelDir() + "/luma_sep_2d_frame_gray_gray_run2_latest.onnx";

    if (fs::exists(fieldModel)) {
        chd::nn::SessionOptions opts;
        auto session = std::make_shared<chd::nn::OrtSession>(fieldModel, opts);
        LdzeugLumaSepDecoder decoder;
        decoder.setNnModel(session);
        decoder.setMode(LdzeugDecoderBase::Mode::Field);
        REQUIRE(decoder.configure(makeNtscVp()));
        REQUIRE(decoder.getLookAhead() == 0);
        // chromaBandpass should default to true (matches ldzeug2 default).
        decoder.setChromaBandpass(false);
        decoder.setChromaBandpass(true);
        std::cout << "LdzeugLumaSep (field) bound from " << fieldModel << "\n";
    } else {
        std::cout << "Skipping LdzeugLumaSep field test (no luma_sep_2dgray_fields.onnx).\n";
    }

    if (fs::exists(frameModel)) {
        chd::nn::SessionOptions opts;
        auto session = std::make_shared<chd::nn::OrtSession>(frameModel, opts);
        LdzeugLumaSepDecoder decoder;
        decoder.setNnModel(session);
        decoder.setMode(LdzeugDecoderBase::Mode::Frame);
        REQUIRE(decoder.configure(makeNtscVp()));
        std::cout << "LdzeugLumaSep (frame) bound from " << frameModel << "\n";
    } else {
        std::cout << "Skipping LdzeugLumaSep frame test (no luma_sep_*_frame_*.onnx).\n";
    }

    return 0;
}

int testNoSessionRejected() {
    using namespace chd::decoders::ldzeug;
    LdzeugColorCnnDecoder color;
    REQUIRE(color.configure(makeNtscVp()));
    std::vector<chd::decoders::SourceField> inputs;
    std::vector<chd::output::ComponentFrame> outputs;
    bool threw = false;
    try {
        color.decodeFrames(inputs, 0, 0, outputs);
    } catch (const std::runtime_error &) {
        threw = true;
    }
    REQUIRE(threw);

    LdzeugLumaSepDecoder lumaSep;
    REQUIRE(lumaSep.configure(makeNtscVp()));
    threw = false;
    try {
        lumaSep.decodeFrames(inputs, 0, 0, outputs);
    } catch (const std::runtime_error &) {
        threw = true;
    }
    REQUIRE(threw);
    return 0;
}

int testRejectsNonNtsc() {
    using namespace chd::decoders::ldzeug;
    chd::metadata::LdDecodeMetaData::VideoParameters palVp = makeNtscVp();
    palVp.system = chd::metadata::PAL;

    LdzeugColorCnnDecoder color;
    REQUIRE(!color.configure(palVp));

    LdzeugLumaSepDecoder lumaSep;
    REQUIRE(!lumaSep.configure(palVp));
    return 0;
}

#endif  // CHD_TEST_NO_ONNXRUNTIME

}  // namespace

int main() {
    if (chd_has_feature("nn") != 1) {
        std::cout << "Skipping ldzeug tests (with_nn=false).\n";
        return 0;
    }
#ifdef CHD_TEST_NO_ONNXRUNTIME
    std::cout << "Skipping ldzeug tests (no ORT headers visible at test compile).\n";
    return 0;
#else
    int rc = 0;
    rc |= testNoSessionRejected();
    rc |= testRejectsNonNtsc();
    rc |= testColorCnnConfigure();
    rc |= testLumaSepConfigure();
    // Tear down the Ort::Env explicitly before C++ static destruction
    // (avoids the static-destruction-order hazard).
    chd_shutdown();
    if (rc == 0) std::cout << "All ldzeug wiring tests passed.\n";
    return rc;
#endif
}
