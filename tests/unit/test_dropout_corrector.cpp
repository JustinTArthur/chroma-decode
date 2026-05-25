// SPDX-License-Identifier: GPL-3.0-or-later
//
// Dropout corrector unit test. Builds a synthetic NTSC field pair
// in memory, injects a known dropout region, runs DropoutCorrector::
// correctFrame, and asserts the stats accounting + sample replacement.
//
// Single-source path is exercised first because it's the vapoursynth-
// analog feature parity bar. Multi-source path is exercised separately so a
// regression there shows up as a distinct failure.

#include <cstdint>
#include <iostream>
#include <vector>

#include "../../src/dropout/dropout_corrector.h"
#include "../../src/format/video_standards.h"
#include "../../src/metadata/core.h"
#include "../../src/metadata/dropouts.h"
#include "../../src/decoders/source_field.h"
#include "../../src/reader/source.h"

namespace {

#define REQUIRE(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #cond "\n"; \
        return 1; \
    } \
} while (0)

// Sentinel sample value that marks "this pixel was a dropout" before
// correction. Anything inside the dropout range that still reads this value
// after correctFrame returns means the corrector failed to overwrite it.
constexpr uint16_t kDropoutSentinel = 0xDEAD;

// Build a synthetic SourceField pair. Every line gets a per-line constant
// value so we can tell which neighbour the corrector pulled from after it
// finishes — line N's data is filled with value `lineFill(N)`.
struct SyntheticFrame {
    chd::decoders::SourceField first;
    chd::decoders::SourceField second;
    chd::metadata::LdDecodeMetaData::VideoParameters vp;
};

uint16_t lineFill(int32_t lineNumber, int32_t fieldOffset) {
    // Returns a stable per-line value distinct between fields so the
    // intra-field replacement path is observable.
    return static_cast<uint16_t>(0x1000 + (lineNumber * 4) + fieldOffset);
}

SyntheticFrame buildSyntheticNtscFrame() {
    using namespace chd::format;
    const VideoStandardPreset &preset = getVideoStandard(VideoStandard::NTSC);
    SyntheticFrame f;
    f.vp = makeVideoParameters(preset, true);

    const int32_t fieldSamples = f.vp.fieldWidth * f.vp.fieldHeight;

    f.first.field.isFirstField = true;
    f.first.field.seqNo = 1;
    f.first.field.vitsMetrics.inUse = true;
    f.first.field.vitsMetrics.bPSNR = 40.0;
    f.first.data.assign(fieldSamples, 0);

    f.second.field.isFirstField = false;
    f.second.field.seqNo = 2;
    f.second.field.vitsMetrics.inUse = true;
    f.second.field.vitsMetrics.bPSNR = 40.0;
    f.second.data.assign(fieldSamples, 0);

    // Fill each line with a stable, distinguishable value.
    for (int32_t line = 0; line < f.vp.fieldHeight; line++) {
        uint16_t firstVal = lineFill(line + 1, 0);
        uint16_t secondVal = lineFill(line + 1, 1);
        for (int32_t x = 0; x < f.vp.fieldWidth; x++) {
            f.first.data[line * f.vp.fieldWidth + x] = firstVal;
            f.second.data[line * f.vp.fieldWidth + x] = secondVal;
        }
    }
    return f;
}

int testSingleSourceCorrection() {
    SyntheticFrame f = buildSyntheticNtscFrame();
    const int32_t targetLine = (f.vp.firstActiveFieldLine + f.vp.lastActiveFieldLine) / 2;
    const int32_t dropoutStart = f.vp.activeVideoStart + 100;
    const int32_t dropoutEnd = f.vp.activeVideoStart + 200;

    // Pre-poison the dropout region in the first field so we can detect
    // whether the corrector wrote over it. Targeting line `targetLine`
    // (1-based), so the data range to poison is the (targetLine-1)th row.
    for (int32_t x = dropoutStart; x < dropoutEnd; x++) {
        f.first.data[(targetLine - 1) * f.vp.fieldWidth + x] = kDropoutSentinel;
    }

    chd::metadata::DropOuts d;
    d.append(dropoutStart, dropoutEnd, targetLine);
    f.first.field.dropOuts = d;

    chd::dropout::DropoutCorrector corrector(f.vp);
    chd::dropout::DropoutCorrectionStats stats;
    corrector.correctFrame(f.first, f.second, /*overCorrect=*/false,
                           /*intraField=*/false, &stats);

    REQUIRE(stats.corrected == 1);
    REQUIRE(stats.failed == 0);
    REQUIRE(stats.totalDistance > 0);

    // Inspect the corrected pixels — every previously-poisoned pixel must
    // have been replaced. The replacement could be from the same field
    // (luma path) or filtered against another line; verify that none of
    // them are still the sentinel.
    for (int32_t x = dropoutStart; x < dropoutEnd; x++) {
        const uint16_t v = f.first.data[(targetLine - 1) * f.vp.fieldWidth + x];
        REQUIRE(v != kDropoutSentinel);
    }
    return 0;
}

int testIntraFieldFallback() {
    // intraField=true forbids cross-field replacement candidates. The
    // corrector should still find a same-field neighbour and the stats
    // should reflect a successful correction.
    SyntheticFrame f = buildSyntheticNtscFrame();
    const int32_t targetLine = (f.vp.firstActiveFieldLine + f.vp.lastActiveFieldLine) / 2;
    const int32_t dropoutStart = f.vp.activeVideoStart + 50;
    const int32_t dropoutEnd = f.vp.activeVideoStart + 80;

    for (int32_t x = dropoutStart; x < dropoutEnd; x++) {
        f.first.data[(targetLine - 1) * f.vp.fieldWidth + x] = kDropoutSentinel;
    }

    chd::metadata::DropOuts d;
    d.append(dropoutStart, dropoutEnd, targetLine);
    f.first.field.dropOuts = d;

    chd::dropout::DropoutCorrector corrector(f.vp);
    chd::dropout::DropoutCorrectionStats stats;
    corrector.correctFrame(f.first, f.second, /*overCorrect=*/false,
                           /*intraField=*/true, &stats);
    REQUIRE(stats.corrected == 1);
    REQUIRE(stats.failed == 0);
    for (int32_t x = dropoutStart; x < dropoutEnd; x++) {
        REQUIRE(f.first.data[(targetLine - 1) * f.vp.fieldWidth + x] != kDropoutSentinel);
    }
    return 0;
}

int testNoDropoutsIsNoOp() {
    SyntheticFrame f = buildSyntheticNtscFrame();
    // Stash a copy of the first-field data so we can confirm bit-equality
    // after a no-op correctFrame call.
    chd::reader::Data before = f.first.data;

    chd::dropout::DropoutCorrector corrector(f.vp);
    chd::dropout::DropoutCorrectionStats stats;
    corrector.correctFrame(f.first, f.second, /*overCorrect=*/false,
                           /*intraField=*/false, &stats);
    REQUIRE(stats.corrected == 0);
    REQUIRE(stats.failed == 0);
    REQUIRE(stats.totalDistance == 0);
    REQUIRE(f.first.data == before);
    return 0;
}

int testMultiSourceCorrection() {
    // Build two NTSC frames; primary has a dropout on line L. Extra source
    // is dropout-free in that region, so the corrector should prefer the
    // extra (same line, distance 0) over the primary's same-field neighbour.
    SyntheticFrame primary = buildSyntheticNtscFrame();
    SyntheticFrame extra = buildSyntheticNtscFrame();

    // Distinguish the extra's sample values so we can verify the corrector
    // actually pulled from it.
    constexpr uint16_t kExtraMark = 0xABCD;
    const int32_t targetLine = (primary.vp.firstActiveFieldLine + primary.vp.lastActiveFieldLine) / 2;
    const int32_t dropoutStart = primary.vp.activeVideoStart + 300;
    const int32_t dropoutEnd = primary.vp.activeVideoStart + 320;
    for (int32_t x = 0; x < primary.vp.fieldWidth; x++) {
        extra.first.data[(targetLine - 1) * primary.vp.fieldWidth + x] = kExtraMark;
    }
    for (int32_t x = dropoutStart; x < dropoutEnd; x++) {
        primary.first.data[(targetLine - 1) * primary.vp.fieldWidth + x] = kDropoutSentinel;
    }

    chd::metadata::DropOuts d;
    d.append(dropoutStart, dropoutEnd, targetLine);
    primary.first.field.dropOuts = d;

    chd::dropout::ExtraSourceFrame extraFrame;
    extraFrame.firstFieldData = extra.first.data;
    extraFrame.secondFieldData = extra.second.data;
    extraFrame.firstFieldMeta = extra.first.field;
    extraFrame.secondFieldMeta = extra.second.field;
    extraFrame.videoParams = extra.vp;
    extraFrame.quality = 50.0;  // higher than primary's 40.0

    chd::dropout::DropoutCorrector corrector(primary.vp);
    chd::dropout::DropoutCorrectionStats stats;
    std::vector<chd::dropout::ExtraSourceFrame> extras = {extraFrame};
    corrector.correctFrame(primary.first, primary.second, extras,
                           /*overCorrect=*/false,
                           /*intraField=*/false, &stats);

    REQUIRE(stats.corrected == 1);
    // The extra contributes the same target line with distance 0.
    REQUIRE(stats.totalDistance == 0);
    for (int32_t x = dropoutStart; x < dropoutEnd; x++) {
        const uint16_t v = primary.first.data[(targetLine - 1) * primary.vp.fieldWidth + x];
        REQUIRE(v == kExtraMark);
    }
    return 0;
}

}  // namespace

int main() {
    if (testSingleSourceCorrection()) return 1;
    if (testIntraFieldFallback()) return 1;
    if (testNoDropoutsIsNoOp()) return 1;
    if (testMultiSourceCorrection()) return 1;
    std::cout << "test_dropout_corrector: OK\n";
    return 0;
}
