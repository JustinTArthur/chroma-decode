// SPDX-License-Identifier: GPL-3.0-or-later
/************************************************************************

    outputwriter.h

    ld-chroma-decoder - Colourisation filter for ld-decode
    Copyright (C) 2019-2021 Adam Sampson
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

#ifndef CHD_OUTPUT_OUTPUT_WRITER_H
#define CHD_OUTPUT_OUTPUT_WRITER_H

#include <cstdint>
#include <string>
#include <vector>

#include "../common/color_conversion.h"
#include "../metadata/core.h"

namespace chd::output {

class ComponentFrame;

// A frame (two interlaced fields), converted to one of the supported output formats.
// Since all the formats currently supported use 16-bit samples, this is just a
// vector of 16-bit numbers.
using OutputFrame = std::vector<uint16_t>;

class OutputWriter {
public:
    // Output pixel formats
    enum PixelFormat {
        RGB48 = 0,
        YUV444P16,
        GRAY16,
        YUV440P16
    };

    // Geometry of the two 4:4:0 chroma planes produced by the convert440
    // paths, in output-frame rows. Heights differ by at most one; each
    // plane's rows keep the frame's top-to-bottom order.
    struct Chroma440Geometry {
        int32_t cbHeight = 0;
        int32_t crHeight = 0;
        int32_t cbFirstRow = 0;
        int32_t crFirstRow = 0;
    };

    // Output sample clamping mode.
    enum ClampMode {
        CLAMP_NONE = 0,
        CLAMP_LEGAL_RGB_SDR,
        CLAMP_LEGAL_RGB_HDR,
        CLAMP_LEGAL_YCBCR_BT601
    };

    // Output settings
    struct Configuration {
        int32_t paddingAmount = 8;
        PixelFormat pixelFormat = RGB48;
        ClampMode clampMode = CLAMP_NONE;
        bool outputY4m = false;
        // Precision of the two coefficient families the conversions apply.
        // The defaults reproduce ld-chroma-decoder.
        chd::color::ColorDifferencePrecision colorDifferencePrecision =
            chd::color::ColorDifferencePrecision::Modern;
        chd::color::BroadcastScalingPrecision broadcastScalingPrecision =
            chd::color::BroadcastScalingPrecision::Scientific;
    };

    // Set the output configuration, and adjust the VideoParameters to suit.
    // (If usePadding is disabled, this will not change the VideoParameters.)
    void updateConfiguration(chd::metadata::LdDecodeMetaData::VideoParameters &videoParameters, const Configuration &config);

    // Print an info message about the output format
    void printOutputInfo() const;

    // Get the header data to be written at the start of the stream
    std::string getStreamHeader() const;

    // Get the header data to be written before each frame
    std::string getFrameHeader() const;

    // For worker threads: convert a component frame to the configured output format
    void convert(const ComponentFrame &componentFrame, OutputFrame &outputFrame) const;

    // For worker threads: convert a component frame to normalized float planes
    // E′Y (plane 0) and, when includeChroma is set, E′Cb (plane 1) / E′Cr
    // (plane 2). outPlanes must point to an array of three vectors; they are
    // resized to the committed output geometry. Used by the CHD_PIXEL_YUV444PS
    // and CHD_PIXEL_GRAYS output formats. The integer convert() path quantizes
    // these same signals.
    void convertToFloat(const ComponentFrame &componentFrame,
                        std::vector<float> *outPlanes, bool includeChroma) const;

    // For worker threads: convert a component frame to normalized float
    // R′G′B′ planes (E′R plane 0, E′G plane 1, E′B plane 2). Computed direct
    // from the ComponentFrame via the configured colour conversion; no
    // intermediate Y′CbCr integer quantization. Used by CHD_PIXEL_RGBS.
    void convertToFloatRGB(const ComponentFrame &componentFrame,
                           std::vector<float> *outPlanes) const;

    // 4:4:0 paths: full-height Y plus subsampled Cb/Cr planes holding only
    // the rows the frame's chromaRowComponents map assigns to each
    // component, in frame order. The integer form appends [Y | Cb | Cr]
    // into outputFrame; the float form fills outPlanes[0..2]. Both require
    // a populated chromaRowComponents map covering the active region.
    Chroma440Geometry convert440(const ComponentFrame &componentFrame,
                                 OutputFrame &outputFrame) const;
    Chroma440Geometry convertToFloat440(const ComponentFrame &componentFrame,
                                        std::vector<float> *outPlanes) const;

    PixelFormat getPixelFormat() const {
        return config.pixelFormat;
    }

    // Committed output geometry (valid after updateConfiguration). These reflect
    // any padding requested via Configuration::paddingAmount: activeWidth is the
    // horizontally-expanded crop, outputHeight includes top/bottom pad lines.
    int32_t getActiveWidth() const { return activeWidth; }
    int32_t getActiveHeight() const { return activeHeight; }
    int32_t getOutputHeight() const { return outputHeight; }
    int32_t getTopPadLines() const { return topPadLines; }
    int32_t getBottomPadLines() const { return bottomPadLines; }

private:
    // Configuration parameters
    Configuration config;
    chd::metadata::LdDecodeMetaData::VideoParameters videoParameters;

    // config's two precision selections, resolved to the scalars every
    // convert path applies per sample. Rebuilt by updateConfiguration.
    chd::color::ColorConversion conversion = chd::color::resolveColorConversion(
        chd::color::ColorDifferencePrecision::Modern,
        chd::color::BroadcastScalingPrecision::Scientific);

    // Number of blank lines to add at the top and bottom of the output
    int32_t topPadLines;
    int32_t bottomPadLines;

    // Output size
    int32_t activeWidth;
    int32_t activeHeight;
    int32_t outputHeight;

    // Get a string representing the pixel format
    const char *getPixelName() const;

    // Clear padding lines
    void clearPadLines(int32_t firstLine, int32_t numLines, OutputFrame &outputFrame) const;

    // Convert one line
    void convertLine(int32_t lineNumber, const ComponentFrame &componentFrame, OutputFrame &outputFrame) const;
};

}  // namespace chd::output

#endif  // CHD_OUTPUT_OUTPUT_WRITER_H
