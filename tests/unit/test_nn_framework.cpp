// SPDX-License-Identifier: GPL-3.0-or-later
//
// Smoke tests for the NN framework — Ort::Env singleton,
// provider availability, and error handling on the chd_nn_model_load_from_file
// and chd_nn_model_load_from_memory paths. Does NOT exercise inference on a
// real model; that lands in a
// follow-up with the dummy ONNX model fixture once nnTransform3D / ldzeug2
// have something to feed.
//
// The test compiles into a no-op pass when the library was built with
// with_nn=false (chd_has_feature("nn") returns 0 → we skip the body).

#include <chromadec/errors.h>
#include <chromadec/nn.h>
#include <chromadec/version.h>
#include <chromadec/video.h>

#include "nn_test_model.h"

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

#define REQUIRE(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #cond \
                  << " (last_error: " << (chd_last_error() ? chd_last_error() : "(none)") << ")\n"; \
        return 1; \
    } \
} while (0)

int testLifecycle() {
    // chd_init must be idempotent + safe to call multiple times.
    REQUIRE(chd_init() == CHD_OK);
    REQUIRE(chd_init() == CHD_OK);
    chd_shutdown();
    // After shutdown the env reconstructs lazily — should be safe to
    // re-init + reuse.
    REQUIRE(chd_init() == CHD_OK);
    chd_shutdown();
    return 0;
}

int testFeatureFlag() {
    // "nn" feature must report whatever the build said. The test only runs
    // body assertions when NN is on (we're built against ORT here).
    REQUIRE(chd_has_feature("nn") == 1);
    REQUIRE(chd_has_feature("sqlite") == 1);
    REQUIRE(chd_has_feature("nonsense-feature") == 0);
    REQUIRE(chd_has_feature(nullptr) == 0);
    return 0;
}

int testProviderAvailability() {
    // CPU is always available.
    REQUIRE(chd_nn_provider_is_available(CHD_NN_EP_CPU) == 1);
    // AUTO should report available (always — it's not a concrete provider).
    REQUIRE(chd_nn_provider_is_available(CHD_NN_EP_AUTO) == 1);

#if defined(__APPLE__)
    // CoreML EP should be present in any current macOS ORT build (Homebrew
    // 1.26.0 bundles it).
    REQUIRE(chd_nn_provider_is_available(CHD_NN_EP_COREML) == 1);
    // The DirectML / TensorRT / MIGraphX providers are Windows-/Linux-only
    // and absent on macOS.
    REQUIRE(chd_nn_provider_is_available(CHD_NN_EP_DIRECTML) == 0);
    REQUIRE(chd_nn_provider_is_available(CHD_NN_EP_MIGRAPHX) == 0);
#endif
    return 0;
}

int testSessionOptsDefault() {
    chd_nn_session_opts_t opts{};
    chd_nn_session_opts_default(&opts);
    REQUIRE(opts.provider           == CHD_NN_EP_AUTO);
    REQUIRE(opts.device_id          == 0);
    REQUIRE(opts.enable_graph_optim == 1);
    REQUIRE(opts.enable_mem_pattern == 1);
    REQUIRE(opts.intra_op_threads   == 1);
    return 0;
}

int testLoadNonexistentModel() {
    // Passing a path that doesn't exist must return CHD_E_NN_MODEL_LOAD
    // (or CHD_E_INVALID_ARG for the null cases), never crash, and never
    // leak the out pointer.
    chd_nn_model_t *m = nullptr;
    REQUIRE(chd_nn_model_load_from_file(nullptr, nullptr, &m) == CHD_E_INVALID_ARG);
    REQUIRE(m == nullptr);

    REQUIRE(chd_nn_model_load_from_file("/nonexistent/path/to/model.onnx", nullptr, &m)
            == CHD_E_NN_MODEL_LOAD);
    REQUIRE(m == nullptr);
    // The error detail must be informative.
    const char *err = chd_last_error();
    REQUIRE(err != nullptr);
    REQUIRE(std::strstr(err, "chd_nn_model_load_from_file") != nullptr);
    return 0;
}

int testLoadFromMemoryInvalid() {
    // The in-memory entry point must reject null/empty buffers and the null
    // out pointer with CHD_E_INVALID_ARG, never crash, never leak. Garbage
    // (non-ONNX) bytes must fail the load cleanly with CHD_E_NN_MODEL_LOAD.
    chd_nn_model_t *m = nullptr;
    const unsigned char dummy[4] = { 0, 1, 2, 3 };

    REQUIRE(chd_nn_model_load_from_memory(nullptr, 4, nullptr, &m) == CHD_E_INVALID_ARG);
    REQUIRE(m == nullptr);
    REQUIRE(chd_nn_model_load_from_memory(dummy, 0, nullptr, &m) == CHD_E_INVALID_ARG);
    REQUIRE(m == nullptr);
    REQUIRE(chd_nn_model_load_from_memory(dummy, 4, nullptr, nullptr) == CHD_E_INVALID_ARG);

    // Well-formed call, but the bytes aren't a valid ONNX model: ORT must
    // reject them and we surface CHD_E_NN_MODEL_LOAD with an informative msg.
    REQUIRE(chd_nn_model_load_from_memory(dummy, sizeof(dummy), nullptr, &m)
            == CHD_E_NN_MODEL_LOAD);
    REQUIRE(m == nullptr);
    const char *err = chd_last_error();
    REQUIRE(err != nullptr);
    REQUIRE(std::strstr(err, "chd_nn_model_load_from_memory") != nullptr);
    return 0;
}

int testLoadRealModel() {
    // Drive the PUBLIC C ABI loaders against a real ONNX model — both the
    // file and in-memory entry points must succeed, produce a usable handle,
    // and resolve the same execution provider (same model + same default
    // opts). This is the happy-path counterpart to the failure-only checks
    // above, and the only place the public chd_nn_model_load_from_memory
    // symbol is exercised against real weights. Skips when no model is
    // discoverable on this machine.
    const std::string modelPath = chd_test::modelFromEnvOrFixture("CHD_TEST_NN_MODEL");
    if (modelPath.empty()) {
        std::cout << "Skipping real-model load test (no model fixture available).\n";
        return 0;
    }

    // File loader.
    chd_nn_model_t *fileModel = nullptr;
    REQUIRE(chd_nn_model_load_from_file(modelPath.c_str(), nullptr, &fileModel) == CHD_OK);
    REQUIRE(fileModel != nullptr);
    chd_nn_provider_t fileProvider = CHD_NN_EP_AUTO;
    REQUIRE(chd_nn_model_get_active_provider(fileModel, &fileProvider) == CHD_OK);

    // Memory loader: same bytes, via the in-memory entry point.
    std::vector<char> bytes = chd_test::readFileBytes(modelPath);
    REQUIRE(!bytes.empty());

    chd_nn_model_t *memModel = nullptr;
    REQUIRE(chd_nn_model_load_from_memory(bytes.data(), bytes.size(), nullptr, &memModel)
            == CHD_OK);
    REQUIRE(memModel != nullptr);
    chd_nn_provider_t memProvider = CHD_NN_EP_AUTO;
    REQUIRE(chd_nn_model_get_active_provider(memModel, &memProvider) == CHD_OK);

    // Identical model + opts must resolve to the same provider regardless of
    // how the bytes were sourced.
    REQUIRE(memProvider == fileProvider);

    chd_nn_model_free(memModel);
    chd_nn_model_free(fileModel);
    std::cout << "Loaded real model via file + memory (" << bytes.size()
              << " bytes), provider " << static_cast<int>(memProvider) << ".\n";
    return 0;
}

int testFreeNullHandle() {
    // free(nullptr) must be a no-op (matches free() / delete semantics).
    chd_nn_model_free(nullptr);
    return 0;
}

int testGetActiveProviderNull() {
    chd_nn_provider_t p = CHD_NN_EP_AUTO;
    REQUIRE(chd_nn_model_get_active_provider(nullptr, &p) == CHD_E_INVALID_ARG);
    return 0;
}

}  // namespace

int main() {
    // Bail with a pass if the build doesn't include NN; the test still
    // confirms the feature flag works.
    if (chd_has_feature("nn") != 1) {
        std::cout << "Skipping NN tests (with_nn=false).\n";
        return 0;
    }
    int rc = 0;
    rc |= testLifecycle();
    rc |= testFeatureFlag();
    rc |= testProviderAvailability();
    rc |= testSessionOptsDefault();
    rc |= testLoadNonexistentModel();
    rc |= testLoadFromMemoryInvalid();
    rc |= testLoadRealModel();
    rc |= testFreeNullHandle();
    rc |= testGetActiveProviderNull();
    // Tear down the Ort::Env explicitly so its destructor runs before C++
    // static destruction reaches ORT's internal globals. Without this, the
    // test process aborts on exit with "mutex lock failed: Invalid argument"
    // because ORT's own static destructors race with the C++ runtime's. This
    // is exactly the destruction-order hazard noted above — chd_shutdown() is
    // intentionally opt-in.
    chd_shutdown();
    if (rc == 0) std::cout << "All NN framework tests passed.\n";
    return rc;
}
