// SPDX-License-Identifier: GPL-3.0-or-later
//
// SECAM line-sequential FM chroma decoder.
//
// Decodes the SECAM FM chroma block (BT.1700 Part C / BT.470-6) from either
// a separated chroma plane (vhs-decode Y/C TBC pair) or a composite signal:
// block-FFT analytic signal with the closed-form inverse of the HF
// pre-correction "bell" applied in the frequency domain, a designed
// differentiating-FIR discriminator, per-field carrier calibration and
// Db/Dr line identification from the back-porch reference carriers, exact
// inverse of the LF pre-correction, and colour-difference scaling to the
// ComponentFrame's composite-domain U/V. Output is inherently 4:4:0: each
// line yields one colour-difference component, recorded per row in the
// ComponentFrame's chromaRowComponents map.

#ifndef CHD_DECODERS_SECAM_SECAM_DECODER_H
#define CHD_DECODERS_SECAM_SECAM_DECODER_H

#include <complex>
#include <cstdint>
#include <vector>

#include <fftw3.h>

#include "../decoder_base.h"

namespace chd::decoders::secam {

class SecamDecoder : public Decoder {
public:
    // How per-line Db/Dr identity is resolved. Auto prefers the back-porch
    // measurement, then field-ident bottles, then content statistics.
    // Manual applies a fixed lattice anchored by manualFirstComponent.
    enum class IdentMode { Auto = 0, Porch, Bottles, Manual };

    struct SecamConfiguration : public Decoder::Configuration {
        double chromaGain = 1.0;
        IdentMode identMode = IdentMode::Auto;
        // Manual-mode anchor: component of the first active line of the
        // first field of frame 0 (0 = Db, 1 = Dr); the deterministic
        // BR.469 alternation extends it across lines, fields, and frames.
        int32_t manualFirstComponent = 0;
        // FM click concealment: 0.0 bypasses the stage entirely. The
        // expert overrides (when > 0) replace the adaptive thresholds.
        double clickNrLevel = 1.0;
        double clickEnvDipDbOverride = 0.0;
        double clickFreqOvershootOverride = 0.0;
    };

    explicit SecamDecoder(const SecamConfiguration &secamConfig);
    ~SecamDecoder() override;

    SecamDecoder(const SecamDecoder &) = delete;
    SecamDecoder &operator=(const SecamDecoder &) = delete;

    bool configure(const chd::metadata::LdDecodeMetaData::VideoParameters &videoParameters) override;

    void decodeFrames(const std::vector<SourceField> &inputFields,
                      int32_t startIndex, int32_t endIndex,
                      std::vector<chd::output::ComponentFrame> &componentFrames) override;

private:
    // Per-field carrier calibration + line identification result.
    struct FieldIdent {
        // Measured undeviated carriers (Hz); nominal when unmeasurable.
        double fob = 0.0;
        double for_ = 0.0;
        // Component of field line l is (l + anchor) & 1 (0 = Db, 1 = Dr).
        int32_t anchor = 0;
        // Fraction of measurable lines agreeing with the majority lattice.
        double confidence = 0.0;
        // Matches chd_chroma_ident_mechanism_t.
        int32_t mechanism = 0;
    };

    // A candidate anchor from one measurement mechanism.
    struct AnchorFit {
        bool valid = false;
        int32_t anchor = 0;
        double confidence = 0.0;
        int32_t measuredRows = 0;
    };

    void decodeField(const SourceField &inputField,
                     chd::output::ComponentFrame &componentFrame,
                     FieldIdent &identOut);

    // Fixed lattice anchor for IdentMode::Manual, extended from the
    // configured frame-0 component by the BR.469 alternation.
    int32_t manualAnchorForField(int32_t seqNo) const;

    // (Re)build maskChroma/maskBand and mixFrequency for the current
    // carrierOffset (where the capture's FM block sits relative to nominal).
    void buildChromaMasks();

    // Overlap-save helpers over the flat field buffer (rows are contiguous
    // in time). Each processes one row of `width` samples with `margin`
    // context on both sides.
    void filterRowAnalytic(const uint16_t *fieldData, int32_t numSamples, int32_t row,
                           std::complex<double> *analyticRow, double *chromaRow);
    void filterRowDeemphasis(const double *demod, int32_t numSamples, int32_t row,
                             double *deemphRow);

    // Instantaneous frequency (Hz) over one row via the differentiating FIR
    // on the analytic signal.
    void discriminateRow(const std::complex<double> *analytic, int32_t numSamples,
                         int32_t row, double *fInstRow);

    SecamConfiguration config;
    bool configurationSet = false;

    int32_t width = 0;
    int32_t height = 0;
    double sampleRate = 0.0;

    // Block FFT geometry: blockSize = 2^n >= width + 2*margin.
    int32_t blockSize = 0;
    int32_t margin = 0;
    fftw_complex *fftIn = nullptr;
    fftw_complex *fftFreq = nullptr;
    fftw_complex *fftWork = nullptr;
    fftw_complex *fftOut = nullptr;
    fftw_plan forwardPlan = nullptr;
    fftw_plan inversePlan = nullptr;

    // Frequency-domain masks, one value per FFT bin.
    std::vector<std::complex<double>> maskChroma;  // analytic x band x inverse bell
    std::vector<double> maskBand;                  // hermitian band (chroma reconstruction)
    std::vector<std::complex<double>> maskDeemphasis;  // inverse LF pre-correction

    // Differentiating FIR taps (odd length, antisymmetric), applied to the
    // analytic signal after it is mixed down by mixFrequency.
    std::vector<double> diffTaps;
    double mixFrequency = 0.0;

    // Measured displacement of the FM block from the nominal carriers (Hz);
    // the masks and mixFrequency are built for this offset.
    double carrierOffset = 0.0;

    // Porch measurement window (sample positions within a row).
    int32_t porchStart = 0;
    int32_t porchEnd = 0;

    // Per-field scratch, sized width*height.
    std::vector<std::complex<double>> analytic;
    std::vector<double> chromaRecon;
    std::vector<double> fInst;
    std::vector<double> demod;
    std::vector<double> deemph;
};

}  // namespace chd::decoders::secam

#endif  // CHD_DECODERS_SECAM_SECAM_DECODER_H
