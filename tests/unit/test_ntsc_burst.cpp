// SPDX-License-Identifier: GPL-3.0-or-later
//
// NTSC burst measurement and phase compensation (decoders/ntsc_burst).
// Synthesizes lines whose subcarrier sits at a known rotation away from the
// nominal field/line phase, and checks that:
//
//   1. detectBurstDeviation recovers that rotation, for both nominal carrier
//      signs, and reports invalid when the burst is too weak to measure.
//   2. burstLockedCarrier rebuilds the carrier pair that actually modulated
//      the line — the input-side correction the ldzeug2 color_cnn decoder
//      feeds the network.
//   3. correctDemodulatedIQ recovers the transmitted chroma vector from a nominal-axis
//      demodulation — the output-side correction the luma_sep decoder
//      applies after its quadrature switch.
//   4. Both corrections collapse to the uncompensated behaviour when the
//      signal is already at nominal phase, so enabling phase compensation on
//      conformant material changes nothing.

#include "../../src/decoders/ntsc_burst.h"
#include "../../src/metadata/core.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

#define REQUIRE(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #cond "\n"; \
        return 1; \
    } \
} while (0)

#define REQUIRE_NEAR(a, b, tol) do { \
    const double va = (a), vb = (b); \
    if (std::fabs(va - vb) > (tol)) { \
        std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " \
                  << #a " (" << va << ") != " #b " (" << vb << ") within " << (tol) << "\n"; \
        return 1; \
    } \
} while (0)

constexpr int32_t BLACK = 16384;
constexpr int32_t WHITE = 54016;
constexpr double  IRE   = (WHITE - BLACK) / 100.0;

constexpr double SIN33 = 0.5446390350150271;
constexpr double COS33 = 0.838670567945424;

chd::metadata::LdDecodeMetaData::VideoParameters makeVideoParameters()
{
    chd::metadata::LdDecodeMetaData::VideoParameters vp;
    vp.system = chd::metadata::NTSC;
    vp.fieldWidth = 910;
    vp.fieldHeight = 263;
    vp.sampleRate = 4.0 * 3.579545e6;
    vp.fSC = 3.579545e6;
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

// A line carrying burst plus a constant chroma vector, all modulated on
// carriers rotated `deltaDeg` away from the nominal phase for `sign`.
// burstAmpIre == 0 synthesizes a line with no burst at all.
std::vector<uint16_t> makeLine(const chd::metadata::LdDecodeMetaData::VideoParameters &vp,
                               double sign, double deltaDeg,
                               double chromaI, double chromaQ,
                               double burstAmpIre = 20.0)
{
    const double delta = deltaDeg * M_PI / 180.0;
    chd::decoders::BurstDeviation dev;
    dev.cosDelta = std::cos(delta);
    dev.sinDelta = std::sin(delta);
    dev.valid = true;

    // Burst is the -U axis at burstAmp; convert to the I/Q pair that the
    // carriers below modulate (the inverse of ldzeug2's uv_from_iq).
    const double burstAmp = burstAmpIre * IRE;
    const double burstI =  SIN33 * burstAmp;
    const double burstQ = -COS33 * burstAmp;

    std::vector<uint16_t> line(vp.fieldWidth, static_cast<uint16_t>(BLACK));
    for (int32_t x = 0; x < vp.fieldWidth; x++) {
        double ic, qc;
        chd::decoders::burstLockedCarrier(x, dev, sign, &ic, &qc);

        double v = BLACK;
        if (x >= vp.colourBurstStart && x < vp.colourBurstEnd) {
            v += burstI * ic + burstQ * qc;
        } else if (x >= vp.activeVideoStart && x < vp.activeVideoEnd) {
            v += 40.0 * IRE + chromaI * ic + chromaQ * qc;
        }
        line[x] = static_cast<uint16_t>(std::lround(v));
    }
    return line;
}

// Demodulate one sample against the *nominal* carriers only. With `sign` set
// to the line's modulation carrier sign this is exactly what the luma_sep
// quadrature switch computes: its linePhase negation cancels against the
// negations in its case table, leaving carrier-signed products.
void nominalDemod(const std::vector<uint16_t> &line, int32_t x, double sign,
                  double pedestal, double *i, double *q)
{
    const double c = (static_cast<double>(line[x]) - pedestal) * sign;
    // The switch assigns each sample to whichever axis its carrier is
    // non-zero on: I at odd x, Q at even x, with the carrier's sign.
    *i = c * chd::decoders::kNtscCarrierI[x % 4];
    *q = c * chd::decoders::kNtscCarrierQ[x % 4];
}

int testDeviationRecovery()
{
    const auto vp = makeVideoParameters();

    for (const double sign : {1.0, -1.0}) {
        for (const double deltaDeg : {0.0, 12.0, -25.0, 70.0, -120.0}) {
            const auto line = makeLine(vp, sign, deltaDeg, 30.0 * IRE, -18.0 * IRE);
            const auto dev = chd::decoders::detectBurstDeviation(line.data(), vp, sign);

            REQUIRE(dev.valid);
            const double delta = deltaDeg * M_PI / 180.0;
            REQUIRE_NEAR(dev.cosDelta, std::cos(delta), 2e-3);
            REQUIRE_NEAR(dev.sinDelta, std::sin(delta), 2e-3);
        }
    }
    return 0;
}

int testCarrierReconstruction()
{
    const auto vp = makeVideoParameters();
    const double sign = 1.0;
    const double deltaDeg = 18.0;
    const double delta = deltaDeg * M_PI / 180.0;

    const auto line = makeLine(vp, sign, deltaDeg, 30.0 * IRE, -18.0 * IRE);
    const auto dev = chd::decoders::detectBurstDeviation(line.data(), vp, sign);
    REQUIRE(dev.valid);

    // The rebuilt carriers must match the ones makeLine actually modulated
    // with, which is what the network is trained to expect.
    chd::decoders::BurstDeviation truth;
    truth.cosDelta = std::cos(delta);
    truth.sinDelta = std::sin(delta);
    truth.valid = true;

    for (int32_t x = 0; x < 16; x++) {
        double icTruth, qcTruth, icMeasured, qcMeasured;
        chd::decoders::burstLockedCarrier(x, truth, sign, &icTruth, &qcTruth);
        chd::decoders::burstLockedCarrier(x, dev,   sign, &icMeasured, &qcMeasured);
        REQUIRE_NEAR(icMeasured, icTruth, 5e-3);
        REQUIRE_NEAR(qcMeasured, qcTruth, 5e-3);
    }
    return 0;
}

int testIQCorrection()
{
    const auto vp = makeVideoParameters();
    const double pedestal = BLACK + 40.0 * IRE;
    const double chromaI = 30.0 * IRE;
    const double chromaQ = -18.0 * IRE;

    for (const double sign : {1.0, -1.0}) {
        const double deltaDeg = 22.0;
        const auto line = makeLine(vp, sign, deltaDeg, chromaI, chromaQ);
        const auto dev = chd::decoders::detectBurstDeviation(line.data(), vp, sign);
        REQUIRE(dev.valid);

        // Demodulating against nominal carriers mixes I into Q and back; the
        // deviation rotation must undo exactly that. Pair adjacent samples so
        // both axes are covered, as the switch's carried si/sq do.
        for (int32_t x = vp.activeVideoStart + 4; x < vp.activeVideoStart + 20; x += 2) {
            double i0, q0, i1, q1;
            nominalDemod(line, x,     sign, pedestal, &i0, &q0);
            nominalDemod(line, x + 1, sign, pedestal, &i1, &q1);
            // Whichever axis each sample lands on, take the non-zero one.
            double i = (std::fabs(i0) > std::fabs(i1)) ? i0 : i1;
            double q = (std::fabs(q0) > std::fabs(q1)) ? q0 : q1;

            chd::decoders::correctDemodulatedIQ(dev, &i, &q);
            REQUIRE_NEAR(i, chromaI, 0.02 * std::fabs(chromaI) + 1.0);
            REQUIRE_NEAR(q, chromaQ, 0.02 * std::fabs(chromaQ) + 1.0);
        }
    }
    return 0;
}

// The luma_sep decoder measures the deviation with the carrier sign of the
// line (+1 on positive-phase lines), not with the sign its quadrature switch
// applies to the composite, which is the opposite. The distinction matters
// because the nominal burst phasor is odd in the carrier sign: measuring
// against the wrong one reports a spurious extra 180 degrees and the
// correction then negates every chroma vector on the line. This mirrors the
// decoder's switch verbatim, feeds the deviation the carrier sign as the
// decoder does, and requires the corrected output to match the transmitted
// chroma on both line parities, deviated and conformant alike.
int testSwitchCarrierSignConvention()
{
    const auto vp = makeVideoParameters();
    const double pedestal = BLACK + 40.0 * IRE;
    const double chromaI = 30.0 * IRE;
    const double chromaQ = -18.0 * IRE;

    for (const bool linePhase : {true, false}) {
        const double carrierSign = linePhase ? 1.0 : -1.0;
        for (const double deltaDeg : {0.0, 22.0, -40.0}) {
            const auto line = makeLine(vp, carrierSign, deltaDeg, chromaI, chromaQ);
            const auto dev = chd::decoders::detectBurstDeviation(line.data(), vp, carrierSign);
            REQUIRE(dev.valid);
            if (deltaDeg == 0.0) {
                // Conformant line: compensation must be a no-op.
                REQUIRE_NEAR(dev.cosDelta, 1.0, 2e-3);
                REQUIRE_NEAR(dev.sinDelta, 0.0, 2e-3);
            }

            // demodChromaRow's quadrature switch, verbatim.
            double si = 0.0, sq = 0.0;
            for (int32_t h = vp.activeVideoStart; h < vp.activeVideoStart + 20; h++) {
                const double C    = static_cast<double>(line[h]) - pedestal;
                const double cavg = linePhase ? -C : C;
                switch (h % 4) {
                    case 0: sq =  cavg; break;
                    case 1: si = -cavg; break;
                    case 2: sq = -cavg; break;
                    case 3: si =  cavg; break;
                    default: break;
                }
                if (h < vp.activeVideoStart + 2) continue;  // both axes seeded

                double i = si, q = sq;
                chd::decoders::correctDemodulatedIQ(dev, &i, &q);
                REQUIRE_NEAR(i, chromaI, 0.02 * std::fabs(chromaI) + 1.0);
                REQUIRE_NEAR(q, chromaQ, 0.02 * std::fabs(chromaQ) + 1.0);
            }
        }
    }
    return 0;
}

// A conformant line must come back with an identity rotation, so turning
// phase compensation on cannot disturb already-correct material.
int testNominalIsIdentity()
{
    const auto vp = makeVideoParameters();

    for (const double sign : {1.0, -1.0}) {
        const auto line = makeLine(vp, sign, 0.0, 30.0 * IRE, -18.0 * IRE);
        const auto dev = chd::decoders::detectBurstDeviation(line.data(), vp, sign);
        REQUIRE(dev.valid);
        REQUIRE_NEAR(dev.cosDelta, 1.0, 2e-3);
        REQUIRE_NEAR(dev.sinDelta, 0.0, 2e-3);

        for (int32_t x = 0; x < 16; x++) {
            double ic, qc;
            chd::decoders::burstLockedCarrier(x, dev, sign, &ic, &qc);
            REQUIRE_NEAR(ic, chd::decoders::kNtscCarrierI[x % 4] * sign, 5e-3);
            REQUIRE_NEAR(qc, chd::decoders::kNtscCarrierQ[x % 4] * sign, 5e-3);
        }
    }
    return 0;
}

// Too little burst to measure: the deviation is invalid and its identity
// default leaves callers on the nominal carriers.
int testWeakBurstFallsBack()
{
    const auto vp = makeVideoParameters();
    const auto line = makeLine(vp, 1.0, 30.0, 30.0 * IRE, -18.0 * IRE, /*burstAmpIre=*/0.0);
    const auto dev = chd::decoders::detectBurstDeviation(line.data(), vp, 1.0);

    REQUIRE(!dev.valid);
    REQUIRE_NEAR(dev.cosDelta, 1.0, 1e-12);
    REQUIRE_NEAR(dev.sinDelta, 0.0, 1e-12);

    for (int32_t x = 0; x < 8; x++) {
        double ic, qc;
        chd::decoders::burstLockedCarrier(x, dev, 1.0, &ic, &qc);
        REQUIRE_NEAR(ic, chd::decoders::kNtscCarrierI[x % 4], 1e-12);
        REQUIRE_NEAR(qc, chd::decoders::kNtscCarrierQ[x % 4], 1e-12);
    }
    return 0;
}

}  // namespace

int main()
{
    if (testDeviationRecovery()   != 0) return 1;
    if (testCarrierReconstruction() != 0) return 1;
    if (testIQCorrection()        != 0) return 1;
    if (testSwitchCarrierSignConvention() != 0) return 1;
    if (testNominalIsIdentity()   != 0) return 1;
    if (testWeakBurstFallsBack()  != 0) return 1;

    std::cout << "PASS test_ntsc_burst\n";
    return 0;
}
