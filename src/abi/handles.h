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
#include "../reader/tbc_source.h"

extern "C" {
struct chd_video {
    std::unique_ptr<chd::metadata::LdDecodeMetaData> metadata;
    std::unique_ptr<chd::reader::SourceVideo> source;
    std::string tbcPath;
    std::vector<std::unique_ptr<chd::reader::SourceVideo>> extraSources;
};
}  // extern "C"
