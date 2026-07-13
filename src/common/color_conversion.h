// SPDX-License-Identifier: GPL-3.0-or-later
//
// The two coefficient families that carry a decoder's composite-domain Y/U/V
// to R'G'B' or to E'Y/E'Cb/E'Cr, and the options selecting their precision.
//
// They come from different standards and are independent of each other, but
// the literature unfortunately spells both with a K and the subscripts R and
// B. This header keeps them apart by name:
//
//   - The colour-difference coefficients kr/kg/kb are the luma matrix of
//     ITU-T H.273 eq 44. They say what fraction of E'Y each primary carries,
//     and they are what MatrixCoefficients signals downstream.
//
//   - The broadcast scaling factors uReduction/vReduction are the reductions
//     applied to the two colour differences before they modulate the
//     subcarrier: U = uReduction*(E'B - E'Y), V = vReduction*(E'R - E'Y).
//     H.273 does not define these at all; they belong to the composite
//     encoding standards.
//
// Only the green row of the U/V → R'G'B' matrix depends on both families. The
// red and blue rows depend on the scaling alone, because the colour-difference
// spans cancel: E'B = E'Y + 2*(1-kb)*E'Cb = E'Y + U/uReduction, and likewise
// for E'R. That is the point of transmitting B-Y and R-Y rather than G-Y.

#ifndef CHD_COMMON_COLOR_CONVERSION_H
#define CHD_COMMON_COLOR_CONVERSION_H

#include <optional>
#include <string>

namespace chd::color {

// Precision of the luma / colour-difference equation (ITU-T H.273 eq 44).
enum class ColorDifferencePrecision {
    // NTSC 1953: E'Y = 0.30 E'R + 0.59 E'G + 0.11 E'B. Transcribed in SMPTE
    // EG 27 Annex A from the NTSC final report; signalled by H.273
    // MatrixCoefficients 4, which cites FCC Title 47 CFR 73.682 (a)(20).
    Classic = 0,
    // ITU-R BT.470 §2.4 / SMPTE ST 170 / ITU-R BT.1700 item 8:
    // E'Y = 0.299 E'R + 0.587 E'G + 0.114 E'B. H.273 MatrixCoefficients 5
    // (625-line) and 6 (525-line).
    Modern,
};

// Precision of the composite U/V reduction factors.
enum class BroadcastScalingPrecision {
    // ITU-R BT.470-6 §2.5 / BT.1700 item 9, as published to three decimals:
    // E'U = 0.493 (E'B - E'Y), E'V = 0.877 (E'R - E'Y). SMPTE ST 170 Annex
    // A.3 records that these trace back to a 1953 derivation that used a blue
    // luma coefficient of 0.115 rather than 0.114.
    Classic = 0,
    // SMPTE ST 170 Annex A.3 eq 4 and 5, which redo that derivation from the
    // correct luma matrix and publish the result to six decimals.
    Modern,
    // The exact closed forms ST 170's trailing ellipses stand for
    // [Poynton eq 28.1 p336], carried to double precision.
    Scientific,
};

// Both families resolved, plus every scalar derived from them that the output
// paths apply per sample.
struct ColorConversion {
    // ITU-T H.273 eq 44 luma coefficients; kg = 1 - kr - kb.
    double kr, kg, kb;

    // Composite U/V reduction factors.
    double uReduction, vReduction;

    // Full-excursion colour-difference spans 2*(1-kb) and 2*(1-kr)
    // [H.273 eq 45, 46].
    double blueDifferenceScale, redDifferenceScale;

    // Composite-domain U/V → normalized E'Cb / E'Cr, per unit signal range.
    double ecbFromU, ecrFromV;

    // Composite-domain U/V → R'G'B'. Luma coefficients reach the green row
    // only; see the header comment.
    double rFromV, gFromU, gFromV, bFromU;
};

ColorConversion resolveColorConversion(ColorDifferencePrecision colorDifference,
                               BroadcastScalingPrecision broadcastScaling);

// Parse an option string. nullopt on an unrecognised value; commit rejects
// that with CHD_E_INVALID_ARG.
std::optional<ColorDifferencePrecision> parseColorDifferencePrecision(const std::string &name);
std::optional<BroadcastScalingPrecision> parseBroadcastScalingPrecision(const std::string &name);

// The canonical option string for a value (for diagnostics / round-trips).
const char *colorDifferencePrecisionName(ColorDifferencePrecision precision);
const char *broadcastScalingPrecisionName(BroadcastScalingPrecision precision);

}  // namespace chd::color

#endif  // CHD_COMMON_COLOR_CONVERSION_H
