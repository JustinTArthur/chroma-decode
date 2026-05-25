// SPDX-License-Identifier: GPL-3.0-or-later
//
// format/ DATA-table sanity tests. Verifies that preset lookup by
// name (the path the CVBS sidecar reader takes) returns the same constexpr
// rows as lookup by enum, and that the spec-normative sample counts /
// sample-level mappings made it into the table.

#include <cstdint>
#include <iostream>
#include <string>

#include "../../src/format/sample_encoding.h"
#include "../../src/format/signal_state.h"
#include "../../src/format/video_standards.h"

namespace {

#define REQUIRE(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #cond "\n"; \
        return 1; \
    } \
} while (0)

int testVideoStandardLookup() {
    using namespace chd::format;
    REQUIRE(findVideoStandardByName("PAL") == &getVideoStandard(VideoStandard::PAL));
    REQUIRE(findVideoStandardByName("NTSC") == &getVideoStandard(VideoStandard::NTSC));
    REQUIRE(findVideoStandardByName("PAL_M") == &getVideoStandard(VideoStandard::PAL_M));
    REQUIRE(findVideoStandardByName("UNKNOWN") == nullptr);
    REQUIRE(findVideoStandardByName("ntsc") == nullptr);  // case-sensitive

    // Spec-normative sample counts (video-standard-presets.md "Exact frame size").
    REQUIRE(getVideoStandard(VideoStandard::PAL).samplesPerFrame == 709379);
    REQUIRE(getVideoStandard(VideoStandard::NTSC).samplesPerFrame == 477750);
    REQUIRE(getVideoStandard(VideoStandard::PAL_M).samplesPerFrame == 477225);

    // Colour-frame sequence lengths.
    REQUIRE(getVideoStandard(VideoStandard::PAL).colourFrameSequenceLength == 4);
    REQUIRE(getVideoStandard(VideoStandard::NTSC).colourFrameSequenceLength == 2);
    REQUIRE(getVideoStandard(VideoStandard::PAL_M).colourFrameSequenceLength == 4);

    // Sample level tables (in the 10-bit domain).
    REQUIRE(getVideoStandard(VideoStandard::PAL).levels.blanking == 256);
    REQUIRE(getVideoStandard(VideoStandard::PAL).levels.black == 282);
    REQUIRE(getVideoStandard(VideoStandard::NTSC).levels.blanking == 240);
    REQUIRE(getVideoStandard(VideoStandard::NTSC).levels.black == 252);
    return 0;
}

int testSampleEncodingLookup() {
    using namespace chd::format;
    REQUIRE(findSampleEncodingByName("CVBS_U10_4FSC")->encoding == SampleEncoding::CVBS_U10_4FSC);
    REQUIRE(findSampleEncodingByName("CVBS_U16_4FSC")->encoding == SampleEncoding::CVBS_U16_4FSC);
    REQUIRE(findSampleEncodingByName("CVBS_TPG21_4FSC")->encoding == SampleEncoding::CVBS_TPG21_4FSC);
    REQUIRE(findSampleEncodingByName("RAW_S16_28M")->encoding == SampleEncoding::RAW_S16_28M);
    REQUIRE(findSampleEncodingByName("RAW_S16_40M")->encoding == SampleEncoding::RAW_S16_40M);
    REQUIRE(findSampleEncodingByName("does-not-exist") == nullptr);

    REQUIRE(getSampleEncoding(SampleEncoding::CVBS_U10_4FSC).hasStandardAmplitudeMapping);
    REQUIRE(!getSampleEncoding(SampleEncoding::RAW_S16_28M).hasStandardAmplitudeMapping);
    return 0;
}

int testSampleConversion() {
    using namespace chd::format;
    // CVBS_U10_4FSC: raw is 10-bit value; × 64 to get canonical TBC domain.
    REQUIRE(convertCompositeSampleToCanonical(SampleEncoding::CVBS_U10_4FSC, 256) == 16384);
    REQUIRE(convertCompositeSampleToCanonical(SampleEncoding::CVBS_U10_4FSC, 0) == 0);
    REQUIRE(convertCompositeSampleToCanonical(SampleEncoding::CVBS_U10_4FSC, 1019) == 65216);

    // CVBS_U16_4FSC: raw is already the 10-bit × 64 value in unsigned 16-bit.
    REQUIRE(convertCompositeSampleToCanonical(SampleEncoding::CVBS_U16_4FSC, static_cast<int16_t>(16384)) == 16384);
    REQUIRE(convertCompositeSampleToCanonical(SampleEncoding::CVBS_U16_4FSC, static_cast<int16_t>(-32768)) == 32768);

    // CVBS_TPG21_4FSC: int16 = (val10 - 508) × 64; blanking 256 → int16 = -16128.
    REQUIRE(convertCompositeSampleToCanonical(SampleEncoding::CVBS_TPG21_4FSC, -16128) == 16384);
    REQUIRE(convertCompositeSampleToCanonical(SampleEncoding::CVBS_TPG21_4FSC, 0) == 32512);  // val10 = 508

    // Chroma centred at 512 → centred excursion × 64.
    REQUIRE(convertChromaSampleToCenteredCanonical(SampleEncoding::CVBS_U10_4FSC, 512) == 0);
    REQUIRE(convertChromaSampleToCenteredCanonical(SampleEncoding::CVBS_U10_4FSC, 600) == (600 - 512) * 64);
    REQUIRE(convertChromaSampleToCenteredCanonical(SampleEncoding::CVBS_U10_4FSC, 400) == (400 - 512) * 64);

    // CVBS_U16_4FSC chroma: 32768 in u16 is chroma zero.
    REQUIRE(convertChromaSampleToCenteredCanonical(SampleEncoding::CVBS_U16_4FSC,
                                                   static_cast<int16_t>(32768)) == 0);
    return 0;
}

int testSignalStateLookup() {
    using namespace chd::format;
    REQUIRE(findSignalStateByName("STANDARD_TBC_LOCKED")->state == SignalState::STANDARD_TBC_LOCKED);
    REQUIRE(findSignalStateByName("STANDARD_RAW")->burstLocked == false);
    REQUIRE(findSignalStateByName("STANDARD_RAW")->tbcApplied == false);
    REQUIRE(findSignalStateByName("NONSTANDARD_TBC_LOCKED")->standardRate == false);
    REQUIRE(findSignalStateByName("invalid") == nullptr);

    REQUIRE(isDecoderCompatible(SignalState::STANDARD_TBC_LOCKED));
    REQUIRE(isDecoderCompatible(SignalState::STANDARD_TBC_UNLOCKED));
    REQUIRE(!isDecoderCompatible(SignalState::STANDARD_RAW));
    REQUIRE(!isDecoderCompatible(SignalState::NONSTANDARD_RAW));
    return 0;
}

int testMakeVideoParameters() {
    using namespace chd::format;
    // NTSC at STANDARD_TBC_LOCKED → orthogonal 910x263 field, blanking at
    // 240 × 64 = 15360, isSubcarrierLocked = true.
    auto vp = makeVideoParameters(getVideoStandard(VideoStandard::NTSC), true);
    REQUIRE(vp.fieldWidth == 910);
    REQUIRE(vp.fieldHeight == 263);
    REQUIRE(vp.blanking16bIre == 15360);
    REQUIRE(vp.black16bIre == 16128);
    REQUIRE(vp.white16bIre == 51200);
    REQUIRE(vp.isSubcarrierLocked == true);
    REQUIRE(vp.system == chd::metadata::NTSC);

    // PAL orthogonal-line simplification: 1135 samples/line, 313 lines/field.
    auto pal = makeVideoParameters(getVideoStandard(VideoStandard::PAL), false);
    REQUIRE(pal.fieldWidth == 1135);
    REQUIRE(pal.fieldHeight == 313);
    REQUIRE(pal.blanking16bIre == 16384);
    REQUIRE(pal.isSubcarrierLocked == false);
    REQUIRE(pal.system == chd::metadata::PAL);

    // PAL_M: 909 samples/line, 263 lines/field.
    auto palm = makeVideoParameters(getVideoStandard(VideoStandard::PAL_M), true);
    REQUIRE(palm.fieldWidth == 909);
    REQUIRE(palm.fieldHeight == 263);
    REQUIRE(palm.system == chd::metadata::PAL_M);
    return 0;
}

}  // namespace

int main() {
    if (testVideoStandardLookup() != 0) return 1;
    if (testSampleEncodingLookup() != 0) return 1;
    if (testSampleConversion() != 0) return 1;
    if (testSignalStateLookup() != 0) return 1;
    if (testMakeVideoParameters() != 0) return 1;
    std::cout << "All format-preset tests passed.\n";
    return 0;
}
