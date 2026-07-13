// SPDX-License-Identifier: GPL-3.0-or-later
/************************************************************************

    palcolour.h

    ld-chroma-decoder - Colourisation filter for ld-decode
    Copyright (C) 2018-2019 Simon Inns
    Copyright (C) 2019 Adam Sampson

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

#ifndef CHD_DECODERS_PALCOLOUR_PALCOLOUR_H
#define CHD_DECODERS_PALCOLOUR_PALCOLOUR_H

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "../../metadata/core.h"

#include "../../output/component_frame.h"
#include "../chroma_filter.h"
#include "../../common/color_conversion.h"
#include "../decoder_base.h"
#include "../source_field.h"
#include "transform_pal.h"

namespace chd::decoders::palcolour {

class PalColour
{
public:
    PalColour();

    // Specify which method to use to separate luma and chroma information.
    // (This is the separation *algorithm*; the chroma *bandwidth* is a
    // separate setting, carried by chromaBandwidthHz below.)
    enum SeparationMethod {
        // PALColour's 2D FIR filter
        palColourFilter = 0,
        // 2D Transform PAL frequency-domain filter
        transform2DFilter,
        // 3D Transform PAL frequency-domain filter
        transform3DFilter,
		//mono decoder
		mono
    };

    struct Configuration {
        double chromaGain = 1.0;
        double chromaPhase = 0.0;
        // Needed only by the show-FFT overlay, which draws R'G'B' colours that
        // must survive the OutputWriter's inverse conversion.
        chd::color::ColorConversion colorConversion = chd::color::resolveColorConversion(
            chd::color::ColorDifferencePrecision::Modern,
            chd::color::BroadcastScalingPrecision::Scientific);
        double yNRLevel = 0.0;
        bool simplePAL = false;
        SeparationMethod separation = palColourFilter;

        // Resolved chroma low-pass cutoff (-6 dB corner) for the PALcolour 2D
        // raised-cosine filter, in Hz. The default is the legacy 1.1/0.93
        // dot-pattern-tuned value (compat); the chroma_filter option resolves
        // equiband (1.3 MHz) / color_under (~0.5 MHz) to their own cutoffs.
        double chromaBandwidthHz = chd::decoders::kCompatPalChromaHz;

        // equiband_vsb: recover the vestigial (lower-sideband-only) chroma
        // between chromaUpperSidebandHz and the equiband ceiling to full
        // amplitude by an EQ shelf. Off (false) leaves a plain symmetric
        // raised-cosine filter at chromaBandwidthHz. chromaUpperSidebandHz is
        // the upper-sideband room +X above the subcarrier where the channel
        // clipped the upper sideband (e.g. 1066000 for System-I PAL).
        bool chromaVsbRecovery = false;
        double chromaUpperSidebandHz = 0.0;

        double transformThreshold = 0.4;
        std::vector<double> transformThresholds;
        bool showFFTs = false;
        int32_t showPositionX = 200;
        int32_t showPositionY = 200;

        int32_t getThresholdsSize() const;
        int32_t getLookBehind() const;
        int32_t getLookAhead() const;
    };

    const Configuration &getConfiguration() const;
    void updateConfiguration(const chd::metadata::LdDecodeMetaData::VideoParameters &videoParameters,
                             const Configuration &configuration);

    // Decode a sequence of fields into a sequence of interlaced frames
    void decodeFrames(const std::vector<chd::decoders::SourceField> &inputFields, int32_t startIndex, int32_t endIndex,
                      std::vector<chd::output::ComponentFrame> &outputFrames);

    // Maximum frame size, based on PAL
    static constexpr int32_t MAX_WIDTH = 1135;

private:
    // Information about a line we're decoding.
    struct LineInfo {
        explicit LineInfo(int32_t number);

        int32_t number;
        // detectBurst computes bp, bq = cos(t), sin(t), where t is the burst phase.
        // They're used to build a rotation matrix for the chroma signals in decodeLine.
        double bp, bq;
        double Vsw;
    };

    void buildLookUpTables();
    void decodeField(const chd::decoders::SourceField &inputField, const double *chromaData, chd::output::ComponentFrame &componentFrame);
    void detectBurst(LineInfo &line, const uint16_t *inputData);
    template <typename ChromaSample, bool PREFILTERED_CHROMA>
    void decodeLine(const chd::decoders::SourceField &inputField, const ChromaSample *chromaData, const LineInfo &line,
                    chd::output::ComponentFrame &componentFrame);
    void doYNR(double *Yline);

    // Configuration parameters
    bool configurationSet;
    Configuration configuration;
    chd::metadata::LdDecodeMetaData::VideoParameters videoParameters;

    // Transform PAL filter
    std::unique_ptr<TransformPal> transformPal;

    // The subcarrier reference signal
    double sine[MAX_WIDTH], cosine[MAX_WIDTH];

    // Coefficients for the three 2D chroma low-pass filters. There are
    // separate filters for U and V, but only the signs differ, so they can
    // share a set of coefficients.
    //
    // The filters are horizontally and vertically symmetrical, so each 2D
    // array represents one quarter of a filter. The zeroth horizontal element
    // is included in the sum twice, so the coefficient is halved to
    // compensate. Each filter is (2 * filterSize) + 1 elements wide.
    //
    // filterSize is the highest tap index carrying a nonzero coefficient: the
    // raised cosine reaches its first zero at the kernel half-width
    // ca = 0.5 * sampleRate / chromaBandwidthHz, so floor(ca) spans the whole
    // nonzero kernel. buildLookUpTables derives it from the resolved chroma
    // cutoff and the sample rate, so narrower chroma bandwidths (which need a
    // wider kernel) and higher sample rates size the tables up automatically
    // rather than overflowing a fixed ceiling.
    int32_t filterSize = 0;
    std::vector<std::array<double, 4>> cfilt;
    std::vector<std::array<double, 2>> yfilt;

    // equiband_vsb: linear-phase EQ kernel that restores the half-amplitude
    // droop of the vestigial (lower-sideband-only) chroma band. Empty unless
    // chromaVsbRecovery is set; synthesized in buildLookUpTables from the
    // upper-sideband room +X and the equiband cutoff. Applied per line to the
    // recovered U and V in decodeField.
    std::vector<double> vsbEqTaps;
};

}  // namespace chd::decoders::palcolour

#endif  // CHD_DECODERS_PALCOLOUR_PALCOLOUR_H
