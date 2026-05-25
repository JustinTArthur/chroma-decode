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

#include <memory>
#include <string>
#include <vector>

#include "../metadata/core.h"
#include "../reader/source.h"

// chd_nn_model is only populated when the build has NN support (the C ABI
// rejects loads cleanly when with_nn=false). Forward-declare the wrapper so
// translation units that don't pull in ORT headers can still compile.
#if defined(CHD_WITH_NN)
#include "../nn/ort_session.h"
#endif

extern "C" {
struct chd_video {
    std::unique_ptr<chd::metadata::LdDecodeMetaData> metadata;
    std::unique_ptr<chd::reader::ISource> source;
    std::string tbcPath;
    std::vector<std::unique_ptr<chd::reader::ISource>> extraSources;
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
