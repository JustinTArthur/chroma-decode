// SPDX-License-Identifier: GPL-3.0-or-later
//
// The two precision options and the matrix they resolve to.
//   1. parse/name round-trip + unknown strings rejected.
//   2. The default (modern, scientific) reproduces the constants
//      ld-chroma-decoder applies, so the options are opt-in only.
//   3. Colour-difference precision reaches the green row of the U/V → R'G'B'
//      matrix and the E'Cb/E'Cr spans, but NOT the red and blue rows: the
//      spans cancel there [H.273 eq 45, 46]. Broadcast scaling reaches all
//      four rows.
//   4. RGB48 and RGBS agree pixel-for-pixel on a frame with real chroma. Both
//      matrix the composite U/V directly, so they must; a black-chroma frame
//      cannot tell them apart, which is how a wrong green coefficient once
//      survived in the float path.
//   5. Colour-difference precision reaches integer Y'CbCr too, through the
//      spans E'Cb/E'Cr are normalized against. Luma stays put.

#include <cmath>
#include <cstdint>
#include <iostream>
#include <utility>
#include <vector>

#include "../../src/common/color_conversion.h"
#include "../../src/output/component_frame.h"
#include "../../src/output/output_writer.h"

namespace {

#define REQUIRE(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #cond "\n"; \
        return 1; \
    } \
} while (0)

using chd::color::BroadcastScalingPrecision;
using chd::color::ColorDifferencePrecision;
using chd::color::ColorConversion;
using chd::color::resolveColorConversion;
using chd::output::ComponentFrame;
using chd::output::OutputWriter;

bool near(double a, double b, double tol) {
    return std::fabs(a - b) <= tol;
}

int testParseRoundTrip() {
    const ColorDifferencePrecision cdps[] = {
        ColorDifferencePrecision::Classic, ColorDifferencePrecision::Modern,
    };
    for (auto p : cdps) {
        auto parsed = chd::color::parseColorDifferencePrecision(
            chd::color::colorDifferencePrecisionName(p));
        REQUIRE(parsed.has_value());
        REQUIRE(*parsed == p);
    }

    const BroadcastScalingPrecision bsps[] = {
        BroadcastScalingPrecision::Classic, BroadcastScalingPrecision::Modern,
        BroadcastScalingPrecision::Scientific,
    };
    for (auto p : bsps) {
        auto parsed = chd::color::parseBroadcastScalingPrecision(
            chd::color::broadcastScalingPrecisionName(p));
        REQUIRE(parsed.has_value());
        REQUIRE(*parsed == p);
    }

    REQUIRE(!chd::color::parseColorDifferencePrecision("scientific").has_value());
    REQUIRE(!chd::color::parseColorDifferencePrecision("").has_value());
    REQUIRE(!chd::color::parseBroadcastScalingPrecision("Modern").has_value());
    REQUIRE(!chd::color::parseBroadcastScalingPrecision("bt470").has_value());
    return 0;
}

// The default pairing must reproduce ld-chroma-decoder's numbers exactly.
int testDefaultsMatchLegacy() {
    const ColorConversion m = resolveColorConversion(ColorDifferencePrecision::Modern,
                                             BroadcastScalingPrecision::Scientific);
    REQUIRE(near(m.kr, 0.299, 1e-12));
    REQUIRE(near(m.kg, 0.587, 1e-12));
    REQUIRE(near(m.kb, 0.114, 1e-12));
    REQUIRE(near(m.blueDifferenceScale, 1.772, 1e-12));
    REQUIRE(near(m.redDifferenceScale,  1.402, 1e-12));

    REQUIRE(near(m.rFromV,  1.139883, 5e-7));
    REQUIRE(near(m.gFromU, -0.394642, 5e-7));
    REQUIRE(near(m.gFromV, -0.580622, 5e-7));
    REQUIRE(near(m.bFromU,  2.032062, 5e-7));
    return 0;
}

// Colour-difference precision selects H.273 MatrixCoefficients 4 vs 5/6.
int testColorDifferencePrecision() {
    const ColorConversion modern = resolveColorConversion(ColorDifferencePrecision::Modern,
                                                  BroadcastScalingPrecision::Scientific);
    const ColorConversion classic = resolveColorConversion(ColorDifferencePrecision::Classic,
                                                   BroadcastScalingPrecision::Scientific);

    // NTSC 1953: E'Y = 0.30 E'R + 0.59 E'G + 0.11 E'B [SMPTE EG 27 Annex A].
    REQUIRE(near(classic.kr, 0.30, 1e-12));
    REQUIRE(near(classic.kg, 0.59, 1e-12));
    REQUIRE(near(classic.kb, 0.11, 1e-12));

    // Spans follow the luma coefficients: 2*(1-kb), 2*(1-kr).
    REQUIRE(near(classic.blueDifferenceScale, 1.78, 1e-12));
    REQUIRE(near(classic.redDifferenceScale,  1.40, 1e-12));
    REQUIRE(classic.ecbFromU != modern.ecbFromU);
    REQUIRE(classic.ecrFromV != modern.ecrFromV);

    // Only green moves in the R'G'B' matrix: the spans cancel out of the red
    // and blue rows, which depend on the broadcast scaling alone.
    REQUIRE(classic.rFromV == modern.rFromV);
    REQUIRE(classic.bFromU == modern.bFromU);
    REQUIRE(classic.gFromU != modern.gFromU);
    REQUIRE(classic.gFromV != modern.gFromV);
    return 0;
}

// Broadcast scaling reaches every row of the R'G'B' matrix.
int testBroadcastScalingPrecision() {
    const ColorConversion sci = resolveColorConversion(ColorDifferencePrecision::Modern,
                                               BroadcastScalingPrecision::Scientific);
    const ColorConversion mod = resolveColorConversion(ColorDifferencePrecision::Modern,
                                               BroadcastScalingPrecision::Modern);
    const ColorConversion cls = resolveColorConversion(ColorDifferencePrecision::Modern,
                                               BroadcastScalingPrecision::Classic);

    // BT.470-6 §2.5 / BT.1700 item 9 as published.
    REQUIRE(near(cls.uReduction, 0.493, 1e-12));
    REQUIRE(near(cls.vReduction, 0.877, 1e-12));
    // SMPTE ST 170 Annex A.3 eq 4, 5.
    REQUIRE(near(mod.uReduction, 0.492111, 1e-12));
    REQUIRE(near(mod.vReduction, 0.877283, 1e-12));

    // ST 170's published six decimals are the closed forms truncated, so
    // modern and scientific agree far below a 16-bit quantum (~1.7e-5).
    REQUIRE(near(mod.uReduction, sci.uReduction, 1e-6));
    REQUIRE(near(mod.vReduction, sci.vReduction, 1e-6));
    REQUIRE(near(mod.rFromV, sci.rFromV, 1e-5));
    REQUIRE(near(mod.bFromU, sci.bFromU, 1e-5));

    // Classic is a real shift, and it moves all four rows.
    REQUIRE(cls.rFromV != sci.rFromV);
    REQUIRE(cls.gFromU != sci.gFromU);
    REQUIRE(cls.gFromV != sci.gFromV);
    REQUIRE(cls.bFromU != sci.bFromU);
    REQUIRE(!near(cls.bFromU, sci.bFromU, 1e-4));
    return 0;
}

chd::metadata::LdDecodeMetaData::VideoParameters ntscParams() {
    chd::metadata::LdDecodeMetaData::VideoParameters vp;
    vp.system = chd::metadata::NTSC;
    vp.fSC = 3579545.0;
    vp.fieldWidth = 910;
    vp.fieldHeight = 263;
    vp.sampleRate = 4.0 * 3579545.0;
    vp.black16bIre = 16384;
    vp.white16bIre = 54016;
    vp.blanking16bIre = 16384;
    vp.colourBurstStart = 78;
    vp.colourBurstEnd = 110;
    vp.activeVideoStart = 134;
    vp.activeVideoEnd = 894;
    vp.firstActiveFrameLine = 39;
    vp.lastActiveFrameLine = 524;
    vp.numberOfSequentialFields = 4;
    vp.isValid = true;
    return vp;
}

// RGB48 and RGBS matrix the same composite U/V, so they must land on the same
// picture. Chroma is deliberately non-zero: a black frame passes even with a
// wrong green coefficient, since it multiplies U and V by nothing.
int testIntegerAndFloatRgbAgree() {
    for (auto cdp : {ColorDifferencePrecision::Modern, ColorDifferencePrecision::Classic}) {
        for (auto bsp : {BroadcastScalingPrecision::Scientific,
                         BroadcastScalingPrecision::Modern,
                         BroadcastScalingPrecision::Classic}) {
            auto vp = ntscParams();

            OutputWriter::Configuration cfg;
            cfg.paddingAmount = 1;
            cfg.pixelFormat = OutputWriter::RGB48;
            cfg.clampMode = OutputWriter::CLAMP_NONE;
            cfg.colorDifferencePrecision = cdp;
            cfg.broadcastScalingPrecision = bsp;

            OutputWriter writer;
            writer.updateConfiguration(vp, cfg);

            ComponentFrame frame;
            frame.init(vp);

            // Mid-grey luma with a chroma ramp across the active region, kept
            // well inside the RGB48 [0, 65535] box so neither path clamps.
            // (RGB48 always saturates to [0, 65535] while float RGB under
            // CLAMP_NONE is unbounded, so an out-of-gamut sample would show up
            // as a difference that says nothing about the matrix.) Frame-line
            // bounds are inclusive, so the last active line is covered too.
            const double yRange = vp.white16bIre - vp.black16bIre;
            for (int32_t line = vp.firstActiveFrameLine; line <= vp.lastActiveFrameLine; line++) {
                double *y = frame.y(line);
                double *u = frame.u(line);
                double *v = frame.v(line);
                for (int32_t x = vp.activeVideoStart; x < vp.activeVideoEnd; x++) {
                    const double t = static_cast<double>(x - vp.activeVideoStart)
                                   / (vp.activeVideoEnd - vp.activeVideoStart);
                    y[x] = vp.black16bIre + 0.5 * yRange;
                    u[x] = 0.10 * yRange * std::sin(6.0 * t);
                    v[x] = 0.10 * yRange * std::cos(4.0 * t);
                }
            }

            chd::output::OutputFrame intFrame;
            writer.convert(frame, intFrame);

            std::vector<float> floatPlanes[3];
            writer.convertToFloatRGB(frame, floatPlanes);

            const int32_t w = writer.getActiveWidth();
            const int32_t h = writer.getOutputHeight();
            REQUIRE(static_cast<int32_t>(floatPlanes[0].size()) == w * h);

            // RGB48 truncates a [0, 65535] full-range value; RGBS carries the
            // same signal normalized in float32. So they can differ by just
            // over one code: up to 1.0 from the truncation, plus the float32
            // quantum (~0.004 at this magnitude). A wrong coefficient shows up
            // as hundreds of codes, so this still bites hard.
            constexpr double tol = 1.05;
            double worst = 0.0;
            int32_t worstI = -1, worstC = -1;
            for (int32_t i = 0; i < w * h; i++) {
                for (int32_t c = 0; c < 3; c++) {
                    const double f = floatPlanes[c][i] * 65535.0;
                    const double d = std::fabs(static_cast<double>(intFrame[i * 3 + c]) - f);
                    if (d > worst) { worst = d; worstI = i; worstC = c; }
                }
            }
            if (worst > tol) {
                std::cerr << "worst=" << worst << " i=" << worstI << " c=" << worstC
                          << " row=" << (worstI / w) << " col=" << (worstI % w)
                          << " int=" << intFrame[worstI * 3 + worstC]
                          << " float=" << floatPlanes[worstC][worstI] * 65535.0 << "\n";
            }
            REQUIRE(worst <= tol);
        }
    }
    return 0;
}

// Colour-difference precision reaches the integer Y'CbCr output, not just RGB:
// it sets the spans E'Cb/E'Cr are normalized against. Luma is untouched (the
// decoder recovers Y' off the wire; nothing re-derives it from primaries).
int testIntegerYCbCrRespondsToColorDifference() {
    const auto convertWith = [](ColorDifferencePrecision cdp) {
        auto vp = ntscParams();

        OutputWriter::Configuration cfg;
        cfg.paddingAmount = 1;
        cfg.pixelFormat = OutputWriter::YUV444P16;
        cfg.clampMode = OutputWriter::CLAMP_NONE;
        cfg.colorDifferencePrecision = cdp;
        cfg.broadcastScalingPrecision = BroadcastScalingPrecision::Scientific;

        OutputWriter writer;
        writer.updateConfiguration(vp, cfg);

        ComponentFrame frame;
        frame.init(vp);

        // Strong, constant chroma so the span change is well clear of rounding.
        const double yRange = vp.white16bIre - vp.black16bIre;
        for (int32_t line = vp.firstActiveFrameLine; line <= vp.lastActiveFrameLine; line++) {
            double *y = frame.y(line);
            double *u = frame.u(line);
            double *v = frame.v(line);
            for (int32_t x = vp.activeVideoStart; x < vp.activeVideoEnd; x++) {
                y[x] = vp.black16bIre + 0.5 * yRange;
                u[x] = 0.40 * yRange;
                v[x] = 0.40 * yRange;
            }
        }

        chd::output::OutputFrame out;
        writer.convert(frame, out);
        return std::make_pair(out, writer.getActiveWidth() * writer.getOutputHeight());
    };

    const auto [modernFrame, planeSize] = convertWith(ColorDifferencePrecision::Modern);
    const auto [classicFrame, planeSize2] = convertWith(ColorDifferencePrecision::Classic);
    REQUIRE(planeSize == planeSize2);

    // YUV444P16 lays out [Y | Cb | Cr], each planeSize samples.
    const int32_t y = 0;
    const int32_t cb = planeSize;
    const int32_t cr = 2 * planeSize;

    // Luma is identical: no luma coefficient touches it.
    REQUIRE(modernFrame[y] == classicFrame[y]);

    // Cb/Cr both move, and by enough to matter: neutral is 128*256 = 32768, so
    // compare the signed excursion from it. Classic's wider blue span (1.78 vs
    // 1.772) shrinks Cb ~0.45%; its narrower red span (1.40 vs 1.402) grows
    // Cr ~0.14%.
    const double neutral = 128.0 * 256.0;
    const double mCb = modernFrame[cb] - neutral, cCb = classicFrame[cb] - neutral;
    const double mCr = modernFrame[cr] - neutral, cCr = classicFrame[cr] - neutral;
    REQUIRE(std::fabs(mCb) > 1000.0);  // the fixture really does carry chroma
    REQUIRE(std::fabs(mCr) > 1000.0);
    REQUIRE(near(cCb / mCb, 1.0 - 0.00449, 1e-4));
    REQUIRE(near(cCr / mCr, 1.0 + 0.00143, 1e-4));
    return 0;
}

}  // namespace

int main() {
    if (testParseRoundTrip() != 0) return 1;
    if (testDefaultsMatchLegacy() != 0) return 1;
    if (testColorDifferencePrecision() != 0) return 1;
    if (testBroadcastScalingPrecision() != 0) return 1;
    if (testIntegerAndFloatRgbAgree() != 0) return 1;
    if (testIntegerYCbCrRespondsToColorDifference() != 0) return 1;
    std::cout << "test_color_conversion: all tests passed\n";
    return 0;
}