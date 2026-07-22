// SPDX-License-Identifier: GPL-3.0-or-later
#include "error_state.h"

namespace chd::detail {

namespace {
thread_local std::string g_last_error;
}

void set_last_error(std::string msg) {
    g_last_error = std::move(msg);
}

const std::string& get_last_error() {
    return g_last_error;
}

void clear_last_error() {
    g_last_error.clear();
}

std::string detail_or(const std::string &fallback) {
    return g_last_error.empty() ? fallback : g_last_error;
}

}  // namespace chd::detail
