// SPDX-License-Identifier: GPL-3.0-or-later
/************************************************************************

    paldecoder.cpp

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

#include "pal_decoder.h"

#include "../../common/log.h"

namespace chd::decoders::palcolour {

PalDecoder::PalDecoder(const PalColour::Configuration &palConfig)
{
    config.pal = palConfig;
}

bool PalDecoder::configure(const chd::metadata::LdDecodeMetaData::VideoParameters &videoParameters) {
    // Ensure the source video is PAL
    if (videoParameters.system != chd::metadata::PAL && videoParameters.system != chd::metadata::PAL_M) {
        chd::log::fail() << "This decoder is for PAL video sources only";
        return false;
    }

    config.videoParameters = videoParameters;

    // Configure PALcolour now that we know the video parameters.
    palColour.updateConfiguration(config.videoParameters, config.pal);

    return true;
}

int32_t PalDecoder::getLookBehind() const
{
    return config.pal.getLookBehind();
}

int32_t PalDecoder::getLookAhead() const
{
    return config.pal.getLookAhead();
}

void PalDecoder::decodeFrames(const std::vector<chd::decoders::SourceField> &inputFields,
                              int32_t startIndex, int32_t endIndex,
                              std::vector<chd::output::ComponentFrame> &componentFrames)
{
    palColour.decodeFrames(inputFields, startIndex, endIndex, componentFrames);
}

}  // namespace chd::decoders::palcolour
