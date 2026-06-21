// SPDX-License-Identifier: GPL-3.0-or-later
//
// NTSC-1953 SSB wideband-I reconstruction (ntsc_chroma_filter
// "wideband_i_ssb"). Synthesizes burst-locked composite fields at 4fSC and
// decodes them through the 1D comb with phase compensation:
//
//   1. An I-axis tone at 1.0 MHz transmitted lower-sideband-only (how
//      NTSC-1953 carries wideband I above ~0.6 MHz). Synchronous demod
//      recovers it at half amplitude; the SSB mode must restore full
//      amplitude (~2.6x the plain wideband_i result, which is halved AND
//      drooped by the chroma prefilter the SSB mode equalizes), with no
//      residue on Q. This also pins the sign of the Hilbert cross-feed:
//      the wrong sign cancels instead of restoring, leaving ~zero.
//   2. A double-sideband I-axis tone at 0.3 MHz: the Hilbert path must not
//      disturb ordinary narrowband chroma (matches wideband_i closely).
//   3. A double-sideband Q-axis tone at 0.3 MHz: real Q energy must not
//      leak onto the I axis through the Hilbert path.
//   4. A double-sideband Q-axis tone at 0.55 MHz — inside Q's spec
//      allocation but above the old Hilbert skirt's fade-in. The steep
//      skirt must keep it off the I axis.
//   5. β-profile corrections: a vestigial (half-upper-sideband) I tone in
//      the 0.4–0.7 MHz transition strip decodes at ~3/4 amplitude with
//      crosstalk left in Q without a profile; with the matching profile
//      attached, I recovers ~full amplitude and the Q ghost nulls. DSB
//      content below the ramp and LSB content above the skirt must be
//      (essentially) unaffected by an active profile.

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

// chroma(i) is the chroma waveform in 16-bit units at sample i (the same
// horizontal sample clock on every line).
std::vector<chd::decoders::SourceField>
makeFields(const chd::metadata::LdDecodeMetaData::VideoParameters &vp,
           const std::function<double(int32_t)> &chroma)
{
    const double burstAmp = 20.0 * IRE;
    const double lumaPed = 40.0 * IRE;

    chd::reader::Data lineData(vp.fieldWidth, static_cast<uint16_t>(BLACK));
    for (int32_t i = vp.colourBurstStart; i < vp.colourBurstEnd; i++) {
        // SMPTE 170M burst: sin(wt + 180 deg), with wt = i*pi/2 at 4fSC
        const double burst = -burstAmp * std::sin(M_PI_2 * i);
        lineData[i] = static_cast<uint16_t>(std::lround(BLACK + burst));
    }
    for (int32_t i = vp.activeVideoStart; i < vp.activeVideoEnd; i++) {
        lineData[i] = static_cast<uint16_t>(std::lround(BLACK + lumaPed + chroma(i)));
    }

    // Identical burst and chroma phase on every line: not how a conformant
    // signal alternates line-to-line, but the burst-locked demodulator
    // (splitIQlocked) detects each line independently, and the 1D comb is
    // horizontal-only, so nothing in the path under test sees the lines'
    // relative phases.
    chd::reader::Data fieldData;
    for (int32_t l = 0; l < vp.fieldHeight; l++) {
        fieldData.insert(fieldData.end(), lineData.begin(), lineData.end());
    }

    // Fields 0..1 are the 1D comb's unused look-behind slots; 2..3 are the
    // decoded frame.
    std::vector<chd::decoders::SourceField> fields(4);
    for (int32_t f = 2; f < 4; f++) {
        fields[f].data = fieldData;
        fields[f].field.isFirstField = (f % 2) == 0;
        fields[f].field.fieldPhaseID = 1;
    }
    return fields;
}

// betaPlateau > 0 attaches an active β profile (synthesized corrections).
chd::output::ComponentFrame
decodeWith(chd::decoders::comb::Comb::ChromaFilterMode mode,
           const chd::metadata::LdDecodeMetaData::VideoParameters &vp,
           const std::vector<chd::decoders::SourceField> &fields,
           double betaPlateau = 0.0, double betaCenterHz = 0.0, double betaWidthHz = 0.0)
{
    using namespace chd::decoders::comb;
    Comb::Configuration cfg;
    cfg.dimensions = 1;
    cfg.adaptive = false;
    cfg.phaseCompensation = true;
    cfg.chromaFilterMode = mode;
    cfg.ssbBetaPlateau = betaPlateau;
    cfg.ssbBetaEdgeCenterHz = betaCenterHz;
    cfg.ssbBetaEdgeWidthHz = betaWidthHz;

    Comb comb;
    comb.updateConfiguration(vp, cfg);
    std::vector<chd::output::ComponentFrame> frames(1);
    comb.decodeFrames(fields, 2, 4, frames);
    return std::move(frames[0]);
}

// Magnitude of the fm-frequency component of plane samples on `line`,
// measured by quadrature projection (insensitive to the demodulator's
// constant one-sample chroma delay).
double toneAmp(const chd::output::ComponentFrame &frame, bool iAxis,
               int32_t line, double fm,
               const chd::metadata::LdDecodeMetaData::VideoParameters &vp)
{
    // transformIQ leaves U/V in the u/v planes; undo its rotation (the
    // matrix is its own inverse) to get back to the I/Q axes.
    const double s = std::sin(RAD33), c = std::cos(RAD33);
    const double *U = frame.u(line);
    const double *V = frame.v(line);

    // Stay clear of the FIR edge regions (Hilbert half-length 98 plus the
    // 25-sample half-length of the cascaded prefilter EQ).
    const int32_t h0 = vp.activeVideoStart + 160;
    const int32_t h1 = vp.activeVideoEnd - 160;

    double a = 0.0, b = 0.0;
    for (int32_t h = h0; h < h1; h++) {
        const double x = iAxis ? (-s * U[h] + c * V[h]) : (c * U[h] + s * V[h]);
        const double ph = 2.0 * M_PI * fm * h / FS;
        a += x * std::cos(ph);
        b += x * std::sin(ph);
    }
    const double n = h1 - h0;
    return 2.0 * std::sqrt(a * a + b * b) / n;
}

int testSsbReconstruction()
{
    using Mode = chd::decoders::comb::Comb::ChromaFilterMode;
    const auto vp = makeVideoParameters();
    const double fm = 1.0e6;
    const double Am = 20.0 * IRE;

    // I-axis tone m(t), transmitted lower-sideband-only:
    // 1/2 * [m(t)cos(wt+33) + m^(t)sin(wt+33)], m^ = Hilbert(m)
    const auto fields = makeFields(vp, [&](int32_t i) {
        const double t = i / FS;
        const double m = Am * std::cos(2.0 * M_PI * fm * t);
        const double mh = Am * std::sin(2.0 * M_PI * fm * t);
        const double wt = M_PI_2 * i;
        return 0.5 * (m * std::cos(wt + RAD33) + mh * std::sin(wt + RAD33));
    });

    const auto ssb = decodeWith(Mode::WidebandISSB, vp, fields);
    const auto wbi = decodeWith(Mode::WidebandI, vp, fields);
    const int32_t line = 90;

    const double ampSsb = toneAmp(ssb, true, line, fm, vp);
    const double ampWbi = toneAmp(wbi, true, line, fm, vp);

    // Full-amplitude reconstruction: the SSB cross-feed restores the halved
    // detail and the prefilter EQ removes the band-edge droop, so the tone
    // comes back at ~its encoded amplitude (a wrong Hilbert sign would
    // cancel the direct path and land near zero instead).
    REQUIRE(std::abs(ampSsb - Am) < 0.12 * Am);
    // ~2.6x the plain asymmetric-low-pass result (2x SSB restoration plus
    // the prefilter droop the plain mode doesn't equalize).
    REQUIRE(ampSsb / ampWbi > 2.3 && ampSsb / ampWbi < 2.95);
    // The crosstalk is removed from Q by its 0.6 MHz low-pass.
    REQUIRE(toneAmp(ssb, false, line, fm, vp) < 0.15 * Am);
    return 0;
}

int testDsbToneUndisturbed()
{
    using Mode = chd::decoders::comb::Comb::ChromaFilterMode;
    const auto vp = makeVideoParameters();
    const double fm = 0.3e6;
    const double Am = 20.0 * IRE;

    // Ordinary double-sideband I-axis tone (equiband-style chroma).
    const auto fields = makeFields(vp, [&](int32_t i) {
        const double t = i / FS;
        const double m = Am * std::cos(2.0 * M_PI * fm * t);
        return m * std::cos(M_PI_2 * i + RAD33);
    });

    const auto ssb = decodeWith(Mode::WidebandISSB, vp, fields);
    const auto wbi = decodeWith(Mode::WidebandI, vp, fields);
    const int32_t line = 90;

    // Below the Hilbert band the SSB mode must match plain wideband_i (the
    // prefilter EQ is ~2% there, well inside this tolerance).
    const double ampSsb = toneAmp(ssb, true, line, fm, vp);
    const double ampWbi = toneAmp(wbi, true, line, fm, vp);
    REQUIRE(std::abs(ampSsb / ampWbi - 1.0) < 0.05);
    REQUIRE(ampSsb > 0.9 * Am);
    return 0;
}

int testRealQNotLeaked()
{
    using Mode = chd::decoders::comb::Comb::ChromaFilterMode;
    const auto vp = makeVideoParameters();
    const double fm = 0.3e6;
    const double Aq = 20.0 * IRE;

    // Real Q-axis content (double-sideband, inside Q's 0.6 MHz band).
    const auto fields = makeFields(vp, [&](int32_t i) {
        const double t = i / FS;
        const double q = Aq * std::cos(2.0 * M_PI * fm * t);
        return q * std::sin(M_PI_2 * i + RAD33);
    });

    const auto ssb = decodeWith(Mode::WidebandISSB, vp, fields);
    const int32_t line = 90;

    const double ampQ = toneAmp(ssb, false, line, fm, vp);
    REQUIRE(ampQ > 0.9 * Aq);
    // The Hilbert band-pass stops >= 38 dB through Q's whole allocation, so
    // real Q must not paint onto the I axis.
    REQUIRE(toneAmp(ssb, true, line, fm, vp) < 0.03 * ampQ);
    return 0;
}

// Fitted profile matching the test vestige (upper-sideband gain a(f) a
// raised cosine 1 → 0 across 0.40–0.70 MHz): β = (1−a)/(1+a) fitted by the
// calibration estimator as plateau 1.0, edge 0.578 ± 0.294/2 MHz.
constexpr double BETA_PLATEAU = 1.0;
constexpr double BETA_CENTER = 0.578e6;
constexpr double BETA_WIDTH = 0.294e6;

int testBetaStripCorrection()
{
    using Mode = chd::decoders::comb::Comb::ChromaFilterMode;
    const auto vp = makeVideoParameters();
    const double fm = 0.55e6;
    const double Am = 20.0 * IRE;
    const double a = 0.5;  // vestige: half the upper sideband survives

    // I tone in the transition strip, transmitted with asymmetric
    // sidebands: (a+b)/2·m·cos(wt+33°) + (b−a)/2·m̂·sin(wt+33°), b = 1.
    const auto fields = makeFields(vp, [&](int32_t i) {
        const double t = i / FS;
        const double m = Am * std::cos(2.0 * M_PI * fm * t);
        const double mh = Am * std::sin(2.0 * M_PI * fm * t);
        const double wt = M_PI_2 * i;
        return 0.5 * (a + 1.0) * m * std::cos(wt + RAD33)
             + 0.5 * (1.0 - a) * mh * std::sin(wt + RAD33);
    });

    const auto plain = decodeWith(Mode::WidebandISSB, vp, fields);
    const auto corrected = decodeWith(Mode::WidebandISSB, vp, fields,
                                      BETA_PLATEAU, BETA_CENTER, BETA_WIDTH);
    const int32_t line = 90;

    // Without a profile the strip recovers (a+b)/2 = 3/4 amplitude and the
    // crosstalk stays in Q; with the profile, I returns ~full and Q nulls.
    const double ampPlain = toneAmp(plain, true, line, fm, vp);
    const double ampCorrected = toneAmp(corrected, true, line, fm, vp);
    REQUIRE(ampPlain > 0.66 * Am && ampPlain < 0.82 * Am);
    REQUIRE(std::abs(ampCorrected - Am) < 0.08 * Am);

    const double ghostPlain = toneAmp(plain, false, line, fm, vp);
    const double ghostCorrected = toneAmp(corrected, false, line, fm, vp);
    std::cerr << "strip: ampPlain=" << ampPlain / Am << " ampCorr=" << ampCorrected / Am
              << " ghostPlain=" << ghostPlain / Am << " ghostCorr=" << ghostCorrected / Am << "\n";
    REQUIRE(ghostPlain > 0.10 * Am);
    REQUIRE(ghostCorrected < 0.05 * Am);
    return 0;
}

int testBetaLeavesOtherBandsAlone()
{
    using Mode = chd::decoders::comb::Comb::ChromaFilterMode;
    const auto vp = makeVideoParameters();
    const double Am = 20.0 * IRE;
    const int32_t line = 90;

    // DSB I tone below the ramp: an active profile must not touch it.
    {
        const double fm = 0.3e6;
        const auto fields = makeFields(vp, [&](int32_t i) {
            const double t = i / FS;
            return Am * std::cos(2.0 * M_PI * fm * t) * std::cos(M_PI_2 * i + RAD33);
        });
        const auto plain = decodeWith(Mode::WidebandISSB, vp, fields);
        const auto corrected = decodeWith(Mode::WidebandISSB, vp, fields,
                                          BETA_PLATEAU, BETA_CENTER, BETA_WIDTH);
        const double r = toneAmp(corrected, true, line, fm, vp)
                       / toneAmp(plain, true, line, fm, vp);
        REQUIRE(std::abs(r - 1.0) < 0.02);
    }

    // LSB tone above the skirt: the cross-feed is already exact there, and
    // the profile's I equalizer blends to unity where the skirt is full on
    // (it also flattens the skirt's ripple, so compare against the encoded
    // amplitude rather than the plain decode).
    {
        const double fm = 1.0e6;
        const auto fields = makeFields(vp, [&](int32_t i) {
            const double t = i / FS;
            const double m = Am * std::cos(2.0 * M_PI * fm * t);
            const double mh = Am * std::sin(2.0 * M_PI * fm * t);
            const double wt = M_PI_2 * i;
            return 0.5 * (m * std::cos(wt + RAD33) + mh * std::sin(wt + RAD33));
        });
        const auto corrected = decodeWith(Mode::WidebandISSB, vp, fields,
                                          BETA_PLATEAU, BETA_CENTER, BETA_WIDTH);
        REQUIRE(std::abs(toneAmp(corrected, true, line, fm, vp) - Am) < 0.06 * Am);
    }
    return 0;
}

int testRealQProtectedStrip()
{
    using Mode = chd::decoders::comb::Comb::ChromaFilterMode;
    const auto vp = makeVideoParameters();
    const double fm = 0.55e6;
    const double Aq = 20.0 * IRE;

    // Real Q near the top of its spec allocation (<= 0.6 MHz) — above where
    // the original Hilbert skirt had already faded in. The steep skirt must
    // keep it off the I axis; its own decode is attenuated only by Q's
    // 0.6 MHz low-pass.
    const auto fields = makeFields(vp, [&](int32_t i) {
        const double t = i / FS;
        const double q = Aq * std::cos(2.0 * M_PI * fm * t);
        return q * std::sin(M_PI_2 * i + RAD33);
    });

    const auto ssb = decodeWith(Mode::WidebandISSB, vp, fields);
    const int32_t line = 90;

    REQUIRE(toneAmp(ssb, false, line, fm, vp) > 0.5 * Aq);
    REQUIRE(toneAmp(ssb, true, line, fm, vp) < 0.03 * Aq);
    return 0;
}

// color_under is a symmetric ~0.5 MHz low-pass for VHS/S-VHS colour-under
// chroma. It must pass in-band chroma like equiband but reject everything above
// its cutoff far harder (the shared prefilter droop cancels in the ratios).
int testColorUnderBandwidth()
{
    using Mode = chd::decoders::comb::Comb::ChromaFilterMode;
    const auto vp = makeVideoParameters();
    const double Am = 20.0 * IRE;
    const int32_t line = 90;

    // Double-sideband I-axis tone at frequency fm.
    const auto toneFields = [&](double fm) {
        return makeFields(vp, [&](int32_t i) {
            const double m = Am * std::cos(2.0 * M_PI * fm * (i / FS));
            return m * std::cos(M_PI_2 * i + RAD33);
        });
    };

    // In band (0.3 MHz): color_under passes it, ~identically to equiband.
    {
        const auto f = toneFields(0.3e6);
        const double cu = toneAmp(decodeWith(Mode::ColorUnder, vp, f), true, line, 0.3e6, vp);
        const double eq = toneAmp(decodeWith(Mode::Equiband13, vp, f), true, line, 0.3e6, vp);
        REQUIRE(cu > 0.7 * Am);
        REQUIRE(std::abs(cu / eq - 1.0) < 0.10);
    }
    // Out of band (1.0 MHz): color_under's ~0.5 MHz cutoff rejects it an order
    // of magnitude harder than equiband's 1.3 MHz passband.
    {
        const auto f = toneFields(1.0e6);
        const double cu = toneAmp(decodeWith(Mode::ColorUnder, vp, f), true, line, 1.0e6, vp);
        const double eq = toneAmp(decodeWith(Mode::Equiband13, vp, f), true, line, 1.0e6, vp);
        REQUIRE(cu < 0.1 * eq);
    }
    return 0;
}

}  // namespace

int main()
{
    int failures = 0;
    failures += testSsbReconstruction();
    failures += testDsbToneUndisturbed();
    failures += testRealQNotLeaked();
    failures += testBetaStripCorrection();
    failures += testBetaLeavesOtherBandsAlone();
    failures += testRealQProtectedStrip();
    failures += testColorUnderBandwidth();
    if (failures == 0) std::cout << "PASS test_ntsc_chroma_ssb\n";
    return failures == 0 ? 0 : 1;
}