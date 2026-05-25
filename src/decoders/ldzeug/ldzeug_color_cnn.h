// SPDX-License-Identifier: GPL-3.0-or-later
//
// LdzeugColorCnnDecoder — replaces both Y/C separation and chroma demod
// with a single CNN inference. Input is 3-channel CVBS + analytically
// synthesized I-carrier + Q-carrier; output is 3-channel Y + I + Q.
// chroma is rotated to U/V via the uv_from_iq transform with
// configurable phase offset and gain.
//
// Ported from vapoursynth-analog's LdzeugColorCnnDecoder; original
// algorithm + model: **jsaowji** (ldzeug2 reference at
// ~/Development/Repos/ldzeug2). Bundled bundled weights live under
// ~/Development/Analog Decoding Models/for ldzeug2/color_cnn_*.onnx.

#ifndef CHD_DECODERS_LDZEUG_LDZEUG_COLOR_CNN_H
#define CHD_DECODERS_LDZEUG_LDZEUG_COLOR_CNN_H

#include "ldzeug_base.h"

namespace chd::decoders::ldzeug {

class LdzeugColorCnnDecoder : public LdzeugDecoderBase {
public:
    void decodeFrames(const std::vector<chd::decoders::SourceField> &inputFields,
                      int32_t startIndex, int32_t endIndex,
                      std::vector<chd::output::ComponentFrame> &componentFrames) override;

    // Configurable chroma rotation (mirrors decode_4fsc_video kwargs).
    void setChromaPhase(double degrees) { chromaPhase_ = degrees; }
    void setChromaGain(double gain)     { chromaGain_  = gain; }

private:
    double chromaPhase_ = 0.0;
    double chromaGain_  = 1.0;
};

}  // namespace chd::decoders::ldzeug

#endif  // CHD_DECODERS_LDZEUG_LDZEUG_COLOR_CNN_H
