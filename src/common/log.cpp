// SPDX-License-Identifier: GPL-3.0-or-later
#include "log.h"

#include <iostream>

namespace chd::log {

namespace {
bool g_debug_enabled = false;
}

bool isDebugEnabled() { return g_debug_enabled; }
void setDebugEnabled(bool enabled) { g_debug_enabled = enabled; }

Stream::Stream(Level level) : level_(level) {
    if (level_ == Level::Debug && !g_debug_enabled) {
        active_ = false;
    }
}

Stream::Stream(Stream &&other) noexcept
    : buf_(std::move(other.buf_)),
      level_(other.level_),
      sep_(other.sep_),
      first_(other.first_),
      active_(other.active_) {
    other.active_ = false;
}

Stream::~Stream() {
    if (!active_) return;
    std::ostream &out = (level_ == Level::Error || level_ == Level::Warn)
                            ? std::cerr
                            : std::clog;
    out << buf_.str() << '\n';
}

Stream debug() { return Stream(Level::Debug); }
Stream info()  { return Stream(Level::Info); }
Stream warn()  { return Stream(Level::Warn); }
Stream error() { return Stream(Level::Error); }

}  // namespace chd::log
