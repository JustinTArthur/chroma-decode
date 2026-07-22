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
    // paths, in output-frame rows (so the first rows include any top pad
    // border). The planes weave the two fields the way the luma plane does:
    // a plane row's parity matches the parity of the output-frame row it was
    // decoded from, so heights are equal and *FirstRow is the source row of
    // plane row 0 (the even-parity field's first line, not necessarily the
    // plane's topmost).
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
        int32_t paddingAmount = 1;
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

    // Set the output configuration. The active crop in videoParameters selects
    // the picture; padding surrounds it and never moves it, so this does not
    // modify videoParameters.
    void updateConfiguration(const chd::metadata::LdDecodeMetaData::VideoParameters &videoParameters, const Configuration &config);

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
    // component, woven by output-row parity: plane row 2j+p is the j-th row
    // of that component on output rows of parity p, so row parity selects
    // the same field on every plane. The integer form appends [Y | Cb | Cr]
    // into outputFrame; the float form fills outPlanes[0..2]. Both require
    // a populated chromaRowComponents map covering the active region and an
    // active height that is a multiple of 4 (chd_decoder_commit enforces
    // this for the 4:4:0 formats).
    // Padding pads the Y plane on both axes; the chroma planes share the
    // padded width (neutral side border) but never gain rows, so every
    // chroma plane row stays a real decoded line.
    Chroma440Geometry convert440(const ComponentFrame &componentFrame,
                                 OutputFrame &outputFrame) const;
    Chroma440Geometry convertToFloat440(const ComponentFrame &componentFrame,
                                        std::vector<float> *outPlanes) const;

    PixelFormat getPixelFormat() const {
        return config.pixelFormat;
    }

    // Committed output geometry (valid after updateConfiguration).
    // activeWidth/activeHeight are the picture crop; outputWidth/outputHeight
    // are the emitted frame, which is the crop surrounded by the black border
    // Configuration::paddingAmount asks for. The picture sits at
    // (leftPadSamples, topPadLines) within the frame.
    int32_t getActiveWidth() const { return activeWidth; }
    int32_t getActiveHeight() const { return activeHeight; }
    int32_t getOutputWidth() const { return outputWidth; }
    int32_t getOutputHeight() const { return outputHeight; }
    int32_t getLeftPadSamples() const { return leftPadSamples; }
    int32_t getRightPadSamples() const { return rightPadSamples; }
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

    // Black border surrounding the active picture
    int32_t leftPadSamples;
    int32_t rightPadSamples;
    int32_t topPadLines;
    int32_t bottomPadLines;

    // Picture crop, and the padded frame that carries it
    int32_t activeWidth;
    int32_t activeHeight;
    int32_t outputWidth;
    int32_t outputHeight;

    // True when Configuration::paddingAmount grew the frame on any side
    bool isPadded() const {
        return leftPadSamples != 0 || rightPadSamples != 0
            || topPadLines != 0 || bottomPadLines != 0;
    }

    // Get a string representing the pixel format
    const char *getPixelName() const;

    // Fill the whole output frame with black, before the active lines are
    // written into its interior
    void fillBlack(OutputFrame &outputFrame) const;

    // Convert one line
    void convertLine(int32_t lineNumber, const ComponentFrame &componentFrame, OutputFrame &outputFrame) const;
};

}  // namespace chd::output

#endif  // CHD_OUTPUT_OUTPUT_WRITER_H
