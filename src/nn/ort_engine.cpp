// SPDX-License-Identifier: GPL-3.0-or-later

#include "ort_engine.h"

#include <utility>

namespace chd::nn {

namespace {

// Holds the Ort::Value outputs of one Run() alive so the caller can read
// their tensor data after run() returns.
class OrtRunResult : public RunResult {
public:
    explicit OrtRunResult(std::vector<Ort::Value> values)
        : values_(std::move(values)) {}

    size_t count() const override { return values_.size(); }

    const float *data(size_t index) const override {
        return values_[index].GetTensorData<float>();
    }

    std::vector<int64_t> shape(size_t index) const override {
        return values_[index].GetTensorTypeAndShapeInfo().GetShape();
    }

private:
    std::vector<Ort::Value> values_;
};

}  // namespace

OrtEngine::OrtEngine(std::shared_ptr<OrtSession> session)
    : session_(std::move(session)),
      memInfo_(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault))
{
    // Eagerly snapshot the I/O names (the OrtSession populate is lazy and
    // unsynchronised; doing it once here keeps the per-call accessors
    // race-free once the engine is shared across worker threads).
    inputNames_  = session_->inputNames();
    outputNames_ = session_->outputNames();
}

std::unique_ptr<RunResult> OrtEngine::run(
    const std::vector<TensorSpec>  &inputs,
    const std::vector<std::string> &outputNames)
{
    std::vector<Ort::Value> inputValues;
    inputValues.reserve(inputs.size());

    // Own the name strings locally so their c_str() pointers stay valid
    // through the Run() call (positional inputs borrow from inputNames_).
    std::vector<std::string> inNameStorage;
    inNameStorage.reserve(inputs.size());

    for (size_t i = 0; i < inputs.size(); ++i) {
        const TensorSpec &t = inputs[i];
        size_t elems = 1;
        for (int64_t d : t.shape) elems *= static_cast<size_t>(d);
        inputValues.push_back(Ort::Value::CreateTensor<float>(
            memInfo_, const_cast<float *>(t.data), elems,
            t.shape.data(), t.shape.size()));
        inNameStorage.push_back(t.name.empty() ? inputNames_[i] : t.name);
    }

    std::vector<const char *> inNamePtrs;
    inNamePtrs.reserve(inNameStorage.size());
    for (const auto &s : inNameStorage) inNamePtrs.push_back(s.c_str());

    const std::vector<std::string> &outNames =
        outputNames.empty() ? outputNames_ : outputNames;
    std::vector<const char *> outNamePtrs;
    outNamePtrs.reserve(outNames.size());
    for (const auto &s : outNames) outNamePtrs.push_back(s.c_str());

    auto outs = session_->session().Run(
        Ort::RunOptions{ nullptr },
        inNamePtrs.data(), inputValues.data(), inputValues.size(),
        outNamePtrs.data(), outNamePtrs.size());

    return std::make_unique<OrtRunResult>(std::move(outs));
}

}  // namespace chd::nn
