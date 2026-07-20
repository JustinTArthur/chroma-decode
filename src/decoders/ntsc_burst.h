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

#include <array>
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

// Minimum averaged burst amplitude, in 16-bit sample units, below which a
// line's burst is too weak to trust. detectBurst uses it as a norm floor so
// weak bursts shrink toward zero chroma; detectBurstDeviation reports the
// line invalid instead.
inline constexpr double kMinBurstAmplitude = 130000.0 / 128;

// Measured colour-burst vector for one line.
struct BurstInfo {
    double bsin, bcos;
};

// Product-detect the burst against the reference carrier, averaged over the
// burst window with no normalisation applied.
BurstInfo detectBurstRaw(const uint16_t *lineData,
                         const chd::metadata::LdDecodeMetaData::VideoParameters &videoParameters);

// detectBurstRaw normalised to a unit vector, with an amplitude floor so weak
// or absent bursts yield a proportionally shortened vector rather than
// amplified noise.
BurstInfo detectBurst(const uint16_t *lineData,
                      const chd::metadata::LdDecodeMetaData::VideoParameters &videoParameters);

// Integer-pixel I/Q carrier samples at 4fsc: the ldzeug2 reference carriers
// cos(wt - 90deg) and sin(wt - 90deg) before the per-line carrier sign.
inline constexpr std::array<double, 4> kNtscCarrierI = { 0.0,  1.0, 0.0, -1.0 };
inline constexpr std::array<double, 4> kNtscCarrierQ = {-1.0,  0.0, 1.0,  0.0 };

// Rotation of a line's measured burst away from the phase the nominal
// field/line carrier-sign table implies. The identity-rotation default means
// an invalid measurement leaves callers on the nominal carriers.
struct BurstDeviation {
    double cosDelta = 1.0;
    double sinDelta = 0.0;
    bool valid = false;
};

// Measure the burst on lineData and return its rotation relative to the
// nominal burst phase for a line whose carrier sign is nominalSign (+1 or
// -1, from the field-phase table). valid is false when the burst amplitude
// is below kMinBurstAmplitude.
BurstDeviation detectBurstDeviation(const uint16_t *lineData,
                                    const chd::metadata::LdDecodeMetaData::VideoParameters &videoParameters,
                                    double nominalSign);

// Carrier pair at sample x, rotated by dev and signed by the line's nominal
// carrier sign. With the identity rotation this reproduces the nominal
// lattice carriers exactly.
inline void burstLockedCarrier(const int32_t x, const BurstDeviation &dev, const double sign,
                               double *ic, double *qc) {
    const double i0 = kNtscCarrierI[x % 4];
    const double q0 = kNtscCarrierQ[x % 4];
    *ic = (i0 * dev.cosDelta - q0 * dev.sinDelta) * sign;
    *qc = (q0 * dev.cosDelta + i0 * dev.sinDelta) * sign;
}

// The output-side equivalent: correct an I/Q pair that was demodulated
// against the nominal carriers when the signal actually sat at the measured
// phase. Demodulating that way yields the chroma vector rotated by -delta,
// so recovering it takes the same +delta rotation burstLockedCarrier applies
// to the carriers. Decoders can use whichever side they have access to.
inline void correctDemodulatedIQ(const BurstDeviation &dev, double *i, double *q) {
    const double i0 = *i;
    const double q0 = *q;
    *i = i0 * dev.cosDelta - q0 * dev.sinDelta;
    *q = q0 * dev.cosDelta + i0 * dev.sinDelta;
}

}  // namespace chd::decoders

#endif  // CHD_DECODERS_NTSC_BURST_H
