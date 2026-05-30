// SPDX-License-Identifier: GPL-3.0-or-later
//
// LdzeugDecoderBase — shared scaffolding for the ldzeug2 NN-based NTSC
// decoders. Ported from vapoursynth-analog's `src/ldzeug_decoders.{h,cpp}`
// (which itself adapts jsaowji's ldzeug2 reference implementation).
//
// ldzeug decoders sit at a different pipeline point than
// nnTransform3D — they consume CVBS and emit Y (with chroma either
// fully derived by the NN, or analytically derived from CVBS-Y plus a
// quadrature demod). They DO NOT run inside the comb-filter chain.
//
// Both concrete decoders (LdzeugColorCnnDecoder, LdzeugLumaSepDecoder)
// share the same Ort::Session lifecycle and field-vs-frame mode handling
// via this base class. The session itself is supplied externally via
// chd::nn::OrtSession; the base never constructs one.

#ifndef CHD_DECODERS_LDZEUG_LDZEUG_BASE_H
#define CHD_DECODERS_LDZEUG_LDZEUG_BASE_H

#include <cstdint>
#include <memory>
#include <vector>

#include "../../metadata/core.h"
#include "../../output/component_frame.h"
#include "../decoder_base.h"
#include "../source_field.h"

namespace chd::nn { class OrtSession; }

namespace chd::decoders::ldzeug {

class LdzeugDecoderBase : public chd::decoders::Decoder {
public:
    // The two pipeline shapes for ldzeug2 models. The bundled weights for
    // jsaowji's color_cnn / luma_sep advertise dynamic input shapes —
    // mode selects whether we feed one inference per field or one per
    // weaved interlaced frame. The choice is a pipeline-structural
    // decision, not auto-detected.
    enum class Mode { Field, Frame };

    ~LdzeugDecoderBase() override = default;

    // chd::decoders::Decoder interface ─────────────────────────────────────
    bool configure(const chd::metadata::LdDecodeMetaData::VideoParameters &videoParameters) override;
    int32_t getLookBehind() const override { return 0; }
    int32_t getLookAhead()  const override { return 0; }

    // Bind the Ort session that will drive inference. Must be set before
    // decodeFrames is called; the decoder takes shared ownership.
    void setNnModel(std::shared_ptr<chd::nn::OrtSession> session);

    // Set the field-vs-frame input mode. Defaults to Field; choose Frame for
    // the weaved-frame model variant rather than the per-field variant.
    void setMode(Mode mode) { mode_ = mode; }
    Mode mode() const { return mode_; }

protected:
    chd::metadata::LdDecodeMetaData::VideoParameters videoParameters{};
    std::shared_ptr<chd::nn::OrtSession>             session_;
    Mode                                             mode_ = Mode::Field;
};

}  // namespace chd::decoders::ldzeug

#endif  // CHD_DECODERS_LDZEUG_LDZEUG_BASE_H
