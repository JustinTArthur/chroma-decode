// SPDX-License-Identifier: GPL-3.0-or-later
//
// NTSC-1953 sideband-asymmetry calibration (BetaAccumulator + fit, and the
// chd_chroma_sideband_calibrate ABI argument validation). Synthesizes burst-locked
// composite fields at 4fSC whose I-axis content passes through a KNOWN
// vestige profile a(f) (raised cosine 1 → 0 across 0.40–0.70 MHz), plus
// real Q-axis tones and a noise floor, and verifies:
//
//   1. The fitted β profile recovers the truth: plateau ≈ 1, edge in the
//      right place, high coherence in the 0.70–1.00 MHz self-test band,
//      and the source classifies as wideband-I. This also pins the
//      estimator's sign conventions end-to-end.
//   2. A DSB control (same tones, symmetric sidebands) yields β ≈ 0 and
//      does NOT classify — the LD/equiband safety property.
//
// Tone phases are re-randomized per line so inter-tone spectral leakage
// decorrelates and the accumulator's Welch averaging works as it does on
// real content. Line-decorrelated chroma defeats the 2D comb's
// line-differencing, so the test drives the horizontal-only 1D comb; the
// ABI driver uses the 2D comb, which is correct for vertically-coherent
// real material. The hook and accumulator under test are identical.

#include <chromadec/calibration.h>

#include "../../src/decoders/comb/beta_calibration.h"
#include "../../src/decoders/comb/comb.h"
#include "../../src/decoders/source_field.h"
#include "../../src/metadata/core.h"
#include "../../src/output/component_frame.h"

#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <vector>

namespace {

#define REQUIRE(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #cond "\n"; \
        return 1; \
    } \
} while (0)

constexpr double FSC = 3.579545e6;
constexpr double FS = 4.0 * FSC;
constexpr double RAD33 = 33.0 * M_PI / 180.0;

constexpr int32_t BLACK = 16384;
constexpr int32_t WHITE = 54016;
constexpr double IRE = (WHITE - BLACK) / 100.0;

chd::metadata::LdDecodeMetaData::VideoParameters makeVideoParameters()
{
    chd::metadata::LdDecodeMetaData::VideoParameters vp;
    vp.system = chd::metadata::NTSC;
    vp.fieldWidth = 910;
    vp.fieldHeight = 263;
    vp.sampleRate = FS;
    vp.fSC = FSC;
    vp.colourBurstStart = 78;
    vp.colourBurstEnd = 110;
    vp.activeVideoStart = 134;
    vp.activeVideoEnd = 894;
    vp.black16bIre = BLACK;
    vp.white16bIre = WHITE;
    vp.firstActiveFrameLine = 40;
    vp.lastActiveFrameLine = 140;
    return vp;
}

struct Tone { double freq, amp, phase; };

// Deterministic per-line, per-tone phase scramble so leakage between tones
// decorrelates across accumulated lines. The line and tone terms must be
// multiplicatively coupled: with an additive hash the phase DIFFERENCE
// between two tones is constant across lines and their interference stays
// fully coherent instead of averaging out.
double linePhase(int32_t line, int32_t toneIndex)
{
    const double x = 0.6180339887 * (line + 1.0) * (toneIndex * toneIndex + 5.0);
    return 2.0 * M_PI * (x - std::floor(x));
}

// I-axis content through a vestige with upper-sideband gain a(f): the
// component (a+b)/2·m·cos(wt+33°) + (b−a)/2·m̂·sin(wt+33°), b = 1.
// a == 1 reduces to plain DSB; a == 0 to pure lower-sideband.
double vestigeChroma(int32_t i, const Tone &t, double a, double extraPhase)
{
    const double time = i / FS;
    const double m = t.amp * std::cos(2.0 * M_PI * t.freq * time + t.phase + extraPhase);
    const double mh = t.amp * std::sin(2.0 * M_PI * t.freq * time + t.phase + extraPhase);
    const double wt = M_PI_2 * i;
    return 0.5 * (a + 1.0) * m * std::cos(wt + RAD33)
         + 0.5 * (1.0 - a) * mh * std::sin(wt + RAD33);
}

double qChroma(int32_t i, const Tone &t, double extraPhase)
{
    const double time = i / FS;
    const double q = t.amp * std::cos(2.0 * M_PI * t.freq * time + t.phase + extraPhase);
    return q * std::sin(M_PI_2 * i + RAD33);
}

// chroma(line, i) gives the chroma waveform at horizontal sample i of the
// running line counter `line`; it is negated on alternate field lines
// (subcarrier flips 180° per line, like real NTSC), along with the burst.
std::vector<chd::decoders::SourceField>
makeFields(const chd::metadata::LdDecodeMetaData::VideoParameters &vp,
           const std::function<double(int32_t, int32_t)> &chroma,
           int32_t lineOffset)
{
    const double burstAmp = 20.0 * IRE;
    const double lumaPed = 40.0 * IRE;
    const double noiseAmp = 0.3 * IRE;
    uint32_t rng = 0x2bad1953u + static_cast<uint32_t>(lineOffset) * 2654435761u;
    const auto noise = [&]() {
        rng = rng * 1664525u + 1013904223u;
        return noiseAmp * ((static_cast<double>(rng >> 8) / 8388608.0) - 1.0);
    };

    chd::reader::Data fieldData;
    fieldData.reserve(static_cast<size_t>(vp.fieldHeight) * vp.fieldWidth);
    std::vector<chd::decoders::SourceField> fields(4);

    int32_t lineCounter = lineOffset;
    for (int32_t f = 2; f < 4; f++) {
        fieldData.clear();
        for (int32_t l = 0; l < vp.fieldHeight; l++, lineCounter++) {
            const double sign = (l % 2 == 0) ? 1.0 : -1.0;
            for (int32_t i = 0; i < vp.fieldWidth; i++) {
                double value = BLACK + noise();
                if (i >= vp.colourBurstStart && i < vp.colourBurstEnd) {
                    value += sign * -burstAmp * std::sin(M_PI_2 * i);
                } else if (i >= vp.activeVideoStart && i < vp.activeVideoEnd) {
                    value += lumaPed + sign * chroma(lineCounter, i);
                }
                fieldData.push_back(static_cast<uint16_t>(std::lround(value)));
            }
        }
        fields[f].data = fieldData;
        fields[f].field.isFirstField = (f % 2) == 0;
        fields[f].field.fieldPhaseID = 1;
    }
    return fields;
}

struct CalibrationRun {
    chd::decoders::comb::BetaAccumulator::FitResult fit;
    std::vector<chd::decoders::comb::BetaAccumulator::BinSample> bins;
};

// Accumulate `numFrames` frames of distinct content (per-line phases and
// noise keep evolving across frames) before fitting, like a real
// calibration pass over a capture.
CalibrationRun
calibrate(const chd::metadata::LdDecodeMetaData::VideoParameters &vp,
          const std::function<double(int32_t, int32_t)> &chroma,
          int32_t numFrames)
{
    using namespace chd::decoders::comb;
    BetaAccumulator accumulator;

    Comb::Configuration cfg;
    cfg.dimensions = 1;
    cfg.adaptive = false;
    cfg.phaseCompensation = true;
    cfg.betaAccumulator = &accumulator;

    Comb comb;
    comb.updateConfiguration(vp, cfg);
    for (int32_t f = 0; f < numFrames; f++) {
        const auto fields = makeFields(vp, chroma, f * 2 * vp.fieldHeight);
        std::vector<chd::output::ComponentFrame> frames(1);
        comb.decodeFrames(fields, 2, 4, frames);
    }
    return { accumulator.fit(), accumulator.bins() };
}

// Weight-averaged raw β̂ over bins within ±20 kHz of a tone frequency.
double binBetaNear(const CalibrationRun &run, double fHz)
{
    double num = 0.0, weight = 0.0;
    for (const auto &b : run.bins) {
        if (std::fabs(b.fHz - fHz) <= 20e3) {
            num += b.weight * b.betaHat;
            weight += b.weight;
        }
    }
    return (weight > 0.0) ? num / weight : 0.0;
}

// Truth profile: a(f) = raised cosine 1 → 0 across 0.40–0.70 MHz.
double vestigeProfile(double f)
{
    const double u = (f - 0.40e6) / 0.30e6;
    if (u <= 0.0) return 1.0;
    if (u >= 1.0) return 0.0;
    return 0.5 + 0.5 * std::cos(M_PI * u);
}

const Tone I_TONES[] = {
    {0.25e6, 8.0 * IRE, 0.3}, {0.35e6, 7.0 * IRE, 1.1}, {0.45e6, 8.0 * IRE, 2.0},
    {0.55e6, 7.0 * IRE, 0.7}, {0.65e6, 8.0 * IRE, 1.7}, {0.80e6, 9.0 * IRE, 0.2},
    {0.95e6, 8.0 * IRE, 2.6}, {1.10e6, 7.0 * IRE, 1.3},
};
const Tone Q_TONES[] = {
    {0.20e6, 8.0 * IRE, 0.9}, {0.40e6, 7.0 * IRE, 2.2},
};

int testWidebandISource()
{
    const auto vp = makeVideoParameters();
    const auto run = calibrate(vp, [&](int32_t line, int32_t i) {
        double c = 0.0;
        int32_t ti = 0;
        for (const auto &t : I_TONES) {
            c += vestigeChroma(i, t, vestigeProfile(t.freq), linePhase(line, ti++));
        }
        for (const auto &t : Q_TONES) {
            c += qChroma(i, t, linePhase(line, ti++));
        }
        return c;
    }, 10);
    const auto &fit = run.fit;

    std::cerr << "wideband-I fit: plateau=" << fit.plateau
              << " center=" << fit.edgeCenterHz / 1e6
              << " width=" << fit.edgeWidthHz / 1e6
              << " coherence=" << fit.coherence
              << " rms=" << fit.fitRms << "\n";
    REQUIRE(fit.linesAccumulated == 1010);
    // Recovers the truth: full lower-sideband plateau, edge where the β
    // curve implied by a(f) crosses half-plateau (~0.58 MHz), coherent
    // crosstalk in the self-test band, classified as wideband-I.
    REQUIRE(fit.plateau > 0.8 && fit.plateau <= 1.0);
    REQUIRE(fit.edgeCenterHz > 0.50e6 && fit.edgeCenterHz < 0.66e6);
    REQUIRE(fit.coherence > 0.7);
    REQUIRE(fit.fitRms < 0.15);
    REQUIRE(fit.isWidebandI);
    // Raw bins read the per-tone truth β = (1−a)/(1+a) directly.
    REQUIRE(std::fabs(binBetaNear(run, 0.45e6) - 0.034) < 0.05);
    REQUIRE(std::fabs(binBetaNear(run, 0.55e6) - (1.0 / 3.0)) < 0.05);
    REQUIRE(std::fabs(binBetaNear(run, 0.65e6) - 0.874) < 0.05);
    REQUIRE(std::fabs(binBetaNear(run, 0.95e6) - 1.0) < 0.05);
    return 0;
}

int testDsbControl()
{
    const auto vp = makeVideoParameters();
    const auto run = calibrate(vp, [&](int32_t line, int32_t i) {
        double c = 0.0;
        int32_t ti = 0;
        for (const auto &t : I_TONES) {
            c += vestigeChroma(i, t, 1.0, linePhase(line, ti++));  // symmetric
        }
        for (const auto &t : Q_TONES) {
            c += qChroma(i, t, linePhase(line, ti++));
        }
        return c;
    }, 10);
    // No sideband asymmetry ⇒ no quadrature cross-spectrum ⇒ β ≈ 0 and no
    // classification: the corrections this profile would one day drive must
    // never trigger on equiband/DSB material.
    REQUIRE(run.fit.plateau < 0.15);
    REQUIRE(!run.fit.isWidebandI);
    return 0;
}

int testAbiValidation()
{
    chd_chroma_sideband_calib_t result = {};
    REQUIRE(chd_chroma_sideband_calibrate(nullptr, 0, 0, &result) == CHD_E_INVALID_ARG);
    REQUIRE(chd_decoder_set_chroma_sideband_calib(nullptr, &result) == CHD_E_INVALID_ARG);
    return 0;
}

}  // namespace

int main()
{
    int failures = 0;
    failures += testWidebandISource();
    failures += testDsbControl();
    failures += testAbiValidation();
    if (failures == 0) std::cout << "PASS test_ntsc1953_calibration\n";
    return failures == 0 ? 0 : 1;
}
