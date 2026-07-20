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

namespace {
    // sin/cos of the 33 degree angle between the burst-locked U/V axes and
    // the I/Q axes.
    constexpr double kSin33 = 0.5446390350150271;
    constexpr double kCos33 = 0.838670567945424;
}

BurstInfo detectBurstRaw(const uint16_t *lineData,
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

    return BurstInfo{bsin, bcos};
}

BurstInfo detectBurst(const uint16_t *lineData,
                      const chd::metadata::LdDecodeMetaData::VideoParameters &videoParameters)
{
    BurstInfo info = detectBurstRaw(lineData, videoParameters);

    const double burstNorm =
        std::max(sqrt(info.bsin * info.bsin + info.bcos * info.bcos), kMinBurstAmplitude);

    info.bsin /= burstNorm;
    info.bcos /= burstNorm;

    return info;
}

BurstDeviation detectBurstDeviation(const uint16_t *lineData,
                                    const chd::metadata::LdDecodeMetaData::VideoParameters &videoParameters,
                                    const double nominalSign)
{
    BurstDeviation dev;

    const BurstInfo raw = detectBurstRaw(lineData, videoParameters);
    const double mag = std::sqrt(raw.bsin * raw.bsin + raw.bcos * raw.bcos);
    if (mag < kMinBurstAmplitude) return dev;

    // Product detection of A*cos(wt + phi) against the reference carrier
    // yields (bsin, bcos) = A/2 * (cos phi, sin phi), so the normalised
    // measurement is the (cos, sin) phasor of the burst phase.
    const double mCos = raw.bsin / mag;
    const double mSin = raw.bcos / mag;

    // Nominal burst phasor for this line: burst sits at 180 degrees on the
    // U axis, which the nominal carriers modulate to
    // sign * A * cos(wt - 33deg), giving the phasor sign * (cos33, -sin33).
    const double nCos = nominalSign * kCos33;
    const double nSin = -nominalSign * kSin33;

    // Rotation from the nominal phasor to the measured one.
    dev.cosDelta = mCos * nCos + mSin * nSin;
    dev.sinDelta = mSin * nCos - mCos * nSin;
    dev.valid = true;
    return dev;
}

}  // namespace chd::decoders