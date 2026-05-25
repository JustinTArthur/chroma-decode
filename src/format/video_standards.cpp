// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_standards.h"

#include <cstring>

namespace chd::format {

// EBU 3280 (PAL) sample-level table.
static constexpr SampleLevels10b PAL_LEVELS = {
    /*syncTip*/        4,
    /*blanking*/       256,
    /*black*/          282,
    /*white*/          844,
    /*peak*/           1019,
    /*protectedMinMax*/ 3,
    /*protectedMaxMin*/ 1020,
};

// SMPTE ST.0244 (NTSC, PAL_M) sample-level table.
static constexpr SampleLevels10b ST0244_LEVELS = {
    /*syncTip*/        16,
    /*blanking*/       240,
    /*black*/          252,
    /*white*/          800,
    /*peak*/           988,
    /*protectedMinMax*/ 3,
    /*protectedMaxMin*/ 1020,
};

// Sampling-rate formulas evaluated at compile time. Constants from the spec.
static constexpr double PAL_SAMPLE_RATE   = 4.0 * 625.0 * 25.0 * (1135.0 / 4.0 + 1.0 / 625.0);
static constexpr double NTSC_SAMPLE_RATE  = 4.0 * 525.0 * (30000.0 / 1001.0) * (455.0 / 2.0);
static constexpr double PALM_SAMPLE_RATE  = 4.0 * 525.0 * (30000.0 / 1001.0) * (909.0 / 4.0);

// Colour subcarrier frequencies. PAL fSC = 283.75 * 15625 + 25 Hz; NTSC fSC =
// 315 MHz / 88; PAL-M fSC = 5 MHz * (63/88) * (909/910). Match the values
// already initialised in src/metadata/core.cpp's VideoSystemDefaults so the
// CVBS path and the TBC path agree.
static constexpr double PAL_FSC  = 283.75 * 15625.0 + 25.0;
static constexpr double NTSC_FSC = 315.0e6 / 88.0;
static constexpr double PALM_FSC = 5.0e6 * (63.0 / 88.0) * (909.0 / 910.0);

static constexpr VideoStandardPreset PRESETS[] = {
    // PAL: 625 lines, 4-frame colour sequence. Per the spec the per-line
    // average is 1135.0064 (non-orthogonal); the normative invariant is the
    // 709,379-sample frame total.
    {
        VideoStandard::PAL,  "PAL",  chd::metadata::PAL,
        PAL_SAMPLE_RATE, PAL_FSC,
        1135.0 + 1.0 / 156.25,   // = 1135.0064; spec value
        709379,
        625,
        4,
        PAL_LEVELS,
    },
    // NTSC: 525 lines, exact 910 samples/line, 2-frame colour sequence.
    {
        VideoStandard::NTSC, "NTSC", chd::metadata::NTSC,
        NTSC_SAMPLE_RATE, NTSC_FSC,
        910.0,
        477750,
        525,
        2,
        ST0244_LEVELS,
    },
    // PAL_M: 525 lines, exact 909 samples/line, 4-frame colour sequence.
    {
        VideoStandard::PAL_M, "PAL_M", chd::metadata::PAL_M,
        PALM_SAMPLE_RATE, PALM_FSC,
        909.0,
        477225,
        525,
        4,
        ST0244_LEVELS,
    },
};

const VideoStandardPreset *findVideoStandardByName(const std::string &name)
{
    for (const auto &preset : PRESETS) {
        if (name == preset.name) return &preset;
    }
    return nullptr;
}

const VideoStandardPreset &getVideoStandard(VideoStandard standard)
{
    return PRESETS[static_cast<size_t>(standard)];
}

chd::metadata::LdDecodeMetaData::VideoParameters
makeVideoParameters(const VideoStandardPreset &preset,
                    bool isSubcarrierLocked)
{
    chd::metadata::LdDecodeMetaData::VideoParameters vp;

    vp.system = preset.videoSystem;
    vp.isSubcarrierLocked = isSubcarrierLocked;
    vp.isWidescreen = false;

    vp.sampleRate = preset.sampleRateHz;
    vp.fSC        = preset.fSC;

    // Sample-domain values are 10-bit values shifted left 6 (i.e. multiplied
    // by 64), matching the TBC convention so existing decoders can
    // consume the data unchanged.
    vp.blanking16bIre = preset.levels.blanking * 64;
    vp.black16bIre    = preset.levels.black    * 64;
    vp.white16bIre    = preset.levels.white    * 64;

    // Field width: for the orthogonal-line simplification PAL is
    // treated as if orthogonal at 1135 samples/line so PALcolour's existing
    // assumption holds. NTSC and PAL_M are genuinely orthogonal.
    const int32_t samplesPerLine =
        (preset.standard == VideoStandard::PAL)
            ? 1135
            : static_cast<int32_t>(preset.samplesPerLineAvg);
    const int32_t linesPerField = preset.linesPerFrame / 2 + 1;  // 263 for 525, 313 for 625

    vp.fieldWidth  = samplesPerLine;
    vp.fieldHeight = linesPerField;

    // Active region defaults match the legacy metadata's per-system defaults
    // (see VIDEO_SYSTEM_DEFAULTS in src/metadata/core.cpp). The decoder
    // pipeline calls processLineParameters() later to populate
    // firstActiveFrameLine / lastActiveFrameLine; CVBS-opened captures don't
    // yet have a metadata sidecar that overrides these, so populate the
    // defaults explicitly so chd_video_get_info reports sensible values.
    if (preset.standard == VideoStandard::PAL) {
        vp.firstActiveFieldLine = 22;
        vp.lastActiveFieldLine  = 308;
        vp.firstActiveFrameLine = 44;
        vp.lastActiveFrameLine  = 620;
    } else {
        vp.firstActiveFieldLine = 20;
        vp.lastActiveFieldLine  = 263;
        vp.firstActiveFrameLine = 40;
        vp.lastActiveFrameLine  = 525;
    }

    // Colour-burst start/end and active-video start/end are not normatively
    // defined by the Video Standard Preset on its own — they're sample-index
    // ranges that depend on the analogue line timing within the digital
    // raster. The values here are conservative defaults; a metadata override
    // (chd_video_params_t) wins when the caller supplies one.
    vp.activeVideoStart =
        (preset.standard == VideoStandard::PAL) ? 185 : 134;
    vp.activeVideoEnd =
        (preset.standard == VideoStandard::PAL) ? 1107 : 894;
    vp.colourBurstStart =
        (preset.standard == VideoStandard::PAL) ? 98 : 75;
    vp.colourBurstEnd =
        (preset.standard == VideoStandard::PAL) ? 138 : 95;

    vp.isMapped = false;
    vp.isValid  = true;
    return vp;
}

}  // namespace chd::format
