// SPDX-License-Identifier: GPL-3.0-or-later
//
// LdzeugLumaSepDecoder — the NN extracts Y from CVBS only. Chroma is
// derived analytically as `CVBS − Y` and run through a per-pixel
// quadrature demod (mirroring Comb::FrameBuffer::splitIQ) with an
// optional `c_colorlp_b` bandpass FIR.
//
// Ported from vapoursynth-analog's LdzeugLumaSepDecoder; original
// algorithm + model: **jsaowji** (ldzeug2 reference).

#ifndef CHD_DECODERS_LDZEUG_LDZEUG_LUMA_SEP_H
#define CHD_DECODERS_LDZEUG_LDZEUG_LUMA_SEP_H

#include "ldzeug_base.h"

namespace chd::decoders::ldzeug {

class LdzeugLumaSepDecoder : public LdzeugDecoderBase {
public:
    void decodeFrames(const std::vector<chd::decoders::SourceField> &inputFields,
                      int32_t startIndex, int32_t endIndex,
                      std::vector<chd::output::ComponentFrame> &componentFrames) override;

    void setChromaPhase(double degrees) { chromaPhase_ = degrees; }
    void setChromaGain(double gain)     { chromaGain_  = gain; }

    // When true (default), run the c_colorlp_b 17-tap LP FIR on I and Q
    // after the per-pixel demod. Matches ldzeug2's comb_split_already
    // `color_bp=True` default.
    void setChromaBandpass(bool enable) { chromaBandpass_ = enable; }

private:
    double chromaPhase_  = 0.0;
    double chromaGain_   = 1.0;
    bool   chromaBandpass_ = true;
};

}  // namespace chd::decoders::ldzeug

#endif  // CHD_DECODERS_LDZEUG_LDZEUG_LUMA_SEP_H
