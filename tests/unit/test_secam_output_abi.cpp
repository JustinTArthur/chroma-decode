// SPDX-License-Identifier: GPL-3.0-or-later
//
// 4:4:0 output paths and the line-sequential frame accessors, with
// synthetic component frames: OutputWriter::convert440/convertToFloat440
// row provenance and geometry, chd_frame_get_plane_info /
// chd_frame_chroma_row_component / chd_frame_get_chroma_ident, and the
// commit-time output-format gating for SECAM sources.

#include <chromadec/decoder.h>
#include <chromadec/frame.h>
#include <chromadec/video.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "../../src/abi/handles.h"
#include "../../src/metadata/core.h"
#include "../../src/output/component_frame.h"
#include "../../src/output/output_writer.h"

namespace fs = std::filesystem;

namespace {

#define REQUIRE(cond)                                                            \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " << #cond \
                      << "\n";                                                   \
            return 1;                                                            \
        }                                                                        \
    } while (0)

// Small SECAM-declared geometry: 19-row interlaced frame, active frame rows
// 2..17 (16 active rows), active samples 4..60.
chd::metadata::LdDecodeMetaData::VideoParameters testParams() {
    chd::metadata::LdDecodeMetaData::VideoParameters vp;
    vp.system = chd::metadata::SECAM;
    vp.fSC = 4286000.0;
    vp.fieldWidth = 64;
    vp.fieldHeight = 10;
    vp.sampleRate = 17734475.0;
    vp.black16bIre = 16384;
    vp.white16bIre = 54016;
    vp.blanking16bIre = 16384;
    vp.activeVideoStart = 4;
    vp.activeVideoEnd = 60;
    vp.firstActiveFrameLine = 2;
    vp.lastActiveFrameLine = 17;
    vp.isValid = true;
    return vp;
}

// The SECAM interlaced-frame component lattice: within each field successive
// lines alternate Db/Dr, and field 2 sits an odd line count after field 1,
// so components pair up in frame-row order (Db, Dr, Dr, Db, Db, ...).
int8_t pairLatticeComponent(int32_t frameRow) {
    return static_cast<int8_t>(((frameRow + 1) >> 1) & 1);
}

// Expected woven plane rows for one component: group its active rows by
// output-row parity and interleave, so plane row 2j+p is the component's
// j-th row on output rows of parity p (the packing contract under test).
std::vector<int32_t> wovenRows(const chd::metadata::LdDecodeMetaData::VideoParameters &vp,
                               int32_t activeHeight, int32_t topPad, int8_t component) {
    std::vector<int32_t> byParity[2];
    for (int32_t y = 0; y < activeHeight; y++) {
        if (pairLatticeComponent(vp.firstActiveFrameLine + y) == component) {
            byParity[(topPad + y) & 1].push_back(y);
        }
    }
    std::vector<int32_t> rows;
    for (size_t j = 0; j < byParity[0].size(); j++) {
        rows.push_back(byParity[0][j]);
        rows.push_back(byParity[1][j]);
    }
    return rows;
}

// Per-row source values, distinct per line so provenance is checkable.
double uValueFor(int32_t line) { return (line - 9) * 100.0; }
double vValueFor(int32_t line) { return line * -55.0; }

chd::output::ComponentFrame makeFrame(
    const chd::metadata::LdDecodeMetaData::VideoParameters &vp) {
    chd::output::ComponentFrame cf;
    cf.init(vp);
    for (int32_t line = 0; line < cf.getHeight(); line++) {
        for (int32_t x = 0; x < cf.getWidth(); x++) {
            cf.y(line)[x] = vp.black16bIre + line * 64.0 + x;
            cf.u(line)[x] = uValueFor(line);
            cf.v(line)[x] = vValueFor(line);
        }
    }
    cf.chromaRowComponents.resize(cf.getHeight());
    for (int32_t line = 0; line < cf.getHeight(); line++) {
        cf.chromaRowComponents[line] = pairLatticeComponent(line);
    }
    cf.chromaIdent.valid = true;
    cf.chromaIdent.mechanism = 0;  // porch
    cf.chromaIdent.confidence = 0.96875;
    cf.chromaIdent.fieldConfidence[0] = 1.0;
    cf.chromaIdent.fieldConfidence[1] = 0.9375;
    return cf;
}

// The normalized-signal scale factors the OutputWriter applies (BT.601 /
// H.273 MatrixCoefficients 5/6; see output_writer.cpp).
constexpr double kBReduction = 0.49211104112248356308804691718185;
constexpr double kRReduction = 0.87728321993817866838972487283129;

double expectedECb(double uValue, double uvRange) {
    return uValue / (2.0 * (1.0 - 0.114) * kBReduction * uvRange);
}
double expectedECr(double vValue, double uvRange) {
    return vValue / (2.0 * (1.0 - 0.299) * kRReduction * uvRange);
}

int testWriter440() {
    auto vp = testParams();
    const auto cf = makeFrame(vp);

    chd::output::OutputWriter writer;
    chd::output::OutputWriter::Configuration cfg;
    cfg.paddingAmount = 1;
    cfg.pixelFormat = chd::output::OutputWriter::YUV440P16;
    cfg.clampMode = chd::output::OutputWriter::CLAMP_NONE;
    writer.updateConfiguration(vp, cfg);

    const int32_t activeWidth = writer.getActiveWidth();
    const int32_t activeHeight = writer.getActiveHeight();
    REQUIRE(activeWidth == 56);
    REQUIRE(activeHeight == 16);

    // Expected woven per-plane row lists over active frame rows 2..17. The
    // literals anchor the contract: the lattice over active rows is
    // Dr, Db, Db, Dr, ..., so the Cb plane is the pairwise-swapped one on
    // this phase and the Cr plane is in picture order.
    const std::vector<int32_t> cbRows = wovenRows(vp, activeHeight, 0, 0);
    const std::vector<int32_t> crRows = wovenRows(vp, activeHeight, 0, 1);
    REQUIRE(cbRows == (std::vector<int32_t>{2, 1, 6, 5, 10, 9, 14, 13}));
    REQUIRE(crRows == (std::vector<int32_t>{0, 3, 4, 7, 8, 11, 12, 15}));

    chd::output::OutputFrame out;
    const auto g = writer.convert440(cf, out);
    REQUIRE(g.cbHeight == static_cast<int32_t>(cbRows.size()));
    REQUIRE(g.crHeight == static_cast<int32_t>(crRows.size()));
    REQUIRE(g.cbHeight == g.crHeight);
    REQUIRE(g.cbFirstRow == cbRows.front());
    REQUIRE(g.crFirstRow == crRows.front());
    REQUIRE(out.size() == static_cast<size_t>(activeWidth)
                              * (activeHeight + g.cbHeight + g.crHeight));

    std::vector<float> planes[3];
    const auto gf = writer.convertToFloat440(cf, planes);
    REQUIRE(gf.cbHeight == g.cbHeight);
    REQUIRE(gf.crHeight == g.crHeight);
    REQUIRE(gf.cbFirstRow == g.cbFirstRow);
    REQUIRE(gf.crFirstRow == g.crFirstRow);
    REQUIRE(planes[0].size() == static_cast<size_t>(activeWidth) * activeHeight);
    REQUIRE(planes[1].size() == static_cast<size_t>(activeWidth) * g.cbHeight);
    REQUIRE(planes[2].size() == static_cast<size_t>(activeWidth) * g.crHeight);

    // Row provenance: chroma plane row k holds exactly the source line the
    // lattice assigns it, in both precisions; the two paths agree to 1 LSB.
    const double uvRange = vp.white16bIre - vp.black16bIre;
    const uint16_t *outCb = out.data() + static_cast<size_t>(activeWidth) * activeHeight;
    const uint16_t *outCr = outCb + static_cast<size_t>(activeWidth) * g.cbHeight;
    for (size_t k = 0; k < cbRows.size(); k++) {
        const int32_t sourceLine = vp.firstActiveFrameLine + cbRows[k];
        const double eCb = expectedECb(uValueFor(sourceLine), uvRange);
        const float got = planes[1][k * activeWidth];
        REQUIRE(std::abs(got - eCb) < 1e-6);
        const double code = std::round(224.0 * 256.0 * eCb + 128.0 * 256.0);
        REQUIRE(std::abs(outCb[k * activeWidth] - code) <= 1.0);
    }
    for (size_t k = 0; k < crRows.size(); k++) {
        const int32_t sourceLine = vp.firstActiveFrameLine + crRows[k];
        const double eCr = expectedECr(vValueFor(sourceLine), uvRange);
        const float got = planes[2][k * activeWidth];
        REQUIRE(std::abs(got - eCr) < 1e-6);
        const double code = std::round(224.0 * 256.0 * eCr + 128.0 * 256.0);
        REQUIRE(std::abs(outCr[k * activeWidth] - code) <= 1.0);
    }

    // The generic convert() entry point routes YUV440P16 to the same layout.
    chd::output::OutputFrame out2;
    writer.convert(cf, out2);
    REQUIRE(out2 == out);
    return 0;
}

// Padding pads the frame and the Y plane; the chroma planes share the padded
// width (neutral side border) and shift with the top border, but never gain
// rows.
int testWriter440Padded() {
    auto vp = testParams();
    const auto cf = makeFrame(vp);

    chd::output::OutputWriter writer;
    chd::output::OutputWriter::Configuration cfg;
    cfg.paddingAmount = 5;
    cfg.pixelFormat = chd::output::OutputWriter::YUV440P16;
    cfg.clampMode = chd::output::OutputWriter::CLAMP_NONE;
    writer.updateConfiguration(vp, cfg);

    // The 56x16 active picture rounds up to 60x20, centred: a two-sample /
    // two-line border on every side.
    const int32_t activeHeight = writer.getActiveHeight();
    const int32_t outputWidth = writer.getOutputWidth();
    const int32_t outputHeight = writer.getOutputHeight();
    const int32_t leftPad = writer.getLeftPadSamples();
    const int32_t topPad = writer.getTopPadLines();
    REQUIRE(outputWidth == 60);
    REQUIRE(outputHeight == 20);
    REQUIRE(leftPad == 2);
    REQUIRE(topPad == 2);

    // topPad is even here, so the parity weave matches the unpadded one and
    // only the reported first rows shift down with the border.
    const std::vector<int32_t> cbRows = wovenRows(vp, activeHeight, topPad, 0);
    const std::vector<int32_t> crRows = wovenRows(vp, activeHeight, topPad, 1);
    REQUIRE(cbRows == (std::vector<int32_t>{2, 1, 6, 5, 10, 9, 14, 13}));
    REQUIRE(crRows == (std::vector<int32_t>{0, 3, 4, 7, 8, 11, 12, 15}));

    chd::output::OutputFrame out;
    const auto g = writer.convert440(cf, out);
    REQUIRE(g.cbHeight == static_cast<int32_t>(cbRows.size()));
    REQUIRE(g.crHeight == static_cast<int32_t>(crRows.size()));
    REQUIRE(g.cbFirstRow == cbRows.front() + topPad);
    REQUIRE(g.crFirstRow == crRows.front() + topPad);
    REQUIRE(out.size() == static_cast<size_t>(outputWidth)
                              * (outputHeight + g.cbHeight + g.crHeight));

    const uint16_t yBlack = 16 * 256;
    const uint16_t cNeutral = 128 * 256;
    const uint16_t *outY = out.data();
    const uint16_t *outCb = outY + static_cast<size_t>(outputWidth) * outputHeight;
    const uint16_t *outCr = outCb + static_cast<size_t>(outputWidth) * g.cbHeight;

    // Y plane: black border rows and columns around a picture whose samples
    // match the unpadded quantization.
    const double yRange = vp.white16bIre - vp.black16bIre;
    for (int32_t x = 0; x < outputWidth; x++) {
        REQUIRE(outY[x] == yBlack);
        REQUIRE(outY[static_cast<size_t>(outputWidth) * (outputHeight - 1) + x] == yBlack);
    }
    for (int32_t y = 0; y < activeHeight; y++) {
        const uint16_t *row = outY + static_cast<size_t>(outputWidth) * (topPad + y);
        REQUIRE(row[0] == yBlack);
        REQUIRE(row[leftPad - 1] == yBlack);
        REQUIRE(row[outputWidth - 1] == yBlack);
        const int32_t sourceLine = vp.firstActiveFrameLine + y;
        const double eY = (sourceLine * 64.0 + vp.activeVideoStart) / yRange;
        const double code = std::round(219.0 * 256.0 * eY + 16.0 * 256.0);
        REQUIRE(std::abs(row[leftPad] - code) <= 1.0);
    }

    // Chroma planes: neutral side borders, and every row is still the real
    // decoded line the lattice assigns it.
    const double uvRange = yRange;
    for (size_t k = 0; k < cbRows.size(); k++) {
        const uint16_t *row = outCb + k * outputWidth;
        REQUIRE(row[0] == cNeutral);
        REQUIRE(row[leftPad - 1] == cNeutral);
        REQUIRE(row[outputWidth - 1] == cNeutral);
        const int32_t sourceLine = vp.firstActiveFrameLine + cbRows[k];
        const double code = std::round(
            224.0 * 256.0 * expectedECb(uValueFor(sourceLine), uvRange) + 128.0 * 256.0);
        REQUIRE(std::abs(row[leftPad] - code) <= 1.0);
    }
    for (size_t k = 0; k < crRows.size(); k++) {
        const uint16_t *row = outCr + k * outputWidth;
        REQUIRE(row[0] == cNeutral);
        REQUIRE(row[outputWidth - 1] == cNeutral);
        const int32_t sourceLine = vp.firstActiveFrameLine + crRows[k];
        const double code = std::round(
            224.0 * 256.0 * expectedECr(vValueFor(sourceLine), uvRange) + 128.0 * 256.0);
        REQUIRE(std::abs(row[leftPad] - code) <= 1.0);
    }

    // Float path: same geometry, zero-valued border (E'Y black and neutral
    // E'Cb/E'Cr are all 0.0), pixel-aligned active samples.
    std::vector<float> planes[3];
    const auto gf = writer.convertToFloat440(cf, planes);
    REQUIRE(gf.cbHeight == g.cbHeight);
    REQUIRE(gf.crHeight == g.crHeight);
    REQUIRE(gf.cbFirstRow == g.cbFirstRow);
    REQUIRE(gf.crFirstRow == g.crFirstRow);
    REQUIRE(planes[0].size() == static_cast<size_t>(outputWidth) * outputHeight);
    REQUIRE(planes[1].size() == static_cast<size_t>(outputWidth) * g.cbHeight);
    REQUIRE(planes[2].size() == static_cast<size_t>(outputWidth) * g.crHeight);
    REQUIRE(planes[0][0] == 0.0f);
    REQUIRE(planes[1][0] == 0.0f);
    REQUIRE(planes[2][0] == 0.0f);
    const double eY0 = (vp.firstActiveFrameLine * 64.0 + vp.activeVideoStart) / yRange;
    REQUIRE(std::abs(planes[0][static_cast<size_t>(topPad) * outputWidth + leftPad] - eY0)
            < 1e-6);
    const double eCb0 = expectedECb(uValueFor(vp.firstActiveFrameLine + cbRows.front()), uvRange);
    REQUIRE(std::abs(planes[1][leftPad] - eCb0) < 1e-6);
    return 0;
}

// An odd top border flips which source-row parity sits on even output rows,
// and the weave follows the output rows: parity still selects the same field
// on the chroma planes as on the emitted luma plane.
int testWriter440OddTopPad() {
    auto vp = testParams();
    const auto cf = makeFrame(vp);

    chd::output::OutputWriter writer;
    chd::output::OutputWriter::Configuration cfg;
    cfg.paddingAmount = 3;
    cfg.pixelFormat = chd::output::OutputWriter::YUV440P16;
    cfg.clampMode = chd::output::OutputWriter::CLAMP_NONE;
    writer.updateConfiguration(vp, cfg);

    // 56x16 rounds up to 57x18 with a single-line top border.
    const int32_t activeHeight = writer.getActiveHeight();
    const int32_t topPad = writer.getTopPadLines();
    REQUIRE(writer.getOutputWidth() == 57);
    REQUIRE(writer.getOutputHeight() == 18);
    REQUIRE(topPad == 1);

    const std::vector<int32_t> cbRows = wovenRows(vp, activeHeight, topPad, 0);
    const std::vector<int32_t> crRows = wovenRows(vp, activeHeight, topPad, 1);
    REQUIRE(cbRows == (std::vector<int32_t>{1, 2, 5, 6, 9, 10, 13, 14}));
    REQUIRE(crRows == (std::vector<int32_t>{3, 0, 7, 4, 11, 8, 15, 12}));

    std::vector<float> planes[3];
    const auto g = writer.convertToFloat440(cf, planes);
    REQUIRE(g.cbHeight == 8);
    REQUIRE(g.crHeight == 8);
    REQUIRE(g.cbFirstRow == cbRows.front() + topPad);
    REQUIRE(g.crFirstRow == crRows.front() + topPad);

    // Plane row parity matches the output-frame row parity of its line.
    for (size_t k = 0; k < cbRows.size(); k++) {
        REQUIRE(static_cast<int32_t>(k & 1) == ((topPad + cbRows[k]) & 1));
    }
    for (size_t k = 0; k < crRows.size(); k++) {
        REQUIRE(static_cast<int32_t>(k & 1) == ((topPad + crRows[k]) & 1));
    }

    // Row provenance holds through the swapped weave.
    const double uvRange = vp.white16bIre - vp.black16bIre;
    const int32_t leftPad = writer.getLeftPadSamples();
    const int32_t outputWidth = writer.getOutputWidth();
    for (size_t k = 0; k < crRows.size(); k++) {
        const int32_t sourceLine = vp.firstActiveFrameLine + crRows[k];
        const double eCr = expectedECr(vValueFor(sourceLine), uvRange);
        REQUIRE(std::abs(planes[2][k * outputWidth + leftPad] - eCr) < 1e-6);
    }
    return 0;
}

int testFrameAccessors() {
    auto vp = testParams();
    const auto cf = makeFrame(vp);

    chd::output::OutputWriter writer;
    chd::output::OutputWriter::Configuration cfg;
    cfg.paddingAmount = 1;
    cfg.pixelFormat = chd::output::OutputWriter::YUV440P16;
    writer.updateConfiguration(vp, cfg);

    // Assemble a chd_frame the way decodeFrameLocked does for YUV440PS.
    chd_frame *frame = new chd_frame;
    frame->format = CHD_PIXEL_YUV440PS;
    frame->chroma440 = writer.convertToFloat440(cf, frame->floatPlane);
    frame->outputWidth = writer.getActiveWidth();
    frame->outputHeight = writer.getOutputHeight();
    frame->rowComponent.resize(frame->outputHeight);
    for (int32_t y = 0; y < frame->outputHeight; y++) {
        frame->rowComponent[y] = cf.chromaRowComponents[vp.firstActiveFrameLine + y];
    }
    frame->identReport.mechanism = CHD_CHROMA_IDENT_PORCH;
    frame->identReport.confidence = cf.chromaIdent.confidence;
    frame->identReport.field_confidence[0] = cf.chromaIdent.fieldConfidence[0];
    frame->identReport.field_confidence[1] = cf.chromaIdent.fieldConfidence[1];
    frame->identReport.first_row_component =
        frame->rowComponent[0] == 1 ? CHD_CHROMA_ROW_DR : CHD_CHROMA_ROW_DB;
    frame->hasIdentReport = true;
    frame->info.format = frame->format;
    frame->info.width = frame->outputWidth;
    frame->info.height = frame->outputHeight;
    frame->info.num_planes = 3;
    frame->info.frame_index = 0;

    chd_plane_info_t pi{};
    REQUIRE(chd_frame_get_plane_info(frame, CHD_PLANE_Y, &pi) == CHD_OK);
    REQUIRE(pi.width == frame->outputWidth);
    REQUIRE(pi.height == frame->outputHeight);
    REQUIRE(pi.first_frame_row == 0);
    REQUIRE(chd_frame_get_plane_info(frame, CHD_PLANE_CB, &pi) == CHD_OK);
    REQUIRE(pi.height == frame->chroma440.cbHeight);
    REQUIRE(pi.first_frame_row == frame->chroma440.cbFirstRow);
    REQUIRE(chd_frame_get_plane_info(frame, CHD_PLANE_CR, &pi) == CHD_OK);
    REQUIRE(pi.height == frame->chroma440.crHeight);
    REQUIRE(pi.first_frame_row == frame->chroma440.crFirstRow);
    REQUIRE(chd_frame_get_plane_info(frame, CHD_PLANE_R, &pi) == CHD_E_INVALID_ARG);

    // Heights are equal and cover every active row.
    REQUIRE(frame->chroma440.cbHeight == frame->chroma440.crHeight);
    REQUIRE(frame->chroma440.cbHeight + frame->chroma440.crHeight == frame->outputHeight);

    chd_chroma_row_component_t comp;
    for (int32_t y = 0; y < frame->outputHeight; y++) {
        REQUIRE(chd_frame_chroma_row_component(frame, y, &comp) == CHD_OK);
        const auto expected = pairLatticeComponent(vp.firstActiveFrameLine + y) == 0
                                  ? CHD_CHROMA_ROW_DB
                                  : CHD_CHROMA_ROW_DR;
        REQUIRE(comp == expected);
    }
    REQUIRE(chd_frame_chroma_row_component(frame, -1, &comp) == CHD_E_OUT_OF_RANGE);
    REQUIRE(chd_frame_chroma_row_component(frame, frame->outputHeight, &comp)
            == CHD_E_OUT_OF_RANGE);

    chd_chroma_ident_report_t report{};
    REQUIRE(chd_frame_get_chroma_ident(frame, &report) == CHD_OK);
    REQUIRE(report.mechanism == CHD_CHROMA_IDENT_PORCH);
    REQUIRE(report.confidence == cf.chromaIdent.confidence);
    REQUIRE(report.field_confidence[1] == cf.chromaIdent.fieldConfidence[1]);
    // Active row 0 is frame row 2 of the pair lattice (Db, Dr, Dr, ...) → Dr.
    REQUIRE(report.first_row_component == CHD_CHROMA_ROW_DR);

    // Float plane borrow works for the subsampled planes.
    const float *data = nullptr;
    ptrdiff_t stride = 0;
    REQUIRE(chd_frame_get_plane_float(frame, CHD_PLANE_CB, &data, &stride) == CHD_OK);
    REQUIRE(data == frame->floatPlane[1].data());

    chd_frame_free(frame);

    // A non-4:4:0 frame reports the accessors unsupported.
    chd_frame *plain = new chd_frame;
    plain->format = CHD_PIXEL_YUV444PS;
    plain->outputWidth = 8;
    plain->outputHeight = 4;
    REQUIRE(chd_frame_chroma_row_component(plain, 0, &comp) == CHD_E_UNSUPPORTED);
    REQUIRE(chd_frame_get_chroma_ident(plain, &report) == CHD_E_UNSUPPORTED);
    chd_frame_free(plain);
    return 0;
}

// Accessor behavior on a padded 4:4:0 frame, assembled the way
// decodeFrameLocked does: border rows carry no chroma component.
int testFrameAccessorsPadded() {
    auto vp = testParams();
    const auto cf = makeFrame(vp);

    chd::output::OutputWriter writer;
    chd::output::OutputWriter::Configuration cfg;
    cfg.paddingAmount = 5;
    cfg.pixelFormat = chd::output::OutputWriter::YUV440P16;
    writer.updateConfiguration(vp, cfg);
    const int32_t topPad = writer.getTopPadLines();
    const int32_t activeHeight = writer.getActiveHeight();

    chd_frame *frame = new chd_frame;
    frame->format = CHD_PIXEL_YUV440PS;
    frame->chroma440 = writer.convertToFloat440(cf, frame->floatPlane);
    frame->outputWidth = writer.getOutputWidth();
    frame->outputHeight = writer.getOutputHeight();
    frame->rowComponent.assign(frame->outputHeight, -1);
    for (int32_t y = 0; y < activeHeight; y++) {
        frame->rowComponent[topPad + y] =
            cf.chromaRowComponents[vp.firstActiveFrameLine + y];
    }
    frame->info.format = frame->format;
    frame->info.width = frame->outputWidth;
    frame->info.height = frame->outputHeight;
    frame->info.num_planes = 3;
    frame->info.frame_index = 0;

    chd_plane_info_t pi{};
    REQUIRE(chd_frame_get_plane_info(frame, CHD_PLANE_Y, &pi) == CHD_OK);
    REQUIRE(pi.width == frame->outputWidth);
    REQUIRE(pi.height == frame->outputHeight);
    REQUIRE(chd_frame_get_plane_info(frame, CHD_PLANE_CB, &pi) == CHD_OK);
    REQUIRE(pi.height == frame->chroma440.cbHeight);
    REQUIRE(pi.first_frame_row == frame->chroma440.cbFirstRow);
    REQUIRE(pi.first_frame_row >= topPad);

    // Border rows report no decoded chroma; active rows keep the lattice.
    chd_chroma_row_component_t comp;
    REQUIRE(chd_frame_chroma_row_component(frame, 0, &comp) == CHD_E_OUT_OF_RANGE);
    REQUIRE(chd_frame_chroma_row_component(frame, frame->outputHeight - 1, &comp)
            == CHD_E_OUT_OF_RANGE);
    for (int32_t y = 0; y < activeHeight; y++) {
        REQUIRE(chd_frame_chroma_row_component(frame, topPad + y, &comp) == CHD_OK);
        const auto expected = pairLatticeComponent(vp.firstActiveFrameLine + y) == 0
                                  ? CHD_CHROMA_ROW_DB
                                  : CHD_CHROMA_ROW_DR;
        REQUIRE(comp == expected);
    }
    chd_frame_free(frame);
    return 0;
}

// ── Commit-time gating through the public ABI ────────────────────────────────

const char *kPalJsonSidecar = R"({
  "videoParameters": {
    "activeVideoEnd": 1107,
    "activeVideoStart": 185,
    "black16bIre": 16384,
    "colourBurstEnd": 138,
    "colourBurstStart": 98,
    "fieldHeight": 313,
    "fieldWidth": 1135,
    "isMapped": false,
    "isSubcarrierLocked": false,
    "isWidescreen": false,
    "numberOfSequentialFields": 4,
    "sampleRate": 17734475,
    "system": "PAL",
    "white16bIre": 54016
  },
  "fields": [
    {"seqNo": 1, "isFirstField": true,  "syncConf": 100, "medianBurstIRE": 25.0},
    {"seqNo": 2, "isFirstField": false, "syncConf": 100, "medianBurstIRE": 25.0},
    {"seqNo": 3, "isFirstField": true,  "syncConf": 100, "medianBurstIRE": 25.0},
    {"seqNo": 4, "isFirstField": false, "syncConf": 100, "medianBurstIRE": 25.0}
  ]
})";

bool writeFakeTbc(const std::string &path, int32_t fieldWidth, int32_t fieldHeight,
                  int32_t numFields) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    const std::vector<uint16_t> buffer(fieldWidth * fieldHeight, 0);
    for (int32_t i = 0; i < numFields; i++) {
        f.write(reinterpret_cast<const char *>(buffer.data()), buffer.size() * 2);
    }
    return f.good();
}

int commitWithFormat(chd_video_t *video, const char *format, int32_t padding,
                     chd_status_t *statusOut, int32_t lastActiveLine = -1) {
    chd_decoder_t *dec = nullptr;
    REQUIRE(chd_decoder_create(video, CHD_DEC_NONE, &dec) == CHD_OK);
    REQUIRE(chd_decoder_set_option_str(dec, CHD_OPT_OUTPUT_FORMAT, format) == CHD_OK);
    if (padding > 1) {
        REQUIRE(chd_decoder_set_option_i32(dec, CHD_OPT_PADDING_MULTIPLE, padding) == CHD_OK);
    }
    if (lastActiveLine >= 0) {
        REQUIRE(chd_decoder_set_option_i32(dec, CHD_OPT_LAST_ACTIVE_FRAME_LINE,
                                           lastActiveLine) == CHD_OK);
    }
    *statusOut = chd_decoder_commit(dec);
    chd_decoder_free(dec);
    return 0;
}

int testCommitGating(const fs::path &dir) {
    const std::string tbc     = (dir / "gate.tbc").string();
    const std::string sidecar = (dir / "gate.tbc.json").string();
    REQUIRE(writeFakeTbc(tbc, 1135, 313, 4));
    {
        std::ofstream f(sidecar);
        REQUIRE(f.good());
        f << kPalJsonSidecar;
    }

    chd_video_params_t params{};
    params.standard = CHD_STD_SECAM;
    chd_video_t *secam = nullptr;
    REQUIRE(chd_video_open_composite(tbc.c_str(), nullptr, &params, &secam) == CHD_OK);

    chd_status_t st = CHD_OK;
    // Full-height chroma and RGB are rejected for SECAM sources.
    REQUIRE(commitWithFormat(secam, "yuv444ps", 1, &st) == 0);
    REQUIRE(st != CHD_OK);
    REQUIRE(commitWithFormat(secam, "yuv444p16", 1, &st) == 0);
    REQUIRE(st != CHD_OK);
    REQUIRE(commitWithFormat(secam, "rgbs", 1, &st) == 0);
    REQUIRE(st != CHD_OK);
    REQUIRE(commitWithFormat(secam, "rgb48", 1, &st) == 0);
    REQUIRE(st != CHD_OK);
    // 4:4:0 and luma-only commit.
    REQUIRE(commitWithFormat(secam, "yuv440ps", 1, &st) == 0);
    REQUIRE(st == CHD_OK);
    REQUIRE(commitWithFormat(secam, "yuv440p16", 1, &st) == 0);
    REQUIRE(st == CHD_OK);
    REQUIRE(commitWithFormat(secam, "grays", 1, &st) == 0);
    REQUIRE(st == CHD_OK);
    // Padding applies to 4:4:0 like any other format.
    REQUIRE(commitWithFormat(secam, "yuv440ps", 8, &st) == 0);
    REQUIRE(st == CHD_OK);
    // The woven chroma planes tile only over whole two-line pairs of each
    // field: active line crops off a multiple of 4 are rejected for 4:4:0
    // (default crop is 44..619, so last = 617 gives 574 lines and last = 618
    // gives 575), while luma-only output takes any crop.
    REQUIRE(commitWithFormat(secam, "yuv440ps", 1, &st, 617) == 0);
    REQUIRE(st != CHD_OK);
    REQUIRE(commitWithFormat(secam, "yuv440ps", 1, &st, 618) == 0);
    REQUIRE(st != CHD_OK);
    REQUIRE(commitWithFormat(secam, "yuv440ps", 1, &st, 615) == 0);
    REQUIRE(st == CHD_OK);
    REQUIRE(commitWithFormat(secam, "grays", 1, &st, 617) == 0);
    REQUIRE(st == CHD_OK);
    chd_video_free(secam);

    // 4:4:0 is rejected off-SECAM (the sidecar's PAL declaration stands).
    chd_video_t *pal = nullptr;
    REQUIRE(chd_video_open_composite(tbc.c_str(), nullptr, nullptr, &pal) == CHD_OK);
    REQUIRE(commitWithFormat(pal, "yuv440ps", 1, &st) == 0);
    REQUIRE(st != CHD_OK);
    chd_video_free(pal);

    fs::remove(tbc);
    fs::remove(sidecar);
    return 0;
}

}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "chd_secam_output_abi_test";
    fs::create_directories(dir);

    int rc = testWriter440();
    if (rc == 0) rc = testWriter440Padded();
    if (rc == 0) rc = testWriter440OddTopPad();
    if (rc == 0) rc = testFrameAccessors();
    if (rc == 0) rc = testFrameAccessorsPadded();
    if (rc == 0) rc = testCommitGating(dir);

    fs::remove_all(dir);
    if (rc == 0) std::cout << "PASS\n";
    return rc;
}
