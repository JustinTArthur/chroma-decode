// SPDX-License-Identifier: GPL-3.0-or-later

#include "ort_env.h"

#include <stdexcept>

namespace chd::nn {

std::once_flag                 OrtEnvSingleton::onceFlag_;
std::unique_ptr<Ort::Env>      OrtEnvSingleton::env_;
std::mutex                     OrtEnvSingleton::shutdownMutex_;

Ort::Env &OrtEnvSingleton::get()
{
    std::call_once(onceFlag_, []() {
        env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "chromadec");
    });
    if (env_ == nullptr) {
        // Re-construct after a prior shutdown(). Protected by the mutex so
        // concurrent get() calls don't race on env_ assignment.
        std::lock_guard<std::mutex> lock(shutdownMutex_);
        if (env_ == nullptr) {
            env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "chromadec");
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
