// SPDX-License-Identifier: GPL-3.0-or-later
/************************************************************************

    outputwriter.cpp

    ld-chroma-decoder - Colourisation filter for ld-decode
    Copyright (C) 2018-2021 Chad Page
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

#include "output_writer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <sstream>
#include <stdexcept>

#include "../common/log.h"
#include "component_frame.h"

namespace chd::output {

// ITU-R BT.601-7 / ITU-T H.273 luma matrix coefficients for MatrixCoefficients
// 5 (625-line) and 6 (525-line): Kr = 0.299, Kb = 0.114. The full-excursion
// color-difference spans are 2*(1-Kb) and 2*(1-Kr) [H.273 eq 46, 47].
static constexpr double ONE_MINUS_Kb = 1.0 - 0.114;
static constexpr double ONE_MINUS_Kr = 1.0 - 0.299;
static constexpr double BLUE_DIFFERENCE_SCALE = 2.0 * ONE_MINUS_Kb;  // 1.772
static constexpr double RED_DIFFERENCE_SCALE  = 2.0 * ONE_MINUS_Kr;  // 1.402

// Reduction factors relating the composite-domain U/V stored in ComponentFrame
// to the (B'-Y') and (R'-Y') color differences: U = kB*(B'-Y'), V = kR*(R'-Y').
// kB = sqrt(209556997.0 / 96146491.0) / 3.0
// kR = sqrt(221990474.0 / 288439473.0)
// [Poynton eq 28.1 p336]
static constexpr double kB = 0.49211104112248356308804691718185;
static constexpr double kR = 0.87728321993817866838972487283129;

// Narrow-range ("limited range") quantization of the normalized signals
// E'Y in [0,1] and E'Cb/E'Cr in [-0.5,0.5] to 16-bit integers, per
// ITU-T H.273 eq 30-32 with BitDepth = 16 (so 1 << (BitDepth-8) = 256):
//   Y  = Round((219*E'Y  + 16 ) * 256)
//   Cb = Round((224*E'Cb + 128) * 256)
//   Cr = Round((224*E'Cr + 128) * 256)
static constexpr double Y_DIGITAL_SCALE = 219.0 * 256.0;
static constexpr double Y_DIGITAL_ZERO  = 16.0  * 256.0;
static constexpr double C_DIGITAL_SCALE = 224.0 * 256.0;
static constexpr double C_DIGITAL_ZERO  = 128.0 * 256.0;
static constexpr double SAMPLE_MAX      = 65535.0;  // (1 << 16) - 1

namespace {
// ITU-T H.273 Round(x) = Sign(x) * Floor(Abs(x) + 0.5).
double h273Round(double x) {
    return (x < 0.0 ? -1.0 : 1.0) * std::floor(std::abs(x) + 0.5);
}

// Quantize a normalized luma sample E'Y to a 16-bit code [H.273 eq 30],
// then clamp to the supplied code range.
uint16_t quantizeLuma(double eY, double lo, double hi) {
    return static_cast<uint16_t>(
        std::clamp(h273Round(Y_DIGITAL_SCALE * eY + Y_DIGITAL_ZERO), lo, hi));
}

// Quantize a normalized color-difference sample E'Cb/E'Cr to a 16-bit code
// [H.273 eq 31, 32], then clamp to the supplied code range.
uint16_t quantizeChroma(double eC, double lo, double hi) {
    return static_cast<uint16_t>(
        std::clamp(h273Round(C_DIGITAL_SCALE * eC + C_DIGITAL_ZERO), lo, hi));
}

// Code-range bounds for the integer Y'CbCr formats under a given clamp mode.
struct IntYCbCrBounds {
    double yLo, yHi;
    double cLo, cHi;
};

IntYCbCrBounds intYCbCrBounds(OutputWriter::ClampMode mode) {
    switch (mode) {
        case OutputWriter::CLAMP_LEGAL_RGB_SDR:
            // Y in [16, 235]*256, Cb/Cr in [16, 240]*256 — the canonical
            // BT.601 narrow-range box that round-trips to RGB in [0, 1].
            return {16.0 * 256.0, 235.0 * 256.0, 16.0 * 256.0, 240.0 * 256.0};
        case OutputWriter::CLAMP_LEGAL_YCBCR_BT601:
            // Y, Cb, Cr in [1.00d, 254.75d]*256 per ITU-R BT.601-7 sec 2.5.3
            // (codes 0 and 65280..65535 reserved for synchronization).
            return {1.0 * 256.0, 254.75 * 256.0, 1.0 * 256.0, 254.75 * 256.0};
        case OutputWriter::CLAMP_LEGAL_RGB_HDR:
            // No clean per-component Y'CbCr box maps to positive-unbounded
            // R'G'B', so this RGB-domain mode is a no-op for Y'CbCr output.
        case OutputWriter::CLAMP_NONE:
        default:
            // H.273 Clip1 to the full 16-bit numerical range.
            return {0.0, SAMPLE_MAX, 0.0, SAMPLE_MAX};
    }
}

// Per-component min/max for the integer RGB48 path under a given clamp mode.
struct IntRGBBounds {
    double lo, hi;
};

IntRGBBounds intRGBBounds(OutputWriter::ClampMode /*mode*/) {
    // RGB48 cannot represent the out-of-[0, 1] excursion that the legal modes
    // allow in R'G'B' (CLAMP_LEGAL_YCBCR_BT601 reaches R' = +1.884, etc., and
    // CLAMP_LEGAL_RGB_HDR is unbounded above), so the natural unsigned-16-bit
    // [0, 65535] saturation is the only clamp this format can express.
    return {0.0, 65535.0};
}

// Normalized-signal-domain bounds for the float Y'CbCr formats
// (CHD_PIXEL_YUV444PS, CHD_PIXEL_GRAYS) under a given clamp mode.
struct FloatYCbCrBounds {
    float eyLo, eyHi;
    float ecLo, ecHi;
};

FloatYCbCrBounds floatYCbCrBounds(OutputWriter::ClampMode mode) {
    switch (mode) {
        case OutputWriter::CLAMP_LEGAL_RGB_SDR:
            // E'Y in [0, 1]; E'Cb/E'Cr in [-0.5, 0.5].
            return {0.0f, 1.0f, -0.5f, 0.5f};
        case OutputWriter::CLAMP_LEGAL_YCBCR_BT601:
            // E'Y in [(1-16)/219, (254.75-16)/219] = [-15/219, 238.75/219].
            // E'C in [(1-128)/224, (254.75-128)/224] = [-127/224, 126.75/224].
            return {-15.0f / 219.0f, 238.75f / 219.0f,
                    -127.0f / 224.0f, 126.75f / 224.0f};
        case OutputWriter::CLAMP_LEGAL_RGB_HDR:
            // No clean per-component Y'CbCr box maps to positive-unbounded
            // R'G'B', so this RGB-domain mode is a no-op for Y'CbCr output.
        case OutputWriter::CLAMP_NONE:
        default: {
            const float inf = std::numeric_limits<float>::infinity();
            return {-inf, inf, -inf, inf};
        }
    }
}

// Normalized-signal-domain per-component bounds for CHD_PIXEL_RGBS under a
// given clamp mode. CLAMP_LEGAL_YCBCR_BT601 projects the BT.601 §2.5.3
// legal Y'CbCr volume forward through the MatrixCoefficients=5/6 matrix:
// each axis bound is reached at a corner of the Y'CbCr box.
struct FloatRGBBounds {
    float rLo, rHi;
    float gLo, gHi;
    float bLo, bHi;
};

FloatRGBBounds floatRGBBounds(OutputWriter::ClampMode mode) {
    const float inf = std::numeric_limits<float>::infinity();
    switch (mode) {
        case OutputWriter::CLAMP_LEGAL_RGB_SDR:
            // SDR cube: black to white on every component.
            return {0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f};
        case OutputWriter::CLAMP_LEGAL_RGB_HDR:
            // Positive-only light with unconstrained headroom past SDR
            // white (1.0): floor each component at black, no ceiling.
            return {0.0f, inf, 0.0f, inf, 0.0f, inf};
        case OutputWriter::CLAMP_LEGAL_YCBCR_BT601:
            return {-0.8633767f, 1.8835022f,
                    -0.6672844f, 1.6901543f,
                    -1.0731534f, 2.0928660f};
        case OutputWriter::CLAMP_NONE:
        default:
            return {-inf, inf, -inf, inf, -inf, inf};
    }
}
}  // namespace

void OutputWriter::updateConfiguration(chd::metadata::LdDecodeMetaData::VideoParameters &_videoParameters,
                                       const OutputWriter::Configuration &_config)
{
    config = _config;
    videoParameters = _videoParameters;
    topPadLines = 0;
    bottomPadLines = 0;

    activeWidth = videoParameters.activeVideoEnd - videoParameters.activeVideoStart;
    // firstActiveFrameLine/lastActiveFrameLine are inclusive, so the active
    // region spans (last - first + 1) lines.
    activeHeight = (videoParameters.lastActiveFrameLine - videoParameters.firstActiveFrameLine) + 1;
    outputHeight = activeHeight;

    if (config.paddingAmount > 1) {
        // Some video codecs require the width and height of a video to be divisible by
        // a given number of samples on each axis.
        
        // Expand horizontal active region so the width is divisible by the specified padding factor.
        while (true) {
            activeWidth = videoParameters.activeVideoEnd - videoParameters.activeVideoStart;
            if ((activeWidth % config.paddingAmount) == 0) {
                break;
            }

            // Add pixels to the right and left sides in turn, to keep the active area centred
            if ((activeWidth % 2) == 0) {
                videoParameters.activeVideoEnd++;
            } else {
                videoParameters.activeVideoStart--;
            }
        }

        // Insert empty padding lines so the height is divisible by by the specified padding factor.
        while (true) {
            outputHeight = topPadLines + activeHeight + bottomPadLines;
            if ((outputHeight % config.paddingAmount) == 0) {
                break;
            }

            // Add lines to the bottom and top in turn, to keep the active area centred
            if ((outputHeight % 2) == 0) {
                bottomPadLines++;
            } else {
                topPadLines++;
            }
        }

        // Update the caller's copy, now we've adjusted the active area
        _videoParameters = videoParameters;
    }
}

const char *OutputWriter::getPixelName() const
{
    switch (config.pixelFormat) {
    case RGB48:
        return "RGB48";
    case YUV444P16:
        return "YUV444P16";
    case GRAY16:
        return "GRAY16";
    default:
        return "unknown";
    }
}

void OutputWriter::printOutputInfo() const
{
    // Show output information to the user
    const int32_t frameHeight = (videoParameters.fieldHeight * 2) - 1;
    chd::log::info() << "Input video of" << videoParameters.fieldWidth << "x" << frameHeight
            << "will be colourised and trimmed to" << activeWidth << "x" << outputHeight
            << getPixelName() << "frames";
}

std::string OutputWriter::getStreamHeader() const
{
    // Only yuv4mpeg output needs a header
    if (!config.outputY4m) {
        return std::string();
    }

    std::ostringstream str;

    str << "YUV4MPEG2";

    // Frame size
    str << " W" << activeWidth;
    str << " H" << outputHeight;

    // Frame rate
    if (videoParameters.system == chd::metadata::PAL) {
        str << " F25:1";
    } else {
        str << " F30000:1001";
    }

    // Field order
    if (videoParameters.firstActiveFrameLine % 2 ^ topPadLines % 2) {
        str << " Ib";
    } else {
        str << " It";
    }

    // Pixel aspect ratio
    // Follows EBU R92 and SMPTE RP 187 except that values are scaled from
    // BT.601 sampling (13.5 MHz) to 4fSC
    if (videoParameters.system == chd::metadata::PAL) {
        if (videoParameters.isWidescreen) {
            str << " A865:779"; // (16 / 9) * (576 / (702 * 4*fSC / 13.5))
        } else {
            str << " A259:311"; // (4 / 3) * (576 / (702 * 4*fSC / 13.5))
        }
    } else {
        if (videoParameters.isWidescreen) {
            str << " A25:22"; // (16 / 9) * (480 / (708 * 4*fSC / 13.5))
        } else {
            str << " A352:413"; // (4 / 3) * (480 / (708 * 4*fSC / 13.5))
        }
    }

    // Pixel format
    switch (config.pixelFormat) {
    case YUV444P16:
        str << " C444p16 XCOLORRANGE=LIMITED";
        break;
    case GRAY16:
        str << " Cmono16 XCOLORRANGE=LIMITED";
        break;
    default:
        throw std::runtime_error("pixel format not supported in yuv4mpeg header");
    }

    str << "\n";
    return str.str();
}

std::string OutputWriter::getFrameHeader() const
{
    // Only yuv4mpeg output needs a header
    if (!config.outputY4m) {
        return std::string();
    }

    return std::string("FRAME\n");
}

void OutputWriter::convert(const ComponentFrame &componentFrame, OutputFrame &outputFrame) const
{
    // Work out the number of output values, and resize the vector accordingly
    int32_t totalSize = activeWidth * outputHeight;
    switch (config.pixelFormat) {
    case RGB48:
    case YUV444P16:
        totalSize *= 3;
        break;
    case GRAY16:
        break;
    }
    outputFrame.resize(totalSize);

    // Clear padding
    clearPadLines(0, topPadLines, outputFrame);
    clearPadLines(outputHeight - bottomPadLines, bottomPadLines, outputFrame);

    // Convert active lines
    for (int32_t y = 0; y < activeHeight; y++) {
        convertLine(y, componentFrame, outputFrame);
    }
}

void OutputWriter::clearPadLines(int32_t firstLine, int32_t numLines, OutputFrame &outputFrame) const
{
    switch (config.pixelFormat) {
        case RGB48: {
            // Fill with RGB black
            uint16_t *out = outputFrame.data() + (activeWidth * firstLine * 3);

            for (int32_t i = 0; i < numLines * activeWidth * 3; i++) {
                out[i] = 0;
            }

            break;
        }
        case YUV444P16: {
            // Fill Y with black, no chroma
            uint16_t *outY  = outputFrame.data() + (activeWidth * firstLine);
            uint16_t *outCB = outY + (activeWidth * outputHeight);
            uint16_t *outCR = outCB + (activeWidth * outputHeight);

            for (int32_t i = 0; i < numLines * activeWidth; i++) {
                outY[i]  = static_cast<uint16_t>(Y_DIGITAL_ZERO);
                outCB[i] = static_cast<uint16_t>(C_DIGITAL_ZERO);
                outCR[i] = static_cast<uint16_t>(C_DIGITAL_ZERO);
            }

            break;
        }
        case GRAY16: {
            // Fill with black
            uint16_t *out = outputFrame.data() + (activeWidth * firstLine);

            for (int32_t i = 0; i < numLines * activeWidth; i++) {
                out[i] = static_cast<uint16_t>(Y_DIGITAL_ZERO);
            }

            break;
        }
    }
}

void OutputWriter::convertLine(int32_t lineNumber, const ComponentFrame &componentFrame, OutputFrame &outputFrame) const
{
    // Get pointers to the component data for the active region
    const int32_t inputLine = videoParameters.firstActiveFrameLine + lineNumber;
    const double *inY = componentFrame.y(inputLine) + videoParameters.activeVideoStart;
    // Not used if output is GRAY16
    const double *inU = (config.pixelFormat != GRAY16) ?
                            componentFrame.u(inputLine) + videoParameters.activeVideoStart : nullptr;
    const double *inV = (config.pixelFormat != GRAY16) ?
                            componentFrame.v(inputLine) + videoParameters.activeVideoStart : nullptr;

    const int32_t outputLine = topPadLines + lineNumber;

    const double yOffset = videoParameters.black16bIre;
    const double yRange = videoParameters.white16bIre - videoParameters.black16bIre;
    const double uvRange = yRange;

    // Scale factors from the composite-domain Y/U/V stored in the ComponentFrame
    // to the normalized signals E'Y in [0,1] and E'Cb/E'Cr in [-0.5,0.5]
    // [ITU-T H.273 eq 45-47]. The chroma factors undo the U = kB*(B'-Y') /
    // V = kR*(R'-Y') reduction and divide by the full color-difference excursion.
    const double eyScale  = 1.0 / yRange;
    const double ecbScale = 1.0 / (BLUE_DIFFERENCE_SCALE * kB * uvRange);
    const double ecrScale = 1.0 / (RED_DIFFERENCE_SCALE  * kR * uvRange);

    switch (config.pixelFormat) {
        case RGB48: {
            // Convert Y'UV to full-range R'G'B' [Poynton eq 28.6 p337]
            uint16_t *out = outputFrame.data() + (activeWidth * outputLine * 3);

            const double yScale = 65535.0 / yRange;
            const double uvScale = 65535.0 / uvRange;
            const auto rgb = intRGBBounds(config.clampMode);

            for (int32_t x = 0; x < activeWidth; x++) {
                // Scale Y'UV to 0-65535
                const double rY = std::clamp((inY[x] - yOffset) * yScale, rgb.lo, rgb.hi);
                const double rU = inU[x] * uvScale;
                const double rV = inV[x] * uvScale;

                // Convert Y'UV to R'G'B'
                const int32_t pos = x * 3;
                out[pos]     = static_cast<uint16_t>(std::clamp(rY                    + (1.139883 * rV),  rgb.lo, rgb.hi));
                out[pos + 1] = static_cast<uint16_t>(std::clamp(rY + (-0.394642 * rU) + (-0.580622 * rV), rgb.lo, rgb.hi));
                out[pos + 2] = static_cast<uint16_t>(std::clamp(rY + (2.032062 * rU),                     rgb.lo, rgb.hi));
            }

            break;
        }
        case YUV444P16: {
            // Quantize the normalized E'Y/E'Cb/E'Cr signals to 16-bit narrow-range
            // Y'Cb'Cr' [ITU-T H.273 eq 30-32].
            uint16_t *outY  = outputFrame.data() + (activeWidth * outputLine);
            uint16_t *outCB = outY + (activeWidth * outputHeight);
            uint16_t *outCR = outCB + (activeWidth * outputHeight);
            const auto b = intYCbCrBounds(config.clampMode);

            for (int32_t x = 0; x < activeWidth; x++) {
                outY[x]  = quantizeLuma((inY[x] - yOffset) * eyScale, b.yLo, b.yHi);
                outCB[x] = quantizeChroma(inU[x] * ecbScale,           b.cLo, b.cHi);
                outCR[x] = quantizeChroma(inV[x] * ecrScale,           b.cLo, b.cHi);
            }

            break;
        }
        case GRAY16: {
            // Throw away chroma and quantize E'Y to the same narrow-range scale
            // as the Y' plane of Y'Cb'Cr'.
            uint16_t *out = outputFrame.data() + (activeWidth * outputLine);
            const auto b = intYCbCrBounds(config.clampMode);

            for (int32_t x = 0; x < activeWidth; x++) {
                out[x] = quantizeLuma((inY[x] - yOffset) * eyScale, b.yLo, b.yHi);
            }

            break;
        }
    }
}

void OutputWriter::convertToFloat(const ComponentFrame &componentFrame,
                                  std::vector<float> *outPlanes, bool includeChroma) const
{
    // Produce the normalized colour-difference planes E'Y (and, when
    // includeChroma is set, E'Cb and E'Cr) [ITU-T H.273 eq 45-47] directly
    // from the ComponentFrame, sharing the geometry (active crop + top/bottom
    // padding) of the integer convert() path so float and integer frames are
    // pixel-aligned. E'Y is in [0,1] (black to white); E'Cb/E'Cr are centred
    // at 0.0 with a ±0.5 excursion. When includeChroma is false the chroma
    // planes are left empty and the ComponentFrame's U/V are not read, matching
    // GRAY16 and supporting mono ComponentFrames (which deallocate U/V).
    const size_t planeSize = static_cast<size_t>(activeWidth) * outputHeight;
    outPlanes[0].assign(planeSize, 0.0f);
    if (includeChroma) {
        outPlanes[1].assign(planeSize, 0.0f);
        outPlanes[2].assign(planeSize, 0.0f);
    } else {
        outPlanes[1].clear();
        outPlanes[2].clear();
    }

    const double yOffset = videoParameters.black16bIre;
    const double yRange = videoParameters.white16bIre - videoParameters.black16bIre;
    const double uvRange = yRange;
    const double eyScale  = 1.0 / yRange;
    const double ecbScale = 1.0 / (BLUE_DIFFERENCE_SCALE * kB * uvRange);
    const double ecrScale = 1.0 / (RED_DIFFERENCE_SCALE  * kR * uvRange);
    const auto bounds = floatYCbCrBounds(config.clampMode);

    for (int32_t y = 0; y < activeHeight; y++) {
        const int32_t inputLine = videoParameters.firstActiveFrameLine + y;
        const double *inY = componentFrame.y(inputLine) + videoParameters.activeVideoStart;
        const int32_t outputLine = topPadLines + y;
        float *outY = outPlanes[0].data() + static_cast<size_t>(outputLine) * activeWidth;

        for (int32_t x = 0; x < activeWidth; x++) {
            outY[x] = std::clamp(static_cast<float>((inY[x] - yOffset) * eyScale),
                                 bounds.eyLo, bounds.eyHi);
        }

        if (!includeChroma) continue;

        const double *inU = componentFrame.u(inputLine) + videoParameters.activeVideoStart;
        const double *inV = componentFrame.v(inputLine) + videoParameters.activeVideoStart;
        float *outCB = outPlanes[1].data() + static_cast<size_t>(outputLine) * activeWidth;
        float *outCR = outPlanes[2].data() + static_cast<size_t>(outputLine) * activeWidth;

        for (int32_t x = 0; x < activeWidth; x++) {
            outCB[x] = std::clamp(static_cast<float>(inU[x] * ecbScale),
                                  bounds.ecLo, bounds.ecHi);
            outCR[x] = std::clamp(static_cast<float>(inV[x] * ecrScale),
                                  bounds.ecLo, bounds.ecHi);
        }
    }
}

void OutputWriter::convertToFloatRGB(const ComponentFrame &componentFrame,
                                     std::vector<float> *outPlanes) const
{
    // Produce normalized R'G'B' planes (E'R plane 0, E'G plane 1, E'B plane 2)
    // direct from the ComponentFrame's composite-domain Y/U/V via the
    // BT.601/H.273 MatrixCoefficients=5/6 Y'CbCr→R'G'B' matrix. Skips the
    // intermediate Y'CbCr integer quantization the YUV444P16 / RGB48 paths
    // share. Geometry matches the integer convert() path (active crop +
    // top/bottom padding) so the planes line up pixel-for-pixel with the
    // other output formats.
    const size_t planeSize = static_cast<size_t>(activeWidth) * outputHeight;
    outPlanes[0].assign(planeSize, 0.0f);
    outPlanes[1].assign(planeSize, 0.0f);
    outPlanes[2].assign(planeSize, 0.0f);

    const double yOffset = videoParameters.black16bIre;
    const double yRange = videoParameters.white16bIre - videoParameters.black16bIre;
    const double uvRange = yRange;
    const double eyScale = 1.0 / yRange;
    const double uvScale = 1.0 / uvRange;
    // Composite-domain U/V → R'G'B' coefficients, derived from
    // U = kB*(B'-Y') and V = kR*(R'-Y'); see the integer RGB48 path for the
    // identical scalars.
    constexpr double R_FROM_V =  1.0 / kR;                                  //  1.139883
    constexpr double G_FROM_U = -(0.114 * BLUE_DIFFERENCE_SCALE / 0.587) / kB;
    constexpr double G_FROM_V = -(0.299 * RED_DIFFERENCE_SCALE  / 0.587) / kR;
    constexpr double B_FROM_U =  1.0 / kB;                                  //  2.032062
    const auto bounds = floatRGBBounds(config.clampMode);

    for (int32_t y = 0; y < activeHeight; y++) {
        const int32_t inputLine = videoParameters.firstActiveFrameLine + y;
        const double *inY = componentFrame.y(inputLine) + videoParameters.activeVideoStart;
        const double *inU = componentFrame.u(inputLine) + videoParameters.activeVideoStart;
        const double *inV = componentFrame.v(inputLine) + videoParameters.activeVideoStart;

        const int32_t outputLine = topPadLines + y;
        const size_t rowOff = static_cast<size_t>(outputLine) * activeWidth;
        float *outR = outPlanes[0].data() + rowOff;
        float *outG = outPlanes[1].data() + rowOff;
        float *outB = outPlanes[2].data() + rowOff;

        for (int32_t x = 0; x < activeWidth; x++) {
            const double rY = (inY[x] - yOffset) * eyScale;
            const double rU = inU[x] * uvScale;
            const double rV = inV[x] * uvScale;
            outR[x] = std::clamp(static_cast<float>(rY                  + R_FROM_V * rV), bounds.rLo, bounds.rHi);
            outG[x] = std::clamp(static_cast<float>(rY + G_FROM_U * rU + G_FROM_V * rV), bounds.gLo, bounds.gHi);
            outB[x] = std::clamp(static_cast<float>(rY + B_FROM_U * rU                 ), bounds.bLo, bounds.bHi);
        }
    }
}

}  // namespace chd::output
