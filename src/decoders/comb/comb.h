// SPDX-License-Identifier: GPL-3.0-or-later
/************************************************************************

    comb.h

    ld-chroma-decoder - Colourisation filter for ld-decode
    Copyright (C) 2018 Chad Page
    Copyright (C) 2018-2019 Simon Inns
    Copyright (C) 2020-2021 Adam Sampson
    Copyright (C) 2021 Phillip Blucas

    This file is part of ld-decode-tools.

    ld-chroma-decoder is free software: you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

************************************************************************/

#ifndef CHD_DECODERS_COMB_COMB_H
#define CHD_DECODERS_COMB_COMB_H

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "../../metadata/core.h"

#include "../../output/component_frame.h"
#include "../../reader/source.h"
#include "../../common/color_conversion.h"
#include "../decoder_base.h"
#include "../source_field.h"

#if defined(CHD_WITH_NN)
namespace chd::nn { class InferenceEngine; }
#endif

namespace chd::decoders::comb {

class BetaAccumulator;

class Comb
{
public:
    Comb();

    // Reconstruction applied to the demodulated chroma. The first three are
    // "equiband" in the SMPTE ST 170 Annex A.4 sense — equal bandwidth on both
    // axes — differing only in cutoff; the WidebandI* modes are the NTSC-1953
    // unequal (wide-I/narrow-Q) split.
    //   EquibandWide ~2.2 MHz low-pass on both axes (the unchanged
    //   ld-chroma-decoder default; passes everything below fSC)
    //   Equiband13   1.3 MHz low-pass on both axes (SMPTE ST 170 / ITU-R BT.1700 U/V)
    //   ColorUnder   ~0.5 MHz low-pass on both axes, matching the surviving
    //                bandwidth of VHS/S-VHS colour-under chroma (IEC 60774-1 §6.2)
    //   WidebandI    1.3 MHz on I, 0.6 MHz on Q (NTSC-1953 asymmetric split).
    //                Internal only — no ABI string maps to it; retained as the
    //                non-reconstructing baseline the WidebandISSB unit test
    //                measures against. Not a recommended decode for any source
    //                (WidebandISSB dominates it on the I axis).
    //   WidebandISSB WidebandI plus single-sideband reconstruction: the
    //                0.6-1.3 MHz wideband-I detail NTSC-1953 transmits
    //                lower-sideband-only is restored to full amplitude by
    //                Hilbert-transforming its crosstalk off the Q axis
    // The I/Q-asymmetric modes are only meaningful on the burst-locked I/Q
    // axes, so they require phaseCompensation; without it filterIQ falls back
    // to Equiband13. The equiband modes (EquibandWide/Equiband13/ColorUnder)
    // are rotation-invariant and need no phase compensation.
    enum class ChromaFilterMode { EquibandWide, Equiband13, ColorUnder, WidebandI, WidebandISSB };

    // Comb filter configuration parameters
    struct Configuration {
        double chromaGain = 1.0;
        double chromaPhase = 0.0;
        // Needed only by the show-map overlay, which draws R'G'B' colours that
        // must survive the OutputWriter's inverse conversion.
        chd::color::ColorConversion colorConversion = chd::color::resolveColorConversion(
            chd::color::ColorDifferencePrecision::Modern,
            chd::color::BroadcastScalingPrecision::Scientific);
        int32_t dimensions = 2;
        bool adaptive = true;
        bool showMap = false;
        bool phaseCompensation = false;
        ChromaFilterMode chromaFilterMode = ChromaFilterMode::EquibandWide;

        double cNRLevel = 0.0;
        double yNRLevel = 0.0;

        // Adaptation sensitivity for 3D filter (higher = prefer 3D, lower = prefer 1D/2D)
        // Default 1.0, range typically 0.5-2.0
        double adaptThreshold = 1.0;
        double chromaWeight = 1.0;

        // nnTransform3D mode: replace the classical 3D split
        // with asdfqazsnbb's 3D-FFT + CNN-mask + IFFT pipeline.
        // Requires dimensions == 3 + a session bound via Comb::setNnModel.
        // nnInputMagnitudeScale matches the per-model normalisation
        // constant (1.0 for the original chroma_net, 128.0 for v2).
        bool nnTransform3D = false;
        double nnInputMagnitudeScale = 1.0;

        // Calibration pass: when set, decodeFrames feeds the burst-locked
        // demodulated I/Q planes to the accumulator and skips reconstruction
        // (output frames are left undecoded). Only meaningful with
        // phaseCompensation. Non-owning; caller keeps it alive.
        BetaAccumulator *betaAccumulator = nullptr;

        // Measured NTSC-1953 sideband-asymmetry profile (from
        // chd_chroma_sideband_calibrate or a preset). plateau > 0 enables the
        // WidebandISSB transition-band corrections; updateConfiguration
        // synthesizes the correction taps below from these parameters.
        // plateau == 0 (the default) is exactly today's behaviour.
        double ssbBetaPlateau = 0.0;
        double ssbBetaEdgeCenterHz = 0.0;
        double ssbBetaEdgeWidthHz = 0.0;

        // Derived by updateConfiguration — not caller-set.
        std::vector<double> ssbIEqTaps;
        std::vector<double> ssbQNullTaps;

        int32_t getLookBehind() const;
        int32_t getLookAhead() const;
    };

    const Configuration &getConfiguration() const;
    void updateConfiguration(const chd::metadata::LdDecodeMetaData::VideoParameters &videoParameters,
                             const Configuration &configuration);

#if defined(CHD_WITH_NN)
    // Bind the inference engine used by nnTransform3D. The engine is
    // thread-safe (ORT Run / CoreML predict are both reentrant) so one
    // engine is shared across all worker threads. Pass nullptr to unbind;
    // the decoder will fall back to 2D chroma. Caller retains shared
    // ownership.
    void setNnModel(std::shared_ptr<chd::nn::InferenceEngine> engine);
#endif

    // Decode a sequence of fields into a sequence of interlaced frames
    void decodeFrames(const std::vector<chd::decoders::SourceField> &inputFields, int32_t startIndex, int32_t endIndex,
                      std::vector<chd::output::ComponentFrame> &componentFrames);

    // Maximum frame size
    static constexpr int32_t MAX_WIDTH = 910;
    static constexpr int32_t MAX_HEIGHT = 525;

protected:

private:
    // Comb-filter configuration parameters
    bool configurationSet;
    Configuration configuration;
    chd::metadata::LdDecodeMetaData::VideoParameters videoParameters;

#if defined(CHD_WITH_NN)
    // Bound nnTransform3D session (nullptr ⇒ fall back to 2D chroma even
    // when configuration.nnTransform3D is true). Shared by all worker
    // threads via the DecoderPool — Ort::Session is documented as
    // thread-safe; concurrent Run() calls inside the per-tile
    // loop are serialised by nnRunMutex below to match tbc-tools.
    std::shared_ptr<chd::nn::InferenceEngine> nnSession;
    std::mutex nnRunMutex;
#endif

    // An input frame in the process of being decoded
    class FrameBuffer {
    public:
        FrameBuffer(const chd::metadata::LdDecodeMetaData::VideoParameters &videoParameters_, const Configuration &configuration_);

        void loadFields(const chd::decoders::SourceField &firstField, const chd::decoders::SourceField &secondField);

        void split1D();
        void split2D();
        void split3D(const FrameBuffer &previousFrame, const FrameBuffer &nextFrame);

#if defined(CHD_WITH_NN)
        // Run the nnTransform3D 3D-FFT + CNN-mask + IFFT chroma extraction
        // for this frame and its successor. Reads `rawbuffer` of `this` and
        // `nextFrame`; accumulates into `this->nnAccChroma` and
        // `this->nnWeightSum` (plus `nextFrame->nnAccChroma/Sum` for the
        // overlapping tiles that span the frame boundary). Caller must
        // call finalizeNnTransform3D() afterwards to normalise.
        //
        // `session` is the shared Ort::Session; `runMutex` serialises
        // concurrent Run() calls inside this method (matches tbc-tools).
        // Returns true on success, false if the session became unusable
        // mid-frame (in which case the caller should fall back to 2D).
        bool split3DnnTransform(FrameBuffer &nextFrame,
                                chd::nn::InferenceEngine &engine,
                                std::mutex &runMutex,
                                double inputMagnitudeScale);
        void finalizeNnTransform3D();
        void fallbackNnTransform3DTo2D();
#endif

        // Copy raw baseband samples into the component frame's Y plane.
        // Used by the nnTransform3D path before splitIQ; the classical
        // path's splitIQ does this fold inline.
        void copyRawToLuma();

        void setComponentFrame(chd::output::ComponentFrame &_componentFrame) {
            componentFrame = &_componentFrame;
        }

        void splitIQ();
        void splitIQlocked();
        void filterIQ();
        void filterIQFull();
        void adjustY();
        void doCNR();
        void doYNR();
        void transformIQ(double chromaGain, double chromaPhase);

        void overlayMap(const FrameBuffer &previousFrame, const FrameBuffer &nextFrame);

    private:
        const chd::metadata::LdDecodeMetaData::VideoParameters &videoParameters;
        const Configuration &configuration;

        // Calculated frame height
        int32_t frameHeight;

        // IRE scaling
        double irescale;

        // Baseband samples (interlaced to form a complete frame)
        chd::reader::Data rawbuffer;

        // Chroma phase of the frame's two fields
        int32_t firstFieldPhaseID;
        int32_t secondFieldPhaseID;

        // 1D, 2D and 3D-filtered chroma samples
        struct Sample {
            double pixel[MAX_HEIGHT][MAX_WIDTH];
        } clpbuffer[3];

#if defined(CHD_WITH_NN)
        // Per-pixel overlap-add accumulators for the nnTransform3D pass.
        // Sized frameHeight × videoParameters.fieldWidth, lazily allocated
        // on first split3DnnTransform call. After finalizeNnTransform3D
        // the normalised chroma lands in clpbuffer[2] (the 3D-chroma
        // slot) so splitIQ picks it up unchanged.
        std::vector<std::vector<double>> nnAccChroma;
        std::vector<std::vector<double>> nnWeightSum;
#endif

        // Result of evaluating a 3D candidate
        struct Candidate {
            double penalty;
            double sample;
        };

        // The component frame for output (if there is one)
        chd::output::ComponentFrame *componentFrame;

        inline int32_t getFieldID(int32_t lineNumber) const;
        inline bool getLinePhase(int32_t lineNumber) const;
        void getBestCandidate(int32_t lineNumber, int32_t h,
                              const FrameBuffer &previousFrame, const FrameBuffer &nextFrame,
                              int32_t &bestIndex, double &bestSample) const;
        Candidate getCandidate(int32_t refLineNumber, int32_t refH,
                               const FrameBuffer &frameBuffer, int32_t lineNumber, int32_t h,
                               double adjustPenalty) const;
    };
};

}  // namespace chd::decoders::comb

#endif  // CHD_DECODERS_COMB_COMB_H
