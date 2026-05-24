// SPDX-License-Identifier: GPL-3.0-or-later
/************************************************************************

    paldecoder.h

    ld-chroma-decoder - Colourisation filter for ld-decode
    Copyright (C) 2018-2019 Simon Inns
    Copyright (C) 2019-2021 Adam Sampson

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

#ifndef CHD_DECODERS_PALCOLOUR_PAL_DECODER_H
#define CHD_DECODERS_PALCOLOUR_PAL_DECODER_H

#include <cstdint>
#include <vector>

#include "../../output/component_frame.h"
#include "../../metadata/core.h"
#include "../../reader/tbc_source.h"

#include "../decoder_base.h"
#include "../source_field.h"
#include "palcolour.h"

namespace chd::decoders::palcolour {

// 2D PAL decoder using PALcolour
class PalDecoder : public chd::decoders::Decoder {
public:
    PalDecoder(const PalColour::Configuration &palConfig);
    bool configure(const chd::metadata::LdDecodeMetaData::VideoParameters &videoParameters) override;
    int32_t getLookBehind() const override;
    int32_t getLookAhead() const override;

    void decodeFrames(const std::vector<chd::decoders::SourceField> &inputFields,
                      int32_t startIndex, int32_t endIndex,
                      std::vector<chd::output::ComponentFrame> &componentFrames) override;

    // Parameters used by PalDecoder
    struct Configuration : public chd::decoders::Decoder::Configuration {
        PalColour::Configuration pal;
    };

private:
    Configuration config;
    PalColour palColour;
};

}  // namespace chd::decoders::palcolour

#endif  // CHD_DECODERS_PALCOLOUR_PAL_DECODER_H
