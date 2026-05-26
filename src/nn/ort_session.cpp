// SPDX-License-Identifier: GPL-3.0-or-later

#include "ort_session.h"

#include <stdexcept>

#include "ort_env.h"

namespace chd::nn {

namespace {

void applyCommonOptions(Ort::SessionOptions &so, const SessionOptions &opts)
{
    if (opts.intraOpThreads > 0) so.SetIntraOpNumThreads(opts.intraOpThreads);
    if (opts.interOpThreads > 0) so.SetInterOpNumThreads(opts.interOpThreads);
    so.SetGraphOptimizationLevel(opts.enableGraphOptim
                                     ? GraphOptimizationLevel::ORT_ENABLE_ALL
                                     : GraphOptimizationLevel::ORT_DISABLE_ALL);
    if (!opts.enableMemPattern) so.DisableMemPattern();
}

}  // namespace

OrtSession::OrtSession(const std::string &modelPath, const SessionOptions &opts)
{
    // Force-init the process-wide Ort::Env BEFORE any provider attach
    // touches ORT internals. From ORT 1.25 onward, UpdateCUDAProviderOptions
    // (and likely other V2 provider-options mutators) log through the
    // default Logger that the Ort::Env constructor registers; without an
    // Env in scope they abort with "Attempt to use DefaultLogger but none
    // has been registered." Older ORT versions tolerated the reversed
    // order, which is why this was latent on the macOS CoreML path.
    (void)OrtEnvSingleton::get();

    Ort::SessionOptions sessionOptions;
    applyCommonOptions(sessionOptions, opts);

    const auto chain = buildAutoChain(opts.requestedProvider);
    std::string attachError;
    chd_nn_provider_t attached = CHD_NN_EP_CPU;
    if (!attachProviderChain(sessionOptions, chain, &attached, &attachError)) {
        throw std::runtime_error("provider attach failed: " + attachError);
    }
    activeProvider_ = attached;

    try {
#if defined(_WIN32)
        // ORT on Windows takes wide-char paths.
        std::wstring wpath(modelPath.begin(), modelPath.end());
        session_ = std::make_unique<Ort::Session>(OrtEnvSingleton::get(), wpath.c_str(),
                                                  sessionOptions);
#else
        session_ = std::make_unique<Ort::Session>(OrtEnvSingleton::get(), modelPath.c_str(),
                                                  sessionOptions);
#endif
    } catch (const std::exception &e) {
        throw std::runtime_error(std::string("Ort::Session create: ") + e.what());
    }
}

const std::vector<std::string> &OrtSession::inputNames()
{
    if (!ioNamesCached_) {
        Ort::AllocatorWithDefaultOptions allocator;
        const size_t numInputs = session_->GetInputCount();
        for (size_t i = 0; i < numInputs; ++i) {
            auto name = session_->GetInputNameAllocated(i, allocator);
            inputNames_.emplace_back(name.get());
        }
        const size_t numOutputs = session_->GetOutputCount();
        for (size_t i = 0; i < numOutputs; ++i) {
            auto name = session_->GetOutputNameAllocated(i, allocator);
            outputNames_.emplace_back(name.get());
        }
        ioNamesCached_ = true;
    }
    return inputNames_;
}

const std::vector<std::string> &OrtSession::outputNames()
{
    inputNames();  // triggers the lazy populate
    return outputNames_;
}

}  // namespace chd::nn
