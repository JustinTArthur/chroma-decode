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
    vp.firstActiveFieldLine = 1;
    vp.lastActiveFieldLine = 9;
    vp.isValid = true;
    return vp;
}

// The SECAM interlaced-frame component lattice: within each field successive
// lines alternate Db/Dr, and field 2 sits an odd line count after field 1,
// so components pair up in frame-row order (Db, Dr, Dr, Db, Db, ...).
int8_t pairLatticeComponent(int32_t frameRow) {
    return static_cast<int8_t>(((frameRow + 1) >> 1) & 1);
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

    // Expected per-plane row lists over active frame rows 2..17.
    std::vector<int32_t> cbRows, crRows;
    for (int32_t y = 0; y < activeHeight; y++) {
        if (pairLatticeComponent(vp.firstActiveFrameLine + y) == 0) cbRows.push_back(y);
        else crRows.push_back(y);
    }

    chd::output::OutputFrame out;
    const auto g = writer.convert440(cf, out);
    REQUIRE(g.cbHeight == static_cast<int32_t>(cbRows.size()));
    REQUIRE(g.crHeight == static_cast<int32_t>(crRows.size()));
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

    // Heights differ by at most one and cover every active row.
    REQUIRE(std::abs(frame->chroma440.cbHeight - frame->chroma440.crHeight) <= 1);
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
                     chd_status_t *statusOut) {
    chd_decoder_t *dec = nullptr;
    REQUIRE(chd_decoder_create(video, CHD_DEC_NONE, &dec) == CHD_OK);
    REQUIRE(chd_decoder_set_option_str(dec, CHD_OPT_OUTPUT_FORMAT, format) == CHD_OK);
    if (padding > 1) {
        REQUIRE(chd_decoder_set_option_i32(dec, CHD_OPT_PADDING_MULTIPLE, padding) == CHD_OK);
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
    // Padding has no 4:4:0 chroma definition.
    REQUIRE(commitWithFormat(secam, "yuv440ps", 8, &st) == 0);
    REQUIRE(st != CHD_OK);
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
    if (rc == 0) rc = testFrameAccessors();
    if (rc == 0) rc = testCommitGating(dir);

    fs::remove_all(dir);
    if (rc == 0) std::cout << "PASS\n";
    return rc;
}
