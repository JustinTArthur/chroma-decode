// SPDX-License-Identifier: GPL-3.0-or-later
/************************************************************************

    ntsc_burst.cpp

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

#include "ntsc_burst.h"

#include <algorithm>
#include <cmath>

namespace chd::decoders {

BurstInfo detectBurst(const uint16_t *lineData,
                      const chd::metadata::LdDecodeMetaData::VideoParameters &videoParameters)
{
    double bsin = 0, bcos = 0;

    // Find absolute burst phase relative to the reference carrier by
    // product detection.
    // For now we just use the burst on the current line, but we could possibly do some averaging with
    // neighbouring lines later if needed.
    for (int32_t i = videoParameters.colourBurstStart; i < videoParameters.colourBurstEnd; i++) {
        bsin += lineData[i] * sin4fsc(i);
        bcos += lineData[i] * cos4fsc(i);
    }

    // Normalise the sums above
    const int32_t colourBurstLength = videoParameters.colourBurstEnd - videoParameters.colourBurstStart;
    bsin /= colourBurstLength;
    bcos /= colourBurstLength;

    const double burstNorm = std::max(sqrt(bsin * bsin + bcos * bcos), 130000.0 / 128);

    bsin /= burstNorm;
    bcos /= burstNorm;

    const BurstInfo info{bsin, bcos};
    return info;
}

}  // namespace chd::decoders