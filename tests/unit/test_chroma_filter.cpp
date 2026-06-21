// SPDX-License-Identifier: GPL-3.0-or-later
//
// Cross-system chroma-filter table + equiband_vsb EQ synthesis.
//   1. parseChromaFilter / chromaFilterName round-trip + unknowns.
//   2. resolveChromaFilter (intent, system) validity matrix and the
//      standards-derived cutoffs (the single source those numbers live in).
//   3. synthesizeVsbEq frequency response: unity DC and below +X, ~+6 dB at the
//      equiband ceiling (the amplitude shelf that lifts the half-amplitude
//      vestige back to full).

#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

#include "../../src/decoders/chroma_filter.h"

namespace {

#define REQUIRE(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #cond "\n"; \
        return 1; \
    } \
} while (0)

using chd::decoders::ChromaFilter;
using chd::decoders::resolveChromaFilter;
using chd::metadata::NTSC;
using chd::metadata::PAL;
using chd::metadata::PAL_M;

int testParseRoundTrip() {
    const ChromaFilter all[] = {
        ChromaFilter::Compat, ChromaFilter::EquibandWide, ChromaFilter::Equiband,
        ChromaFilter::ColorUnder, ChromaFilter::WidebandISSB, ChromaFilter::EquibandVsb,
    };
    for (auto f : all) {
        auto parsed = chd::decoders::parseChromaFilter(chd::decoders::chromaFilterName(f));
        REQUIRE(parsed.has_value());
        REQUIRE(*parsed == f);
    }
    REQUIRE(!chd::decoders::parseChromaFilter("nonsense").has_value());
    REQUIRE(!chd::decoders::parseChromaFilter("").has_value());
    REQUIRE(!chd::decoders::parseChromaFilter("wideband_i").has_value());  // internal-only, no ABI string
    return 0;
}

int testValidityMatrix() {
    // compat is valid (and the default) on every system.
    REQUIRE(resolveChromaFilter(ChromaFilter::Compat, NTSC).valid);
    REQUIRE(resolveChromaFilter(ChromaFilter::Compat, PAL).valid);
    REQUIRE(resolveChromaFilter(ChromaFilter::Compat, PAL_M).valid);

    // The shared equiband ladder is valid on both systems.
    for (auto sys : {NTSC, PAL, PAL_M}) {
        REQUIRE(resolveChromaFilter(ChromaFilter::Equiband, sys).valid);
        REQUIRE(resolveChromaFilter(ChromaFilter::ColorUnder, sys).valid);
    }

    // equiband_wide and wideband_i_ssb are NTSC-only.
    REQUIRE(resolveChromaFilter(ChromaFilter::EquibandWide, NTSC).valid);
    REQUIRE(!resolveChromaFilter(ChromaFilter::EquibandWide, PAL).valid);
    REQUIRE(!resolveChromaFilter(ChromaFilter::EquibandWide, PAL_M).valid);
    REQUIRE(resolveChromaFilter(ChromaFilter::WidebandISSB, NTSC).valid);
    REQUIRE(!resolveChromaFilter(ChromaFilter::WidebandISSB, PAL).valid);

    // equiband_vsb is PAL-only.
    REQUIRE(!resolveChromaFilter(ChromaFilter::EquibandVsb, NTSC).valid);
    REQUIRE(resolveChromaFilter(ChromaFilter::EquibandVsb, PAL).valid);
    REQUIRE(resolveChromaFilter(ChromaFilter::EquibandVsb, PAL_M).valid);

    // Invalid cells carry a reason for the commit error.
    REQUIRE(resolveChromaFilter(ChromaFilter::EquibandWide, PAL).invalidReason != nullptr);
    REQUIRE(resolveChromaFilter(ChromaFilter::EquibandVsb, NTSC).invalidReason != nullptr);
    return 0;
}

int testCutoffsAndSymmetry() {
    // The symmetric modes are symmetric; the two recovery modes are not (they
    // consume the +X geometry).
    REQUIRE(resolveChromaFilter(ChromaFilter::Compat, PAL).symmetric);
    REQUIRE(resolveChromaFilter(ChromaFilter::Equiband, PAL).symmetric);
    REQUIRE(resolveChromaFilter(ChromaFilter::ColorUnder, PAL).symmetric);
    REQUIRE(!resolveChromaFilter(ChromaFilter::WidebandISSB, NTSC).symmetric);
    REQUIRE(!resolveChromaFilter(ChromaFilter::EquibandVsb, PAL).symmetric);

    // PAL compat is the legacy 1.1/0.93 dot-pattern-tuned value, byte-for-byte.
    REQUIRE(std::fabs(resolveChromaFilter(ChromaFilter::Compat, PAL).cutoffHz
                      - (1100000.0 / 0.93)) < 1e-6);
    // equiband is the shared 1.3 MHz ST 170 / BT.1700 ceiling, same on both.
    REQUIRE(std::fabs(resolveChromaFilter(ChromaFilter::Equiband, NTSC).cutoffHz - 1300000.0) < 1.0);
    REQUIRE(std::fabs(resolveChromaFilter(ChromaFilter::Equiband, PAL).cutoffHz - 1300000.0) < 1.0);
    // color_under is ~0.5 MHz.
    REQUIRE(std::fabs(resolveChromaFilter(ChromaFilter::ColorUnder, PAL).cutoffHz - 500000.0) < 1.0);
    // The recovery cutoff is the equiband ceiling (the band the EQ lifts up to).
    REQUIRE(std::fabs(resolveChromaFilter(ChromaFilter::EquibandVsb, PAL).cutoffHz - 1300000.0) < 1.0);

    // NTSC compat resolves wider than PAL compat (≡ equiband_wide ~2.2 MHz).
    REQUIRE(resolveChromaFilter(ChromaFilter::Compat, NTSC).cutoffHz
            > resolveChromaFilter(ChromaFilter::Compat, PAL).cutoffHz);
    return 0;
}

// Zero-phase amplitude response of a type-I (symmetric, odd-length) FIR at fHz.
double ampResponse(const std::vector<double> &h, double fHz, double sampleRate) {
    const int L = static_cast<int>(h.size());
    const int M = (L - 1) / 2;
    const double w = 2.0 * M_PI * fHz / sampleRate;
    double a = h[M];
    for (int d = 1; d <= M; d++) a += 2.0 * h[M + d] * std::cos(w * d);
    return a;
}

int testVsbEqResponse() {
    // 4·fSC PAL, System-I geometry (+1066 kHz upper room, 1.3 MHz ceiling).
    const double fs = 17734475.0;
    const double ceil = 1300000.0;
    const double x = 1066000.0;
    auto h = chd::decoders::synthesizeVsbEq(x, ceil, fs);

    REQUIRE(h.size() % 2 == 1);          // odd length (linear phase)

    const double dc   = ampResponse(h, 0.0, fs);
    const double atLo = ampResponse(h, 300000.0, fs);   // well below +X: untouched
    const double atX  = ampResponse(h, x, fs);
    const double atCeil = ampResponse(h, ceil, fs);

    std::cout << "  VSB EQ (+1066k): DC=" << dc << " 0.3M=" << atLo
              << " +X=" << atX << " ceil=" << atCeil << " taps=" << h.size() << "\n";

    REQUIRE(std::fabs(dc - 1.0) < 1e-6);     // DC normalised to unity exactly
    REQUIRE(std::fabs(atLo - 1.0) < 0.05);   // bulk chroma untouched below +X
    REQUIRE(atX > 0.9 && atX < 1.3);         // ramp just starting at the edge
    REQUIRE(atCeil > 1.6 && atCeil <= 2.05); // ~+6 dB at the ceiling
    REQUIRE(atCeil > atX + 0.4);             // a clear, monotone boost across the vestige

    // A wider vestige (smaller +X) boosts a wider band, so its mid-band gain at
    // a fixed frequency is no less than the narrow-vestige design's.
    auto hWide = chd::decoders::synthesizeVsbEq(570000.0, ceil, fs);
    REQUIRE(ampResponse(hWide, 800000.0, fs) >= ampResponse(h, 800000.0, fs) - 1e-9);
    return 0;
}

}  // namespace

int main() {
    if (testParseRoundTrip() != 0) return 1;
    if (testValidityMatrix() != 0) return 1;
    if (testCutoffsAndSymmetry() != 0) return 1;
    if (testVsbEqResponse() != 0) return 1;
    std::cout << "All chroma-filter table + VSB EQ tests passed.\n";
    return 0;
}
