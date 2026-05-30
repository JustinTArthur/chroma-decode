// SPDX-License-Identifier: GPL-3.0-or-later
//
// Smoke test: instantiate NtscDecoder with nnTransform3D=true, bind an ONNX
// session (the synthetic test fixture by default, or real chroma_net weights
// when $CHD_TEST_NN_MODEL points at them), configure for NTSC, and verify the
// wiring (look-ahead bump, configure success, no crash) — both when the
// session is built from a file path and from an in-memory buffer. No inference
// is run, so the model's contents don't matter. Full frame-decode validation
// against a golden reference is deferred to a follow-up once an encode-orc
// fixture pipeline exists.
//
// Skips gracefully when the build was made without NN support
// (chd_has_feature("nn") == 0), or, in the unlikely case the committed
// fixture is missing, when no model can be located at all.

#include <chromadec/version.h>
#include <chromadec/video.h>   // chd_shutdown

#include "nn_test_model.h"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

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

// Build an nnTransform3D NtscDecoder, bind `session`, configure it for NTSC,
// and assert the wiring held (look-ahead bump + configure success). Shared by
// the file- and memory-load session tests so they verify identical behaviour
// regardless of how the session's model bytes were sourced.
int bindSessionAndConfigure(const std::shared_ptr<chd::nn::OrtSession> &session) {
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
    return 0;
}

int testConfigureWithSession() {
    const std::string modelPath = chd_test::modelFromEnvOrFixture("CHD_TEST_NN_MODEL");
    if (modelPath.empty()) {
        std::cout << "Skipping nnTransform3D session test (no model fixture available).\n";
        return 0;
    }

    chd::nn::SessionOptions opts;
    std::shared_ptr<chd::nn::OrtSession> session;
    try {
        session = std::make_shared<chd::nn::OrtSession>(modelPath, opts);
    } catch (const std::exception &e) {
        std::cerr << "FAIL: OrtSession construction from file failed: " << e.what() << "\n";
        return 1;
    }
    REQUIRE(session != nullptr);

    if (int rc = bindSessionAndConfigure(session)) return rc;

    std::cout << "nnTransform3D session bound from file " << modelPath << "\n";
    return 0;
}

int testConfigureWithMemorySession() {
    const std::string modelPath = chd_test::modelFromEnvOrFixture("CHD_TEST_NN_MODEL");
    if (modelPath.empty()) {
        std::cout << "Skipping nnTransform3D in-memory session test (no model fixture available).\n";
        return 0;
    }

    // Read the same model file into a buffer and construct the session from
    // memory, exercising the in-memory OrtSession constructor (the one the C
    // ABI's chd_nn_model_load_from_memory forwards to) end-to-end against a
    // real model — not just the failure paths covered in test_nn_framework.
    std::vector<char> bytes = chd_test::readFileBytes(modelPath);
    REQUIRE(!bytes.empty());

    chd::nn::SessionOptions opts;
    std::shared_ptr<chd::nn::OrtSession> session;
    try {
        session = std::make_shared<chd::nn::OrtSession>(bytes.data(), bytes.size(), opts);
    } catch (const std::exception &e) {
        std::cerr << "FAIL: OrtSession construction from memory failed: " << e.what() << "\n";
        return 1;
    }
    REQUIRE(session != nullptr);

    if (int rc = bindSessionAndConfigure(session)) return rc;

    std::cout << "nnTransform3D session bound from memory (" << bytes.size()
              << " bytes) via " << modelPath << "\n";
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
    rc |= testConfigureWithMemorySession();
    // Tear down the Ort::Env explicitly so its destructor runs before
    // C++ static destruction (the same destruction-order hazard as
    // test_nn_framework).
    chd_shutdown();
    if (rc == 0) std::cout << "All nnTransform3D wiring tests passed.\n";
    return rc;
#endif
}
