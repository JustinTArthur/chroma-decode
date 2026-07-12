// SPDX-License-Identifier: GPL-3.0-or-later
/************************************************************************

    componentframe.h

    ld-chroma-decoder - Colourisation filter for ld-decode
    Copyright (C) 2021 Adam Sampson

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

#ifndef CHD_OUTPUT_COMPONENT_FRAME_H
#define CHD_OUTPUT_COMPONENT_FRAME_H

#include <cassert>
#include <cstdint>
#include <vector>

#include "../metadata/core.h"

namespace chd::output {

// Two complete, interlaced fields' worth of decoded luma and chroma information.
//
// The luma and chroma samples have the same scaling as in the original
// composite signal (i.e. they're not in Y'CbCr form yet). You can recover the
// chroma signal by subtracting Y from the composite signal.
class ComponentFrame
{
public:
    ComponentFrame();

    // Set the frame's size and clear it to black
    // If mono is true, only Y set to black, while U and V are cleared.
    void init(const chd::metadata::LdDecodeMetaData::VideoParameters &videoParameters, bool mono=false);

    // Get a pointer to a line of samples. Line numbers are 0-based within the frame.
    // Lines are stored in a contiguous array, so it's safe to get a pointer to
    // line 0 and use it to refer to later lines.
    double *y(int32_t line) {
        return yData.data() + getLineOffset(line);
    }
    double *u(int32_t line) {
        return uData.data() + getLineOffsetUV(line);
    }
    double *v(int32_t line) {
        return vData.data() + getLineOffsetUV(line);
    }
    const double *y(int32_t line) const {
        return yData.data() + getLineOffset(line);
    }
    const double *u(int32_t line) const {
        return uData.data() + getLineOffsetUV(line);
    }
    const double *v(int32_t line) const {
        return vData.data() + getLineOffsetUV(line);
    }
	
	std::vector<double>* getY(){
		return &yData;
	}
	
	std::vector<double>* getU(){
		return &uData;
	}
	
	std::vector<double>* getV(){
		return &vData;
	}
	
	void setY(std::vector<double>& _yData){
		yData = _yData;
	}
	
	void setU(std::vector<double>& _uData){
		uData = _uData;
	}
	
	void setV(std::vector<double>& _vData){
		vData = _vData;
	}

    int32_t getWidth() const {
        return width;
    }
    int32_t getHeight() const {
        return height;
    }

    // Line-sequential (SECAM) chroma lattice: per frame row, the colour-
    // difference component that row's chroma carries (0 = Db, 1 = Dr, -1 =
    // no chroma decoded for the row). Empty for simultaneous-chroma decodes.
    std::vector<int8_t> chromaRowComponents;

    // Ident measurement summary backing chd_frame_get_chroma_ident.
    // `mechanism` matches chd_chroma_ident_mechanism_t.
    struct ChromaIdent {
        bool valid = false;
        int32_t mechanism = 0;
        double confidence = 0.0;
        double fieldConfidence[2] = {0.0, 0.0};
    };
    ChromaIdent chromaIdent;

    // FM click concealment results: sample spans (frame rows, full-line
    // sample coordinates) whose chroma was concealed rather than decoded,
    // and the effective thresholds that were applied.
    struct ChromaConcealedSpan {
        int32_t frameRow;
        int32_t xStart;
        int32_t xEnd;
    };
    std::vector<ChromaConcealedSpan> chromaConcealedSpans;
    struct ChromaClickThresholds {
        bool valid = false;
        double envDipDb = 0.0;
        double freqOvershoot = 0.0;
    };
    ChromaClickThresholds chromaClick;

private:
    int32_t getLineOffset(int32_t line) const {
        assert(line >= 0);
        assert(static_cast<size_t>(line) < yData.size());
        return line * width;
    }

    int32_t getLineOffsetUV(int32_t line) const {
        assert(line >= 0);
        assert(static_cast<size_t>(line) < uData.size());
        return line * width;
    }

    // Size of the frame
    int32_t width;
    int32_t height;

    // Samples for Y, U and V
    std::vector<double> yData;
    std::vector<double> uData;
    std::vector<double> vData;
};

}  // namespace chd::output

#endif  // CHD_OUTPUT_COMPONENT_FRAME_H
