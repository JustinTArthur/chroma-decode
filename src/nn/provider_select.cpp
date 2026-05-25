// SPDX-License-Identifier: GPL-3.0-or-later

#include "provider_select.h"

#include <algorithm>
#include <sstream>
#include <unordered_map>

namespace chd::nn {

namespace {

const char *providerName(ProviderPreference p)
{
    switch (p) {
        case CHD_NN_EP_AUTO:     return "auto";
        case CHD_NN_EP_CPU:      return "CPU";
        case CHD_NN_EP_CUDA:     return "CUDA";
        case CHD_NN_EP_TENSORRT: return "TensorRT";
        case CHD_NN_EP_COREML:   return "CoreML";
        case CHD_NN_EP_DIRECTML: return "DirectML";
        case CHD_NN_EP_MIGRAPHX: return "MIGraphX";
    }
    return "unknown";
}

// ORT's name for each provider in Ort::GetAvailableProviders()'s list.
// Names taken from the ORT public API documentation; the C++ wrapper just
// reads the C-style strings out of the OrtApi vector.
const char *availabilityKey(ProviderPreference p)
{
    switch (p) {
        case CHD_NN_EP_CPU:      return "CPUExecutionProvider";
        case CHD_NN_EP_CUDA:     return "CUDAExecutionProvider";
        case CHD_NN_EP_TENSORRT: return "TensorrtExecutionProvider";
        case CHD_NN_EP_COREML:   return "CoreMLExecutionProvider";
        case CHD_NN_EP_DIRECTML: return "DmlExecutionProvider";
        case CHD_NN_EP_MIGRAPHX: return "MIGraphXExecutionProvider";
        case CHD_NN_EP_AUTO:     return "";
    }
    return "";
}

}  // namespace

std::vector<ProviderPreference> buildAutoChain(ProviderPreference requested)
{
    std::vector<ProviderPreference> autoChain;
#if defined(_WIN32)
    autoChain = { CHD_NN_EP_TENSORRT, CHD_NN_EP_CUDA, CHD_NN_EP_DIRECTML, CHD_NN_EP_CPU };
#elif defined(__APPLE__)
    autoChain = { CHD_NN_EP_COREML, CHD_NN_EP_CPU };
#else
    // Linux + everything else
    autoChain = { CHD_NN_EP_CUDA, CHD_NN_EP_MIGRAPHX, CHD_NN_EP_CPU };
#endif

    if (requested == CHD_NN_EP_AUTO) return autoChain;

    // Caller pinned a specific provider: try only that one, then fall through
    // to CPU as the universal fallback only if the caller pinned CPU
    // explicitly. The ABI contract is: "Explicit: try only that one;
    // CHD_E_NN_PROVIDER_UNAVAILABLE on failure".
    return { requested };
}

bool providerIsAvailable(ProviderPreference provider)
{
    if (provider == CHD_NN_EP_AUTO) return true;
    if (provider == CHD_NN_EP_CPU)  return true;  // always
    const std::string key = availabilityKey(provider);
    if (key.empty()) return false;
    try {
        const auto providers = Ort::GetAvailableProviders();
        for (const auto &name : providers) {
            if (name == key) return true;
        }
    } catch (...) {
        return false;
    }
    return false;
}

bool attachCpu(Ort::SessionOptions &, std::string *)
{
    // The CPU provider is ORT's default — nothing to append. Returning true
    // here is the universal "everything tried and we're using CPU" leaf.
    return true;
}

bool attachCuda(Ort::SessionOptions &options, std::string *outError)
{
    // CUDA attach is non-trivial: it requires CUDA driver libs loaded, plus
    // the onnxruntime_providers_cuda.dll/.so to be discoverable. The full
    // tbc-tools recipe (CreateCUDAProviderOptions + UpdateCUDAProviderOptions
    // + SessionOptionsAppendExecutionProvider_CUDA_V2 with fallback to a
    // compatibility option set) lives at
    // ~/Development/Repos/tbc-tools/src/ld-chroma-decoder/comb.cpp:368.
    //
    // The initial attach uses the V2 API with default options;
    // production CUDA tuning (cudnn_conv_algo_search, tunable_op_enable,
    // prefer_nhwc, etc.) is part of the CUDA FFT path, where the
    // full driver-load probe also lands.
    const auto &api = Ort::GetApi();
    OrtCUDAProviderOptionsV2 *cudaOptions = nullptr;
    OrtStatus *status = api.CreateCUDAProviderOptions(&cudaOptions);
    if (status != nullptr) {
        if (outError) *outError = std::string("CreateCUDAProviderOptions: ") +
                                  api.GetErrorMessage(status);
        api.ReleaseStatus(status);
        return false;
    }
    status = api.SessionOptionsAppendExecutionProvider_CUDA_V2(options, cudaOptions);
    api.ReleaseCUDAProviderOptions(cudaOptions);
    if (status != nullptr) {
        if (outError) *outError = std::string("SessionOptionsAppendExecutionProvider_CUDA_V2: ") +
                                  api.GetErrorMessage(status);
        api.ReleaseStatus(status);
        return false;
    }
    return true;
}

bool attachCoreML(Ort::SessionOptions &options, std::string *outError)
{
#if defined(__APPLE__)
    try {
        const std::unordered_map<std::string, std::string> coremlOpts = {
            { "MLComputeUnits", "ALL" },
            { "ModelFormat",    "MLProgram" },
        };
        options.AppendExecutionProvider("CoreML", coremlOpts);
        return true;
    } catch (const std::exception &e) {
        if (outError) *outError = e.what();
        return false;
    }
#else
    (void)options;
    if (outError) *outError = "CoreML execution provider is only available on macOS";
    return false;
#endif
}

bool attachDirectML(Ort::SessionOptions &, std::string *outError)
{
    // Full DirectML attach lands in a follow-up alongside the CUDA tuning.
    if (outError) *outError = "DirectML execution provider attach is not yet implemented "
                              "(lands in a follow-up)";
    return false;
}

bool attachTensorRT(Ort::SessionOptions &, std::string *outError)
{
    // Same as DirectML — lands in a follow-up.
    if (outError) *outError = "TensorRT execution provider attach is not yet implemented "
                              "(lands in a follow-up)";
    return false;
}

bool attachMIGraphX(Ort::SessionOptions &, std::string *outError)
{
    // Same as DirectML — lands in a follow-up.
    if (outError) *outError = "MIGraphX execution provider attach is not yet implemented "
                              "(lands in a follow-up)";
    return false;
}

bool attachProviderChain(Ort::SessionOptions &options,
                         const std::vector<ProviderPreference> &chain,
                         ProviderPreference *outAttached,
                         std::string *outError)
{
    std::ostringstream errs;
    for (const auto provider : chain) {
        if (!providerIsAvailable(provider)) {
            errs << providerName(provider) << ": not available in this ORT build\n";
            continue;
        }

        bool ok = false;
        std::string err;
        switch (provider) {
            case CHD_NN_EP_CPU:      ok = attachCpu     (options, &err); break;
            case CHD_NN_EP_CUDA:     ok = attachCuda    (options, &err); break;
            case CHD_NN_EP_COREML:   ok = attachCoreML  (options, &err); break;
            case CHD_NN_EP_DIRECTML: ok = attachDirectML(options, &err); break;
            case CHD_NN_EP_TENSORRT: ok = attachTensorRT(options, &err); break;
            case CHD_NN_EP_MIGRAPHX: ok = attachMIGraphX(options, &err); break;
            case CHD_NN_EP_AUTO:
                // AUTO can't appear in a resolved chain.
                err = "AUTO is not a concrete provider";
                break;
        }
        if (ok) {
            if (outAttached) *outAttached = provider;
            return true;
        }
        errs << providerName(provider) << ": " << err << "\n";
    }

    if (outError) *outError = errs.str();
    return false;
}

}  // namespace chd::nn
