// SPDX-License-Identifier: GPL-3.0-or-later
//
// Internal diagnostic logging. Messages are handed to the consumer-installed
// sink (see <chromadec/log.h>); with no sink installed nothing is emitted
// anywhere, which is the default. A library does not own the process's
// console, so failures travel the return path (chd_status_t plus
// chd::detail::set_last_error) and this channel carries diagnostics.

#pragma once

#include <chromadec/log.h>

#include <sstream>

namespace chd::log {

using Level = chd_log_level_t;
using Flags = chd_log_flags_t;

// Whether a message at this level would reach a sink. Checked up front by
// Stream, so a suppressed message costs one atomic load and no formatting.
// CHD_LOG_OFF (and anything above it) always answers false: it is a threshold
// value and no message carries it.
bool isEnabled(Level level);

class Stream {
public:
    explicit Stream(Level level, bool record_as_last_error = false);
    Stream(const Stream &) = delete;
    Stream &operator=(const Stream &) = delete;
    Stream(Stream &&other) noexcept;
    ~Stream();

    template <typename T>
    Stream &operator<<(const T &value) {
        if (!active_) return *this;
        if (!first_) buf_ << sep_;
        buf_ << value;
        first_ = false;
        return *this;
    }

    Stream &nospace() { sep_ = ""; return *this; }

private:
    std::ostringstream buf_;
    Level level_;
    const char *sep_ = " ";
    bool first_ = true;
    bool active_ = true;
    bool record_ = false;
};

Stream debug();
Stream info();
Stream warn();
Stream error();

// A failure the caller is about to be told about. Records the message as the
// thread's chd_last_error() detail as well as emitting it at error level, so
// the reason survives the trip up through a `false` or a chd_status_t that has
// no room to carry it. Use error() only for an error that is not being
// returned; anything a caller learns about through a return value wants this.
//
// The distinction reaches the consumer: a fail() message carries
// CHD_LOG_F_RETURNED, which is how a sink tells "you are about to be handed
// this as a status too" from "this is the only place you will hear it". So the
// choice is a promise, not a shade of meaning. If an intermediate layer
// recovers from the condition instead of propagating it, the site is an
// error(), because no caller is going to be told.
//
// A fail() message becomes public API text, so it names what the caller gave
// us rather than the internal that noticed: the ABI shim already prefixes the
// entry point, and a class or method name means nothing to a consumer. error()
// and the other levels are diagnostics and may name internals freely.
//
// Unlike the other streams this one always formats its message, since the
// detail string has to be there whether or not a sink is listening.
Stream fail();

// Emit an already-formatted message. Used for diagnostics that arrive as a
// complete string rather than as a stream, such as the sink the library
// installs on ONNX Runtime.
void emit(Level level, Flags flags, const char *message);

// Sink and threshold configuration, backing the <chromadec/log.h> entry
// points. setSink(nullptr, nullptr) uninstalls; once it returns the previous
// sink is neither running nor reachable.
void  setSink(chd_log_fn fn_or_null, void *user_data);
void  installStderrSink();
void  setMinLevel(Level level);
Level minLevel();

}  // namespace chd::log