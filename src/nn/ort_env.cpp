// SPDX-License-Identifier: GPL-3.0-or-later

#include "ort_env.h"

#include <stdexcept>

#include "../common/log.h"

namespace chd::nn {

namespace {

// ORT's default logger writes to stderr on its own. Give it our logging
// function instead, so everything a consumer sees arrives through the one sink
// they installed. These never carry CHD_LOG_F_RETURNED: ORT hands them to us
// out of band, with no relation to whatever call is in progress.
void ORT_API_CALL ortLogSink(void *, OrtLoggingLevel severity, const char *category,
                             const char *logid, const char *code_location,
                             const char *message)
{
    chd::log::Level level = CHD_LOG_INFO;
    switch (severity) {
        case ORT_LOGGING_LEVEL_VERBOSE: level = CHD_LOG_DEBUG; break;
        case ORT_LOGGING_LEVEL_INFO:    level = CHD_LOG_INFO;  break;
        case ORT_LOGGING_LEVEL_WARNING: level = CHD_LOG_WARN;  break;
        case ORT_LOGGING_LEVEL_ERROR:
        case ORT_LOGGING_LEVEL_FATAL:   level = CHD_LOG_ERROR; break;
    }
    if (!chd::log::isEnabled(level)) return;
    chd::log::Stream(level).nospace()
        << "onnxruntime [" << (logid ? logid : "") << ":" << (category ? category : "")
        << " " << (code_location ? code_location : "") << "] "
        << (message ? message : "");
}

std::unique_ptr<Ort::Env> makeEnv()
{
    return std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "chromadec",
                                      ortLogSink, nullptr);
}

}  // namespace

std::once_flag                 OrtEnvSingleton::onceFlag_;
std::unique_ptr<Ort::Env>      OrtEnvSingleton::env_;
std::mutex                     OrtEnvSingleton::shutdownMutex_;

Ort::Env &OrtEnvSingleton::get()
{
    std::call_once(onceFlag_, []() { env_ = makeEnv(); });
    if (env_ == nullptr) {
        // Re-construct after a prior shutdown(). Protected by the mutex so
        // concurrent get() calls don't race on env_ assignment.
        std::lock_guard<std::mutex> lock(shutdownMutex_);
        if (env_ == nullptr) {
            env_ = makeEnv();
        }
    }
    return *env_;
}

void OrtEnvSingleton::shutdown()
{
    std::lock_guard<std::mutex> lock(shutdownMutex_);
    env_.reset();
}

}  // namespace chd::nn
