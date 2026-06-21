// SPDX-License-Identifier: GPL-3.0-or-later
#include "chroma_filter.h"

#include <algorithm>
#include <cmath>

namespace chd::decoders {

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

// Standards-derived cutoffs (the §2.12 / ST 170 numbers in one place).
//   compat (PAL): the legacy 1.1/0.93 dot-pattern-tuned value (System-I +1066
//                 room) PALcolour has always shipped (kCompatPalChromaHz, in
//                 the header so the PalColour default shares it).
//   equiband:     1.3 MHz, the SMPTE ST 170 / BT.1700 / Clarke U/V mask shared
//                 by both systems.
//   color_under:  ~0.5 MHz, the surviving VHS/S-VHS colour-under bandwidth.
//   equiband_wide: the NTSC comb's ~2.2 MHz loose legacy default (informational
//                 here; the comb selects its own precomputed FIR).
constexpr double kEquibandWideHz = 2200000.0;
constexpr double kColorUnderHz  = 500000.0;

ChromaFilterResolution sym(double cutoffHz) {
    return {/*valid=*/true, /*symmetric=*/true, cutoffHz, nullptr};
}
ChromaFilterResolution recovery(double cutoffHz) {
    return {/*valid=*/true, /*symmetric=*/false, cutoffHz, nullptr};
}
ChromaFilterResolution invalid(const char *reason) {
    return {/*valid=*/false, /*symmetric=*/true, 0.0, reason};
}

bool isPal(chd::metadata::VideoSystem s) {
    return s == chd::metadata::PAL || s == chd::metadata::PAL_M;
}

}  // namespace

std::optional<ChromaFilter> parseChromaFilter(const std::string &name) {
    if (name == "compat")         return ChromaFilter::Compat;
    if (name == "equiband_wide")  return ChromaFilter::EquibandWide;
    if (name == "equiband")       return ChromaFilter::Equiband;
    if (name == "color_under")    return ChromaFilter::ColorUnder;
    if (name == "wideband_i_ssb") return ChromaFilter::WidebandISSB;
    if (name == "equiband_vsb")   return ChromaFilter::EquibandVsb;
    return std::nullopt;
}

const char *chromaFilterName(ChromaFilter f) {
    switch (f) {
        case ChromaFilter::Compat:       return "compat";
        case ChromaFilter::EquibandWide: return "equiband_wide";
        case ChromaFilter::Equiband:     return "equiband";
        case ChromaFilter::ColorUnder:   return "color_under";
        case ChromaFilter::WidebandISSB: return "wideband_i_ssb";
        case ChromaFilter::EquibandVsb:  return "equiband_vsb";
    }
    return "compat";
}

ChromaFilterResolution resolveChromaFilter(ChromaFilter f, chd::metadata::VideoSystem system) {
    const bool pal = isPal(system);
    switch (f) {
        case ChromaFilter::Compat:
            // System-resolved legacy default: NTSC ≡ equiband_wide (~2.2), PAL
            // the 1.1/0.93 value. Valid (and the unset default) on every system.
            return sym(pal ? kCompatPalChromaHz : kEquibandWideHz);
        case ChromaFilter::EquibandWide:
            if (pal)
                return invalid("equiband_wide is NTSC only; PAL's legacy "
                               "default is reached via compat");
            return sym(kEquibandWideHz);
        case ChromaFilter::Equiband:
            return sym(kEquibandCeilingHz);
        case ChromaFilter::ColorUnder:
            return sym(kColorUnderHz);
        case ChromaFilter::WidebandISSB:
            if (pal)
                return invalid("wideband_i_ssb is NTSC only; PAL/PAL-M use "
                               "equiband_vsb for vestige recovery");
            return recovery(kEquibandCeilingHz);
        case ChromaFilter::EquibandVsb:
            if (!pal)
                return invalid("equiband_vsb is PAL only; NTSC uses "
                               "wideband_i_ssb for vestige recovery");
            return recovery(kEquibandCeilingHz);
    }
    return invalid("unknown chroma_filter");
}

std::vector<double> synthesizeVsbEq(double upperSidebandHz, double ceilingHz, double sampleRate) {
    const double transition = ceilingHz - upperSidebandHz;   // > 0 (validated at commit)
    // Half-length scales with the sample rate over the +X→ceiling transition,
    // bounded so the per-line filter stays affordable. The physical sideband
    // rolloff is gradual, so an over-sharp design buys nothing.
    int32_t M = static_cast<int32_t>(std::lround(sampleRate / std::max(transition, 1.0)));
    M = std::clamp(M, 12, 128);
    const int32_t L = (2 * M) + 1;

    const auto desired = [&](double f) -> double {
        if (f <= upperSidebandHz) return 1.0;
        if (f >= ceilingHz)       return 2.0;
        const double t = (f - upperSidebandHz) / transition;   // 0..1
        return 1.0 + 0.5 * (1.0 - std::cos(M_PI * t));         // 1 → 2, raised cosine
    };

    // Zero-phase impulse response: h[d] = (1/π) ∫₀^π H(ω) cos(ω·d) dω, by
    // midpoint Riemann sum over a dense ω grid.
    constexpr int32_t kGrid = 4096;
    std::vector<double> h(L, 0.0);
    for (int32_t d = 0; d <= M; d++) {
        double acc = 0.0;
        for (int32_t k = 0; k < kGrid; k++) {
            const double omega = M_PI * (k + 0.5) / kGrid;
            const double f = omega * sampleRate / (2.0 * M_PI);
            acc += desired(f) * std::cos(omega * d);
        }
        acc /= kGrid;
        h[M + d] = acc;
        h[M - d] = acc;
    }
    // Hann window, then normalise DC gain to exactly 1.
    for (int32_t n = 0; n < L; n++) {
        h[n] *= 0.5 - 0.5 * std::cos(2.0 * M_PI * n / (L - 1));
    }
    double sum = 0.0;
    for (double v : h) sum += v;
    if (sum != 0.0) {
        for (double &v : h) v /= sum;
    }
    return h;
}

}  // namespace chd::decoders
