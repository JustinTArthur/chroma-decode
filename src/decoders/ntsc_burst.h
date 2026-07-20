// SPDX-License-Identifier: GPL-3.0-or-later
/************************************************************************

    ntsc_burst.h

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

// NTSC colour-burst measurement at 4fSC, shared by the comb decoder's
// burst-locked demodulation and the ldzeug2 decoders' phase compensation.

#ifndef CHD_DECODERS_NTSC_BURST_H
#define CHD_DECODERS_NTSC_BURST_H

#include <cstdint>

#include "../metadata/core.h"

namespace chd::decoders {

// Since we are at exactly 4fsc, calculating the value of a in-phase sine wave
// at a specific position is very simple.
inline constexpr double sin4fsc_data[] = {1.0, 0.0, -1.0, 0.0};

// 4fsc sine wave
constexpr double sin4fsc(const int32_t i) {
    return sin4fsc_data[i % 4];
}

// 4fsc cos wave
constexpr double cos4fsc(const int32_t i) {
    // cos(rad) is just sin(rad + pi/2) and we are at 4 fsc.
    return sin4fsc(i + 1);
}

// Measured colour-burst vector for one line.
struct BurstInfo {
    double bsin, bcos;
};

// Product-detect the burst against the reference carrier and normalise the
// result to a unit vector, with an amplitude floor so weak or absent bursts
// yield a proportionally shortened vector rather than amplified noise.
BurstInfo detectBurst(const uint16_t *lineData,
                      const chd::metadata::LdDecodeMetaData::VideoParameters &videoParameters);

}  // namespace chd::decoders

#endif  // CHD_DECODERS_NTSC_BURST_H
