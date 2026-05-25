// SPDX-License-Identifier: GPL-3.0-or-later

#include "ldzeug_color_cnn.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include <onnxruntime_cxx_api.h>

#include "../../common/log.h"
#include "../../nn/ort_session.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace chd::decoders::ldzeug {

namespace {

// I-channel rotation angle for U/V derivation. Matches ldzeug2's
// uv_from_iq when chromaPhase is in degrees and added to 33°.
constexpr double IQ_BASE_ANGLE_DEG = 33.0;

// Inverse normalisation: ldzeug2 trains against TBC samples in [0,1]
// where 16bIre 0..65535 → 0..1 directly, so ×65535 is the inverse.
constexpr double FROM_NORMALIZED = 65535.0;

// fieldPhase {1,2,3,4} → carrier sign, matching the table in
// ldzeug2.colordecoder.input_for_color_cnn.
inline double fieldPhaseSign(int32_t fieldPhase) {
    switch (fieldPhase) {
        case 1: return  1.0;
        case 2: return -1.0;
        case 3: return -1.0;
        case 4: return  1.0;
        default: return 1.0;   // unknown phase; preserve input
    }
}

// Integer-pixel I/Q carrier samples at 4fsc.
// cos((x*pi)/2 - pi/2) → {0, 1, 0, -1}.
// sin((x*pi)/2 - pi/2) → {-1, 0, 1, 0}.
constexpr std::array<double, 4> kBaseI = { 0.0,  1.0, 0.0, -1.0 };
constexpr std::array<double, 4> kBaseQ = {-1.0,  0.0, 1.0,  0.0 };

// uv_from_iq: rotation by (33° + chromaPhase), scaled by chromaGain.
//   theta = (33 + chromaPhase) * pi / 180
//   bp = sin(theta) * chromaGain
//   bq = cos(theta) * chromaGain
//   u = -bp*i + bq*q
//   v =  bq*i + bp*q
inline void applyUvFromIq(double i, double q, double bp, double bq,
                          double *u, double *v) {
    *u = -bp * i + bq * q;
    *v =  bq * i + bp * q;
}

}  // namespace

void LdzeugColorCnnDecoder::decodeFrames(
    const std::vector<chd::decoders::SourceField> &inputFields,
    int32_t startIndex, int32_t endIndex,
    std::vector<chd::output::ComponentFrame> &componentFrames)
{
    if (session_ == nullptr) {
        throw std::runtime_error("ldzeug2_color_cnn: no NN model bound");
    }

    const auto &vp = videoParameters;
    const int32_t fieldWidth  = vp.fieldWidth;
    const int32_t fieldHeight = vp.fieldHeight;
    const int32_t frameHeight = fieldHeight * 2;
    const size_t  fieldStride = static_cast<size_t>(fieldWidth) * fieldHeight;
    const size_t  frameStride = static_cast<size_t>(fieldWidth) * frameHeight;

    // Lifted out of the per-field loop.
    const double theta = (IQ_BASE_ANGLE_DEG + chromaPhase_) * M_PI / 180.0;
    const double bp = std::sin(theta) * chromaGain_;
    const double bq = std::cos(theta) * chromaGain_;

    const size_t planeStride = (mode_ == Mode::Frame) ? frameStride : fieldStride;
    std::vector<float> inputBuffer(3 * planeStride);

    Ort::MemoryInfo memInfo = Ort::MemoryInfo::CreateCpu(
        OrtArenaAllocator, OrtMemTypeDefault);

    const int32_t modelHeight = (mode_ == Mode::Frame) ? frameHeight : fieldHeight;
    const std::array<int64_t, 4> inputShape = { 1, 3, modelHeight, fieldWidth };

    // Cache input/output names (ORT requires them as raw c_str pointers
    // owned by the AllocatedStringPtr).
    Ort::AllocatorWithDefaultOptions allocator;
    auto inputName  = session_->session().GetInputNameAllocated(0, allocator);
    auto outputName = session_->session().GetOutputNameAllocated(0, allocator);
    const char *inputNamePtr  = inputName.get();
    const char *outputNamePtr = outputName.get();

    auto runOnce = [&](Ort::Value &&inputTensor) {
        auto outputs = session_->session().Run(
            Ort::RunOptions{ nullptr },
            &inputNamePtr,  &inputTensor, 1,
            &outputNamePtr, 1);
        if (outputs.empty()) {
            throw std::runtime_error("ldzeug2_color_cnn: inference produced no outputs");
        }
        const auto outShape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
        if (outShape.size() != 4 || outShape[1] != 3) {
            throw std::runtime_error("ldzeug2_color_cnn: unexpected output tensor shape");
        }
        return outputs;
    };

    const int32_t inputCount = static_cast<int32_t>(inputFields.size());
    const int32_t outFrameCap = static_cast<int32_t>(componentFrames.size());

    for (int32_t frameIdx = startIndex;
         frameIdx + 1 < endIndex && frameIdx + 1 < inputCount;
         frameIdx += 2) {
        const int32_t outFrame = (frameIdx - startIndex) / 2;
        if (outFrame >= outFrameCap) break;
        chd::output::ComponentFrame &out = componentFrames[outFrame];
        out.init(vp);

        if (mode_ == Mode::Frame) {
            // Weave both fields into a single 3-channel weaved-frame input.
            float *cvbsPlane = inputBuffer.data() + 0 * frameStride;
            float *iCarPlane = inputBuffer.data() + 1 * frameStride;
            float *qCarPlane = inputBuffer.data() + 2 * frameStride;

            constexpr float scale = 1.0f / 65535.0f;

            for (int32_t sub = 0; sub < 2; ++sub) {
                const chd::decoders::SourceField &field = inputFields[frameIdx + sub];
                const int32_t yOffset = field.getOffset();
                const int32_t fieldPhase = field.field.fieldPhaseID;
                const double  phaseSign  = fieldPhaseSign(fieldPhase);
                const auto *src = field.data.data();
                for (int32_t y = 0; y < fieldHeight; ++y) {
                    const int32_t frameLine = y * 2 + yOffset;
                    if (frameLine >= frameHeight) break;
                    const double rowSign = (y % 2 == 1 ? -1.0 : 1.0) * phaseSign;
                    float *cv = cvbsPlane + frameLine * fieldWidth;
                    float *ic = iCarPlane + frameLine * fieldWidth;
                    float *qc = qCarPlane + frameLine * fieldWidth;
                    for (int32_t x = 0; x < fieldWidth; ++x) {
                        cv[x] = static_cast<float>(src[y * fieldWidth + x]) * scale;
                        ic[x] = static_cast<float>(kBaseI[x % 4] * rowSign);
                        qc[x] = static_cast<float>(kBaseQ[x % 4] * rowSign);
                    }
                }
            }

            Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
                memInfo, inputBuffer.data(), inputBuffer.size(),
                inputShape.data(), inputShape.size());
            auto outputs = runOnce(std::move(inputTensor));

            const float *outData = outputs[0].GetTensorData<float>();
            const float *yPlane  = outData + 0 * frameStride;
            const float *iPlane  = outData + 1 * frameStride;
            const float *qPlane  = outData + 2 * frameStride;

            for (int32_t fy = 0; fy < frameHeight; ++fy) {
                double *yRow = out.y(fy);
                double *uRow = out.u(fy);
                double *vRow = out.v(fy);
                for (int32_t x = 0; x < fieldWidth; ++x) {
                    const double iVal = static_cast<double>(iPlane[fy * fieldWidth + x]) * FROM_NORMALIZED;
                    const double qVal = static_cast<double>(qPlane[fy * fieldWidth + x]) * FROM_NORMALIZED;
                    double u, v;
                    applyUvFromIq(iVal, qVal, bp, bq, &u, &v);
                    yRow[x] = static_cast<double>(yPlane[fy * fieldWidth + x]) * FROM_NORMALIZED;
                    uRow[x] = u;
                    vRow[x] = v;
                }
            }
            continue;
        }

        // Field mode: per-field inference, then weave outputs.
        for (int32_t sub = 0; sub < 2; ++sub) {
            const chd::decoders::SourceField &field = inputFields[frameIdx + sub];
            const int32_t fieldPhase = field.field.fieldPhaseID;
            const int32_t yOffset    = field.getOffset();
            const double  phaseSign  = fieldPhaseSign(fieldPhase);
            const auto   *src        = field.data.data();

            float *cvbsPlane = inputBuffer.data() + 0 * fieldStride;
            float *iCarPlane = inputBuffer.data() + 1 * fieldStride;
            float *qCarPlane = inputBuffer.data() + 2 * fieldStride;

            constexpr float scale = 1.0f / 65535.0f;
            for (int32_t y = 0; y < fieldHeight; ++y) {
                const double rowSign = (y % 2 == 1 ? -1.0 : 1.0) * phaseSign;
                float *cv = cvbsPlane + y * fieldWidth;
                float *ic = iCarPlane + y * fieldWidth;
                float *qc = qCarPlane + y * fieldWidth;
                for (int32_t x = 0; x < fieldWidth; ++x) {
                    cv[x] = static_cast<float>(src[y * fieldWidth + x]) * scale;
                    ic[x] = static_cast<float>(kBaseI[x % 4] * rowSign);
                    qc[x] = static_cast<float>(kBaseQ[x % 4] * rowSign);
                }
            }

            Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
                memInfo, inputBuffer.data(), inputBuffer.size(),
                inputShape.data(), inputShape.size());
            auto outputs = runOnce(std::move(inputTensor));

            const float *outData = outputs[0].GetTensorData<float>();
            const float *yPlane  = outData + 0 * fieldStride;
            const float *iPlane  = outData + 1 * fieldStride;
            const float *qPlane  = outData + 2 * fieldStride;

            for (int32_t y = 0; y < fieldHeight; ++y) {
                const int32_t frameLine = y * 2 + yOffset;
                if (frameLine >= frameHeight) break;
                double *yRow = out.y(frameLine);
                double *uRow = out.u(frameLine);
                double *vRow = out.v(frameLine);
                for (int32_t x = 0; x < fieldWidth; ++x) {
                    const double iVal = static_cast<double>(iPlane[y * fieldWidth + x]) * FROM_NORMALIZED;
                    const double qVal = static_cast<double>(qPlane[y * fieldWidth + x]) * FROM_NORMALIZED;
                    double u, v;
                    applyUvFromIq(iVal, qVal, bp, bq, &u, &v);
                    yRow[x] = static_cast<double>(yPlane[y * fieldWidth + x]) * FROM_NORMALIZED;
                    uRow[x] = u;
                    vRow[x] = v;
                }
            }
        }
    }
}

}  // namespace chd::decoders::ldzeug
