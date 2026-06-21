// SPDX-License-Identifier: GPL-3.0-or-later

#include "beta_calibration.h"

#include <algorithm>
#include <cmath>

#include "../../output/component_frame.h"
#include "../filter/deemp.h"

namespace chd::decoders::comb {

namespace {

// In-place iterative radix-2 complex FFT (forward, e^{-j2πkn/N}). The
// transform is one 1024-point pass per line; a dependency-free kernel is
// simpler here than sharing the NN modules' FFTW plan management.
void fft(std::vector<std::complex<double>> &x)
{
    const size_t n = x.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(x[i], x[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * M_PI / static_cast<double>(len);
        const std::complex<double> wlen(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (size_t k = 0; k < len / 2; k++) {
                const std::complex<double> u = x[i + k];
                const std::complex<double> v = x[i + k + len / 2] * w;
                x[i + k] = u + v;
                x[i + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

}  // namespace

void BetaAccumulator::accumulateFrame(const chd::output::ComponentFrame &frame,
                                      const chd::metadata::LdDecodeMetaData::VideoParameters &vp)
{
    const int32_t width = std::min(vp.activeVideoEnd - vp.activeVideoStart, kFftSize);
    if (width <= 0) return;

    sampleRate = vp.sampleRate;
    if (window.size() != static_cast<size_t>(width)) {
        window.resize(width);
        for (int32_t n = 0; n < width; n++) {
            window[n] = 0.5 - 0.5 * std::cos(2.0 * M_PI * n / (width - 1));
        }
    }
    fftBuf.resize(kFftSize);

    for (int32_t lineNumber = vp.firstActiveFrameLine; lineNumber <= vp.lastActiveFrameLine; lineNumber++) {
        const double *I = frame.u(lineNumber) + vp.activeVideoStart;
        const double *Q = frame.v(lineNumber) + vp.activeVideoStart;

        // Pack both real planes into one complex transform: x = I + jQ.
        for (int32_t n = 0; n < width; n++) {
            fftBuf[n] = { I[n] * window[n], Q[n] * window[n] };
        }
        std::fill(fftBuf.begin() + width, fftBuf.end(), std::complex<double>(0.0, 0.0));
        fft(fftBuf);

        // Unpack the two conjugate-symmetric spectra and accumulate.
        for (int32_t k = 1; k < kFftSize / 2; k++) {
            const std::complex<double> xk = fftBuf[k];
            const std::complex<double> xnk = std::conj(fftBuf[kFftSize - k]);
            const std::complex<double> Ik = 0.5 * (xk + xnk);
            const std::complex<double> Qk = std::complex<double>(0.0, -0.5) * (xk - xnk);

            const std::complex<double> c = Ik * std::conj(Qk);
            numIm[k] += c.imag();
            crossRe[k] += c.real();
            den[k] += std::norm(Ik);
            qq[k] += std::norm(Qk);
        }
        lines++;
    }
}

double BetaAccumulator::betaModel(double fHz, double centerHz, double widthHz, double plateau)
{
    const double u = (fHz - (centerHz - widthHz / 2.0)) / widthHz;
    if (u <= 0.0) return 0.0;
    if (u >= 1.0) return plateau;
    return plateau * (0.5 - 0.5 * std::cos(M_PI * u));
}

std::vector<BetaAccumulator::BinSample> BetaAccumulator::bins() const
{
    std::vector<BinSample> out;
    if (lines == 0 || sampleRate <= 0.0) return out;
    const double binHz = sampleRate / kFftSize;
    out.reserve(kFftSize / 2 - 1);
    for (int32_t k = 1; k < kFftSize / 2; k++) {
        BinSample s;
        s.fHz = k * binHz;
        s.weight = den[k];
        if (den[k] > 0.0) s.betaHat = numIm[k] / den[k];
        const double power = den[k] * qq[k];
        if (power > 0.0) {
            s.coherence = (numIm[k] * numIm[k] + crossRe[k] * crossRe[k]) / power;
        }
        out.push_back(s);
    }
    return out;
}

BetaAccumulator::FitResult BetaAccumulator::fit() const
{
    FitResult result;
    result.linesAccumulated = lines;
    if (lines == 0 || sampleRate <= 0.0) return result;

    const double binHz = sampleRate / kFftSize;
    const auto binOf = [&](double fHz) {
        return std::clamp(static_cast<int32_t>(std::lround(fHz / binHz)), 1, kFftSize / 2 - 1);
    };

    // β̂ bins over the fit range, weighted by the accumulated I energy.
    const int32_t kLo = binOf(0.25e6), kHi = binOf(1.10e6);
    std::vector<double> betaHat(kHi + 1, 0.0);
    double totalWeight = 0.0;
    for (int32_t k = kLo; k <= kHi; k++) {
        if (den[k] > 0.0) betaHat[k] = numIm[k] / den[k];
        totalWeight += den[k];
    }
    if (totalWeight <= 0.0) return result;

    // Weighted least squares against the raised-cosine edge model. For a
    // fixed (center, width) the optimal plateau is closed-form, so a grid
    // search plus one refinement pass over the two shape parameters is
    // sufficient (the surface is small and smooth).
    double bestSse = -1.0, bestCenter = 0.0, bestWidth = 0.0, bestPlateau = 0.0;
    const auto evaluate = [&](double center, double width) {
        double rNum = 0.0, rDen = 0.0;
        for (int32_t k = kLo; k <= kHi; k++) {
            const double r = betaModel(k * binHz, center, width, 1.0);
            rNum += den[k] * betaHat[k] * r;
            rDen += den[k] * r * r;
        }
        const double p = (rDen > 0.0) ? std::clamp(rNum / rDen, 0.0, 1.0) : 0.0;
        double sse = 0.0;
        for (int32_t k = kLo; k <= kHi; k++) {
            const double e = betaHat[k] - betaModel(k * binHz, center, width, p);
            sse += den[k] * e * e;
        }
        if (bestSse < 0.0 || sse < bestSse) {
            bestSse = sse;
            bestCenter = center;
            bestWidth = width;
            bestPlateau = p;
        }
    };
    for (double center = 0.30e6; center <= 0.90e6; center += 0.01e6) {
        for (double width = 0.05e6; width <= 0.50e6; width += 0.01e6) {
            evaluate(center, width);
        }
    }
    const double coarseCenter = bestCenter, coarseWidth = bestWidth;
    for (double center = coarseCenter - 0.01e6; center <= coarseCenter + 0.01e6; center += 0.002e6) {
        for (double width = std::max(0.03e6, coarseWidth - 0.01e6); width <= coarseWidth + 0.01e6; width += 0.002e6) {
            evaluate(center, width);
        }
    }

    result.plateau = bestPlateau;
    result.edgeCenterHz = bestCenter;
    result.edgeWidthHz = bestWidth;
    result.fitRms = std::sqrt(bestSse / totalWeight);

    // Magnitude-squared coherence over the self-test band, weighted by the
    // I energy so empty bins don't dilute it.
    const int32_t cLo = binOf(0.70e6), cHi = binOf(1.00e6);
    double cohSum = 0.0, cohWeight = 0.0;
    for (int32_t k = cLo; k <= cHi; k++) {
        const double cross = numIm[k] * numIm[k] + crossRe[k] * crossRe[k];
        const double power = den[k] * qq[k];
        if (power > 0.0) {
            cohSum += den[k] * (cross / power);
            cohWeight += den[k];
        }
    }
    result.coherence = (cohWeight > 0.0) ? (cohSum / cohWeight) : 0.0;

    result.isWidebandI = result.coherence >= kMinCoherence && result.plateau >= kMinPlateau;
    return result;
}

namespace {

// Frequency-sampling FIR design: sample a zero-phase desired response on a
// dense grid, add the centering delay, inverse-FFT, truncate and window.
// `desired(f)` is real for a symmetric design and pure-imaginary (−j·B) for
// an antisymmetric (Hilbert-characteristic) one; the impulse response comes
// out with the matching symmetry automatically.
template <typename Desired>
std::vector<double> designByFrequencySampling(int32_t numTaps, double sampleRate,
                                              const Desired &desired)
{
    constexpr int32_t kGrid = 2048;
    const int32_t center = (numTaps - 1) / 2;

    std::vector<std::complex<double>> spectrum(kGrid);
    for (int32_t k = 0; k <= kGrid / 2; k++) {
        const double phase = -2.0 * M_PI * k * center / kGrid;
        std::complex<double> d = desired(k * sampleRate / kGrid);
        if (k == kGrid / 2) d = { d.real(), 0.0 };  // Nyquist bin must be real
        spectrum[k] = d * std::complex<double>(std::cos(phase), std::sin(phase));
        if (k != 0 && k != kGrid / 2) spectrum[kGrid - k] = std::conj(spectrum[k]);
    }

    // Inverse FFT via conjugation.
    for (auto &v : spectrum) v = std::conj(v);
    fft(spectrum);

    std::vector<double> taps(numTaps);
    for (int32_t n = 0; n < numTaps; n++) {
        const double hann = 0.5 - 0.5 * std::cos(2.0 * M_PI * n / (numTaps - 1));
        taps[n] = (spectrum[n].real() / kGrid) * hann;
    }
    return taps;
}

}  // namespace

SsbCorrectionTaps synthesizeSsbCorrections(double plateau, double edgeCenterHz,
                                           double edgeWidthHz, double sampleRate)
{
    constexpr int32_t kGrid = 2048;
    constexpr int32_t kTaps = 257;

    // As-built Hilbert skirt magnitude W(f) on the design grid, by
    // transforming the c_colorhilb_b taps.
    std::vector<std::complex<double>> h(kGrid, { 0.0, 0.0 });
    for (size_t n = 0; n < chd::decoders::filter::c_colorhilb_b.size(); n++) {
        h[n] = { chd::decoders::filter::c_colorhilb_b[n], 0.0 };
    }
    fft(h);
    const auto skirt = [&](double fHz) {
        const double k = fHz * kGrid / sampleRate;
        const int32_t k0 = std::clamp(static_cast<int32_t>(k), 0, kGrid / 2 - 1);
        const double frac = k - k0;
        return (1.0 - frac) * std::abs(h[k0]) + frac * std::abs(h[k0 + 1]);
    };

    // Confine both corrections to the chroma band: β would otherwise hold
    // its plateau out to where the skirt's stopband resumes (>1.55 MHz) and
    // boost out-of-band noise.
    const auto taper = [](double fHz) {
        const double u = (fHz - 1.4e6) / 0.3e6;
        if (u <= 0.0) return 1.0;
        if (u >= 1.0) return 0.0;
        return 0.5 + 0.5 * std::cos(M_PI * u);
    };
    const auto beta = [&](double fHz) {
        return BetaAccumulator::betaModel(fHz, edgeCenterHz, edgeWidthHz, plateau) * taper(fHz);
    };

    SsbCorrectionTaps out;
    out.iEq = designByFrequencySampling(kTaps, sampleRate, [&](double fHz) {
        return std::complex<double>(1.0 + beta(fHz) * (1.0 - std::min(skirt(fHz), 1.0)), 0.0);
    });
    // +jβ, not −jβ: firfilter.h's apply() is a correlation, which negates an
    // antisymmetric kernel's designed response. Specifying +jβ makes the
    // APPLIED operator the standard Hilbert scaled by β — the same
    // orientation apply() gives the c_colorhilb_b cross-feed — so the
    // subtraction nulls the crosstalk (the opposite sign doubles it).
    out.qNull = designByFrequencySampling(kTaps, sampleRate, [&](double fHz) {
        return std::complex<double>(0.0, beta(fHz));
    });
    return out;
}

}  // namespace chd::decoders::comb
