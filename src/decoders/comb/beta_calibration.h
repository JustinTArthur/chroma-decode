// SPDX-License-Identifier: GPL-3.0-or-later
//
// NTSC-1953 sideband-asymmetry calibration. Accumulates the quadrature
// cross-spectrum between the burst-locked demodulated I and Q planes
// (tapped ahead of filterIQ, so the >0.6 MHz crosstalk is still present)
// and fits the per-frequency asymmetry profile β(f) = (b−a)/(a+b), where
// a/b are the upper/lower chroma sideband gains of the source chain.
//
// Estimator: for a lower-sideband component, Q̃(f) = −jβ·Ĩ(f), so
//   β̂(f) = Im⟨Ĩ(f)·conj(Q̃(f))⟩ / ⟨|Ĩ(f)|²⟩
// accumulated Welch-style (ratio of sums) over every active line. The
// imaginary-part projection is load-bearing: scene-driven I/Q correlation
// (co-located colored edges) and burst-phase rotation errors land in the
// REAL part of the cross-spectrum; only a 90° (Hilbert) relationship lands
// in the imaginary part.

#ifndef CHD_DECODERS_COMB_BETA_CALIBRATION_H
#define CHD_DECODERS_COMB_BETA_CALIBRATION_H

#include <array>
#include <complex>
#include <cstdint>
#include <vector>

#include "../../metadata/core.h"

namespace chd::output { class ComponentFrame; }

namespace chd::decoders::comb {

class BetaAccumulator
{
public:
    struct FitResult {
        // Raised-cosine edge model: β(f) ramps 0 → plateau across
        // [edgeCenterHz − edgeWidthHz/2, edgeCenterHz + edgeWidthHz/2].
        double plateau = 0.0;
        double edgeCenterHz = 0.0;
        double edgeWidthHz = 0.0;
        // Mean I/Q magnitude-squared coherence over 0.70–1.00 MHz, where the
        // model predicts Q̃ = −jβĨ exactly (real Q is zero by construction).
        double coherence = 0.0;
        // Weighted RMS residual of β̂ bins against the fitted model.
        double fitRms = 0.0;
        int64_t linesAccumulated = 0;
        // Source classification: coherent crosstalk with a substantial
        // plateau ⇒ the capture carries lower-sideband wideband I.
        bool isWidebandI = false;
    };

    // Accumulate every active line of a frame whose u/v planes hold the
    // burst-locked demodulated I/Q (after splitIQlocked, before filterIQ).
    void accumulateFrame(const chd::output::ComponentFrame &frame,
                         const chd::metadata::LdDecodeMetaData::VideoParameters &vp);

    FitResult fit() const;

    // Raw per-bin estimates (diagnostics / reporting).
    struct BinSample {
        double fHz = 0.0;
        double betaHat = 0.0;    // Im-projected LS estimate at this bin
        double weight = 0.0;     // accumulated |Ĩ|²
        double coherence = 0.0;  // magnitude-squared I/Q coherence
    };
    std::vector<BinSample> bins() const;

    // Evaluate the fitted model at a frequency (Hz).
    static double betaModel(double fHz, double centerHz, double widthHz, double plateau);

    // β-classification gate shared by fit() and the correction consumers:
    // a profile only drives corrections when the source classified as
    // wideband-I AND the plateau is substantial.
    static constexpr double kMinCoherence = 0.5;
    static constexpr double kMinPlateau = 0.25;

    // Lines are Hann-windowed and zero-padded to this transform length.
    static constexpr int32_t kFftSize = 1024;

private:
    std::array<double, kFftSize / 2> numIm{};    // Σ Im(Ĩ·conj(Q̃))
    std::array<double, kFftSize / 2> crossRe{};  // Σ Re(Ĩ·conj(Q̃))
    std::array<double, kFftSize / 2> den{};      // Σ |Ĩ|²
    std::array<double, kFftSize / 2> qq{};       // Σ |Q̃|²
    int64_t lines = 0;
    double sampleRate = 0.0;

    std::vector<double> window;
    std::vector<std::complex<double>> fftBuf;
};

// Correction FIRs synthesized from a fitted β profile, consumed by the
// wideband_i_ssb reconstruction (filterIQ):
//   iEq    symmetric; direct-path gain 1 + β(f)·(1 − W(f)), where W is the
//          as-built Hilbert skirt — restores b·m through the vestigial
//          transition strip and blends exactly into the skirt's
//          vestige-blind region (gain → 1 where W → 1, and ≡ 1 at β ≡ 0)
//   qNull  antisymmetric (Hilbert characteristic) with magnitude β(f);
//          subtracted from the Q plane to null the I-detail crosstalk
struct SsbCorrectionTaps {
    std::vector<double> iEq;
    std::vector<double> qNull;
};

SsbCorrectionTaps synthesizeSsbCorrections(double plateau, double edgeCenterHz,
                                           double edgeWidthHz, double sampleRate);

}  // namespace chd::decoders::comb

#endif  // CHD_DECODERS_COMB_BETA_CALIBRATION_H
