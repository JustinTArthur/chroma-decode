// SPDX-License-Identifier: GPL-3.0-or-later
//
// Smoke test: instantiate NtscDecoder with nnTransform3D=true,
// bind a real chroma_net ONNX session, configure for NTSC, and verify the
// wiring (look-ahead bump, configure success, no crash). Full frame-decode
// validation against a golden reference is deferred to a follow-up once an
// encode-orc fixture pipeline exists.
//
// Skips gracefully when:
//   - the build was made without NN support (chd_has_feature("nn") == 0)
//   - the chroma_net model file isn't present at the expected paths

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
// Internal headers — we link the static lib for test access (matches the
// existing test_encode_orc_color_bars pattern).
#include "../../src/decoders/comb/comb.h"
#include "../../src/decoders/comb/ntsc_decoder.h"
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

// Candidate paths for the chroma_net v1/v2 model. The first existing one
// wins. The user's machine has the asdfqazsnbb harness at this location
// per the project's reference-repos memory; the tbc-tools build also
// copies the v1 model into its source tree.
std::string findChromaNetModel() {
    const std::string candidates[] = {
        std::string(std::getenv("HOME") ? std::getenv("HOME") : "") +
            "/Development/Analog Decoding Models/nnTransform3D/nnTransform3D/chroma_net.onnx",
        std::string(std::getenv("HOME") ? std::getenv("HOME") : "") +
            "/Development/Analog Decoding Models/nnTransform3D/Repos/nnTransform3D/chroma_net.onnx",
        std::string(std::getenv("HOME") ? std::getenv("HOME") : "") +
            "/Development/Repos/tbc-tools/src/ld-chroma-decoder/chroma_net.onnx",
    };
    for (const auto &candidate : candidates) {
        if (fs::exists(candidate)) return candidate;
    }
    return {};
}

int testLookAheadBump() {
    using namespace chd::decoders::comb;
    Comb::Configuration cfg;
    cfg.dimensions = 3;
    cfg.nnTransform3D = false;
    REQUIRE(cfg.getLookAhead() == 1);
    REQUIRE(cfg.getLookBehind() == 1);

    cfg.nnTransform3D = true;
    // nnTransform3D walks tiles across the frame boundary, so it needs
    // one *additional* future frame for overlap-add.
    REQUIRE(cfg.getLookAhead() == 2);
    REQUIRE(cfg.getLookBehind() == 1);
    return 0;
}

int testConfigureWithoutSession() {
    using namespace chd::decoders::comb;
    Comb::Configuration cfg;
    cfg.dimensions = 3;
    cfg.nnTransform3D = true;
    cfg.nnInputMagnitudeScale = 128.0;

    NtscDecoder decoder(cfg);
    chd::metadata::LdDecodeMetaData::VideoParameters vp;
    vp.system = chd::metadata::NTSC;
    vp.fieldWidth = 910;
    vp.fieldHeight = 263;
    vp.fSC = 315.0e6 / 88.0;
    vp.sampleRate = vp.fSC * 4.0;
    vp.activeVideoStart = 134;
    vp.activeVideoEnd = 894;
    vp.firstActiveFrameLine = 40;
    vp.lastActiveFrameLine = 525;
    vp.black16bIre = 16128;
    vp.white16bIre = 51200;
    vp.blanking16bIre = 15360;
    vp.isValid = true;

    // Configure without binding a session: should still succeed.
    // decodeFrames would fall back to 2D (we don't exercise it here
    // because nnTransform3D=true requires real input frames for the
    // unique pathway).
    REQUIRE(decoder.configure(vp));
    REQUIRE(decoder.getLookAhead() == 2);
    return 0;
}

int testConfigureWithSession() {
    const std::string modelPath = findChromaNetModel();
    if (modelPath.empty()) {
        std::cout << "Skipping nnTransform3D session test (no chroma_net.onnx found).\n";
        return 0;
    }

    chd::nn::SessionOptions opts;
    std::shared_ptr<chd::nn::OrtSession> session;
    try {
        session = std::make_shared<chd::nn::OrtSession>(modelPath, opts);
    } catch (const std::exception &e) {
        std::cerr << "FAIL: OrtSession construction failed: " << e.what() << "\n";
        return 1;
    }
    REQUIRE(session != nullptr);

    using namespace chd::decoders::comb;
    Comb::Configuration cfg;
    cfg.dimensions = 3;
    cfg.nnTransform3D = true;
    cfg.nnInputMagnitudeScale = 128.0;

    NtscDecoder decoder(cfg);
    decoder.setNnModel(session);

    chd::metadata::LdDecodeMetaData::VideoParameters vp;
    vp.system = chd::metadata::NTSC;
    vp.fieldWidth = 910;
    vp.fieldHeight = 263;
    vp.fSC = 315.0e6 / 88.0;
    vp.sampleRate = vp.fSC * 4.0;
    vp.activeVideoStart = 134;
    vp.activeVideoEnd = 894;
    vp.firstActiveFrameLine = 40;
    vp.lastActiveFrameLine = 525;
    vp.black16bIre = 16128;
    vp.white16bIre = 51200;
    vp.blanking16bIre = 15360;
    vp.isValid = true;
    REQUIRE(decoder.configure(vp));
    REQUIRE(decoder.getLookAhead() == 2);

    std::cout << "nnTransform3D session bound from " << modelPath << "\n";
    return 0;
}

#endif  // CHD_TEST_NO_ONNXRUNTIME

}  // namespace

int main() {
    if (chd_has_feature("nn") != 1) {
        std::cout << "Skipping nnTransform3D tests (with_nn=false).\n";
        return 0;
    }
#ifdef CHD_TEST_NO_ONNXRUNTIME
    std::cout << "Skipping nnTransform3D tests (no ORT headers visible at test compile).\n";
    return 0;
#else
    int rc = 0;
    rc |= testLookAheadBump();
    rc |= testConfigureWithoutSession();
    rc |= testConfigureWithSession();
    // Tear down the Ort::Env explicitly so its destructor runs before
    // C++ static destruction (the same destruction-order hazard as
    // test_nn_framework).
    chd_shutdown();
    if (rc == 0) std::cout << "All nnTransform3D wiring tests passed.\n";
    return rc;
#endif
}
