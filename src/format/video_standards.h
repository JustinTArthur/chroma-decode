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

#include <cstddef>
#include <cstdint>
#include <string>

#include "../metadata/core.h"

namespace chd::format {

// Identifies the Video Standard Preset. These are the names the CVBS spec
// allows in the `preset` column of the `cvbs_file` SQLite metadata table.
enum class VideoStandard {
    PAL = 0,
    NTSC,
    PAL_M,
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
                    bool isSubcarrierLocked);

}  // namespace chd::format

#endif  // CHD_FORMAT_VIDEO_STANDARDS_H
