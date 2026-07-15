// SPDX-License-Identifier: GPL-3.0-or-later
//
// Video Standard Preset definitions, per the CVBS file format specification
// (cvbs-file-format-specification/docs/video-standard-presets.md).
//
// A Video Standard Preset fully defines timing and structural parameters for a
// video standard: sampling rate, line counts, field structure, and the
// normative sample-level tables. The presets are exposed as constexpr DATA
// tables, not switch statements, so adding a preset is one row.

#ifndef CHD_FORMAT_VIDEO_STANDARDS_H
#define CHD_FORMAT_VIDEO_STANDARDS_H

#include <cstdint>
#include <optional>
#include <string>

#include "../metadata/core.h"
#include "sample_encoding.h"
#include "signal_state.h"

namespace chd::format {

// Identifies the Video Standard Preset. These are the names the CVBS spec
// allows in the `preset` column of the `cvbs_file` SQLite metadata table.
enum class VideoStandard {
    PAL = 0,
    NTSC,
    PAL_M,
};

// Container addressing: how samples are blocked in the file. Independent of
// the sampling lattice (isSubcarrierLocked); CVBS-native NTSC is
// orthogonal-sampled yet frame-addressed.
enum class FrameLayout {
    UNKNOWN = 0,     // resolve from override / sidecar frame count / file size
    FIELD_RASTER,    // fixed fieldWidth x fieldHeight, field-addressed
    FRAME_NATIVE,    // the CVBS spec's exact frame totals, frame-addressed
};

// Horizontal alignment of the stored rows: where 0H sits within a row (the
// phase of the line cut relative to the signal). Named by what the row starts
// with. The burst gate and active-video windows are only meaningful relative to this.
enum class HorizontalAlignment {
    SYNC_START = 0,   // rows begin at the sync leading edge (line-locked TBC)
    BLANKING_START,   // rows begin at the first digital blanking sample
                      // (ld-chroma-encoder scLocked; frame-native conform)
    ACTIVE_START,     // rows begin at the first digital active sample (the
                      // EBU/SMPTE line-numbering origin; recognised by the
                      // open-time measurement but not yet a served layout)
};

// Per-preset sample level table in the 10-bit domain (0..1023).
// These are the normative values defined in video-standard-presets.md and
// apply when the Sample Encoding Preset is CVBS_U10_4FSC, CVBS_U16_4FSC, or
// CVBS_TPG21_4FSC. They are recorded in the 10-bit domain here; the stored
// integer-domain values are produced by the Sample Encoding Preset.
struct SampleLevels10b {
    int32_t syncTip;
    int32_t blanking;
    int32_t black;
    int32_t white;
    int32_t peak;
    // Reserved exclusion ranges: [0..protectedMinMax] and [protectedMaxMin..1023]
    int32_t protectedMinMax;
    int32_t protectedMaxMin;
};

// Static description of a Video Standard Preset.
struct VideoStandardPreset {
    VideoStandard standard;
    const char *name;
    // Bridge to the VideoSystem enum used throughout the existing metadata
    // and decoder code paths.
    chd::metadata::VideoSystem videoSystem;

    // Sampling rate (Hz). For PAL this is exactly 4 * 625 * 25 * (1135/4 +
    // 1/625); for NTSC, 4 * 525 * (30000/1001) * (455/2); for PAL-M, 4 * 525
    // * (30000/1001) * (909/4). The values are double-precision; the spec's
    // formulas are evaluated at compile time below.
    double sampleRateHz;

    // Colour subcarrier frequency (Hz).
    double fSC;

    // Total samples per line. For PAL this is the per-line average value
    // 1135.0064 (non-orthogonal); only the per-frame total is normative there.
    // For NTSC and PAL_M the per-line value is exact and orthogonal.
    double samplesPerLineAvg;

    // Exact normative samples-per-frame at standard 4xfsc when
    // tbc_applied=TRUE (per video-standard-presets.md "Exact frame size"
    // tables). For PAL: 709,379. For NTSC: 477,750. For PAL-M: 477,225.
    int32_t samplesPerFrame;

    // Line counts. PAL is 625 lines/frame; NTSC and PAL-M are 525.
    int32_t linesPerFrame;

    // Colour frame sequence length in frames. PAL and PAL_M repeat over 4
    // frames; NTSC repeats over 2.
    int32_t colourFrameSequenceLength;

    // Row-local 0H position, in samples, when rows begin at the first digital
    // blanking sample (EBU 3280 / SMPTE 244M numbering: PAL 957.5 - 948, NTSC
    // 784 + 33/90 - 768; PAL_M carries the NTSC timing to its 909-sample line).
    double zeroHBlankingStartRow;

    // Burst gate relative to 0H, in samples: gate opens burstStartFromZeroH
    // after 0H and stays open for burstLengthSamples (PAL: 5.6 us + 10 cycles;
    // NTSC/PAL_M: 19 cycles + 9 cycles).
    double  burstStartFromZeroH;
    int32_t burstLengthSamples;

    // Digital active samples per line, per the interface standard (EBU Tech
    // 3280-E PAL 948; SMPTE ST 244 NTSC/PAL_M 768). This is the default
    // active-video crop width; see makeVideoParameters.
    int32_t digitalActiveSamples;

    // Sample level table in the 10-bit domain.
    SampleLevels10b levels;
};

// Look up a preset by its uppercase ASCII name as defined in the spec
// ("PAL", "NTSC", "PAL_M"). Returns nullptr if the name is unrecognised; an
// unrecognised preset MUST NOT be silently interpreted (spec §4.2).
const VideoStandardPreset *findVideoStandardByName(const std::string &name);

// Look up a preset by enum. Returns a stable pointer into the static table.
const VideoStandardPreset &getVideoStandard(VideoStandard standard);

// Populate a VideoParameters from a preset + signal-state knowledge, using
// the spec's per-frame sample counts and orthogonal-line assumption for
// NTSC/PAL_M. For PAL the orthogonal-line simplification treats each
// frame as if orthogonal at ~1135 samples/line so PALcolour's existing
// assumption holds. Levels are folded into the 16-bit-shifted domain that
// the existing decoders use (10-bit value * 64).
//
// signalState selects between STANDARD_TBC_LOCKED and STANDARD_TBC_UNLOCKED
// for the isSubcarrierLocked flag; raw / non-standard states still fill in
// nominal values but the decoder layer is responsible for refusing them.
chd::metadata::LdDecodeMetaData::VideoParameters
makeVideoParameters(const VideoStandardPreset &preset,
                    bool isSubcarrierLocked,
                    HorizontalAlignment alignment = HorizontalAlignment::SYNC_START);

// As makeVideoParameters, but for a CVBS source: derives isSubcarrierLocked
// from the container layout (frame-native PAL is the only subcarrier-locked
// lattice; a caller override can mark a subcarrier-locked field raster) and
// applies an optional `.meta` black_level override (in the Sample Encoding's
// integer domain) over the preset default.
// alignmentOverride replaces the derived horizontal alignment (field rasters
// are sync-start unless the subcarrier-locked override marks an encoder-style
// raster); frame-native sources pass the value resolved from the open-time 0H
// measurement (resolveFrameNativeAlignment).
chd::metadata::LdDecodeMetaData::VideoParameters
makeCvbsVideoParameters(const VideoStandardPreset &preset,
                        SampleEncoding encoding,
                        std::optional<int32_t> blackLevelOverride = std::nullopt,
                        FrameLayout layout = FrameLayout::FIELD_RASTER,
                        std::optional<bool> subcarrierLockedOverride = std::nullopt,
                        std::optional<HorizontalAlignment> alignmentOverride = std::nullopt);

// Samples per frame when each field is padded to the fixed
// fieldWidth x fieldHeight raster (2 x 1135 x 313 for PAL, etc.).
int32_t fieldPackedSamplesPerFrame(const VideoStandardPreset &preset);

// Resolve the container layout for a CVBS data file. Order: an explicit
// override wins; frame totals are only normative for TBC-applied
// standard-rate states, so anything else is FIELD_RASTER; a sidecar-declared
// frame count divides the file size exactly and unambiguously; otherwise a
// file-size modulo test against the two candidate totals, falling back to
// FIELD_RASTER (with a warning) on ties, truncation, or no match.
FrameLayout resolveFrameLayout(FrameLayout overrideLayout,
                               const VideoStandardPreset &preset,
                               SignalState signalState,
                               int32_t bytesPerSample,
                               int64_t fileSize,
                               std::optional<int64_t> declaredFrames = std::nullopt);

// Contiguous read covering field-raster rows [firstRow0, lastRow0] of one
// field within a frame-native file, plus the count of blanking samples to
// append after it (the second field's tail past the native frame total).
// The mapping is a flat cut of the native frame into the two field buffers,
// bit-identical to ld-chroma-encoder's layout.
struct FrameNativeFieldRead {
    int64_t startByte;
    int64_t numBytes;
    int32_t padSamples;
};
FrameNativeFieldRead planFrameNativeFieldRead(const VideoStandardPreset &preset,
                                              int32_t fieldIndex,
                                              int32_t firstRow0,
                                              int32_t lastRow0,
                                              int32_t bytesPerSample);

// Measure the row-local 0H position from canonical-domain samples: per row,
// locate the half-amplitude falling sync edge (sustained high before,
// sustained low after) and return the median position across rows. Returns
// nullopt when fewer than half the rows yield a clean edge (e.g. synthetic
// data with no sync structure). Levels are the preset's 10-bit values.
std::optional<double> measureRowZeroH(const uint16_t *samples,
                                      int32_t numRows,
                                      int32_t rowWidth,
                                      const SampleLevels10b &levels);

// Choose a frame-native source's horizontal alignment from the measured
// row-local 0H: within tolerance of the blanking-start 0H selects
// BLANKING_START (encoder-flattened data); within tolerance of the row
// start, SYNC_START (what real hardware captures measure as). Active-start
// rows warn: they cannot be served correctly without a row re-cut.
// Unmeasurable or unmatched signals fall back to SYNC_START. Logs the
// outcome; sourceName prefixes the log lines.
HorizontalAlignment resolveFrameNativeAlignment(const VideoStandardPreset &preset,
                                                const std::optional<double> &measuredZeroH,
                                                const char *sourceName);

// Measure the back-porch chroma reference of a 625/50 capture: the per-line
// dominant frequency over the [porchStart, porchEnd) window across `numRows`
// consecutive rows, summarised as the mean frequency and the absolute
// difference between even- and odd-row means. A PAL burst reads a near-zero
// alternation around 4.433619 MHz; the SECAM line-ident carrier pair
// alternates by ~fOR-fOB (156.25 kHz). Returns nullopt when too few rows
// carry a measurable reference. Used for the open-time declared-vs-measured
// warning only; it never switches decode semantics.
struct ChromaPorchSignature {
    double meanHz;
    double alternationHz;
};
std::optional<ChromaPorchSignature> measure625ChromaPorchSignature(
    const uint16_t *rows, int32_t numRows, int32_t rowWidth,
    int32_t porchStart, int32_t porchEnd, double sampleRateHz,
    int32_t blanking16bIre, int32_t white16bIre);

// Measure one NTSC field's colour burst polarity from canonical-domain
// samples: the RS-170 "positive burst phase on even field lines" flag that
// positions the field within the four-field sequence. `rows` holds numRows
// consecutive field lines starting at 0-based field line firstRow, each
// rowWidth samples, with the burst gated by [burstStart, burstEnd) sample
// positions. Returns nullopt when the burst is too weak or the per-line
// votes disagree (no burst, non-4fsc data, or a lattice-unlocked capture).
std::optional<bool> measureNtscFieldBurstPolarity(const uint16_t *rows,
                                                  int32_t firstRow,
                                                  int32_t numRows,
                                                  int32_t rowWidth,
                                                  int32_t burstStart,
                                                  int32_t burstEnd,
                                                  int32_t blanking16bIre,
                                                  int32_t white16bIre);

}  // namespace chd::format

#endif  // CHD_FORMAT_VIDEO_STANDARDS_H
