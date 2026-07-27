// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shared helpers for the NN tests: locating a loadable ONNX model and reading
// its bytes. No model paths are hardcoded anywhere. Real weights (which are
// large and separately licensed; see models/README.md) are supplied only via
// environment variables, and every lookup falls back to a committed synthetic
// fixture, so the load/bind tests run unconditionally in CI and for every
// contributor without shipping real weights.

#ifndef CHD_TEST_NN_TEST_MODEL_H
#define CHD_TEST_NN_TEST_MODEL_H

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace chd_test {

// Absolute path to the committed synthetic fixture — a structurally valid but
// meaningless ONNX graph (single Identity node). CHD_TEST_FIXTURE_DIR is
// defined by the build to the tests/fixtures directory; returns empty when the
// build didn't define it.
inline std::string syntheticFixture() {
#ifdef CHD_TEST_FIXTURE_DIR
    return std::string(CHD_TEST_FIXTURE_DIR) + "/tiny_identity.onnx";
#else
    return {};
#endif
}

// Resolve a model path for a test. If `envVar` is set, non-empty, and points
// to an existing file, that file is used — this is the ONLY way real weights
// are supplied; no paths are baked in. Otherwise falls back to the committed
// synthetic fixture. Returns empty only when neither is available (the caller
// then skips).
inline std::string modelFromEnvOrFixture(const char *envVar) {
    if (const char *p = std::getenv(envVar); p && *p) {
        if (std::filesystem::exists(p)) return p;
    }
    const std::string fixture = syntheticFixture();
    if (!fixture.empty() && std::filesystem::exists(fixture)) return fixture;
    return {};
}

// Execution provider a test should ask for, from CHD_TEST_NN_BACKEND (a
// chd_nn_backend_t value). Returns 0 (CHD_NN_BACKEND_AUTO) when unset, which is
// what every contributor and hosted runner gets.
//
// Asserting the provider that AUTO happened to pick is not enough for the GPU
// jobs: the auto chain stops at the first one that attaches, so on a box with
// several available EPs the later ones are never exercised. CI pins the one the
// job exists to validate, and pairs this with CHD_TEST_EXPECT_NN_BACKEND to
// assert it really attached.
inline int backendFromEnv() {
    if (const char *b = std::getenv("CHD_TEST_NN_BACKEND"); b && *b) {
        return std::atoi(b);
    }
    return 0;
}

// Read an entire file into a byte buffer, for the in-memory model loaders.
inline std::vector<char> readFileBytes(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
}

}  // namespace chd_test

#endif  // CHD_TEST_NN_TEST_MODEL_H
