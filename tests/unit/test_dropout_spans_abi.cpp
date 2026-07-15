// SPDX-License-Identifier: GPL-3.0-or-later
//
// Decode-free dropout detection C ABI: CHD_DEC_NONE + chd_decoder_get_output_info
// + chd_decoder_get_dropout_spans + chd_decode_dropout_mask.
//
// Builds a synthetic NTSC TBC (black fields + a minimal sqlite sidecar, same
// shape as test_decode_frame_abi) with two injected dropouts on frame 0 — one
// on the first field, one on the second — and asserts that:
//   - a CHD_DEC_NONE decoder commits without building a chroma decoder and
//     reports the committed output framing via chd_decoder_get_output_info;
//   - chd_decode_frame / chd_decode_frames_async reject a CHD_DEC_NONE decoder;
//   - chd_decoder_get_dropout_spans maps each stored (startx, endx, fieldLine)
//     into active-output pixel coordinates (interlace weave + horizontal crop),
//     in both CHD_DROPOUT_DETECTED and CHD_DROPOUT_OVERCORRECT modes (the latter
//     widened by ±24 and clamped), and rejects an unknown mode;
//   - a frame with no dropouts yields count 0 / spans NULL;
//   - chd_decode_dropout_mask paints those spans into a single-plane frame whose
//     format follows the committed precision domain (GRAY16, or GRAYS when the
//     decoder is committed to a float output format);
//   - the NONE framing matches a real CHD_DEC_MONO decoded frame's geometry.
//
// Fixture geometry (NTSC, from the sidecar): field_width 910, field_height 263,
// active_video_start 147, active_video_end 905 ⇒ active width 758. Default NTSC
// active frame lines 40..525 ⇒ output height 485 at padding 1.
//
// Mapping under test (per field): offset = isFirstField ? 0 : 1;
//   frameRow = 2*(fieldLine-1) + offset;  y = topPadLines + frameRow - firstActiveFrameLine;
//   x = sample_x - activeVideoStart (clamped to the active region).

#include <chromadec/chromadec.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <sqlite3.h>

namespace fs = std::filesystem;

#define REQUIRE(cond)                                                            \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " << #cond \
                      << " (last_error=" << chd_last_error() << ")\n";           \
            return 1;                                                            \
        }                                                                        \
    } while (0)

namespace {

const char *kTbcSchema = R"(
PRAGMA user_version = 1;

CREATE TABLE IF NOT EXISTS capture (
    capture_id INTEGER PRIMARY KEY,
    system TEXT NOT NULL,
    decoder TEXT NOT NULL,
    git_branch TEXT,
    git_commit TEXT,
    video_sample_rate REAL,
    active_video_start INTEGER,
    active_video_end INTEGER,
    field_width INTEGER,
    field_height INTEGER,
    number_of_sequential_fields INTEGER,
    colour_burst_start INTEGER,
    colour_burst_end INTEGER,
    is_mapped INTEGER,
    is_subcarrier_locked INTEGER,
    is_widescreen INTEGER,
    white_16b_ire INTEGER,
    black_16b_ire INTEGER,
    blanking_16b_ire INTEGER,
    capture_notes TEXT
);

CREATE TABLE IF NOT EXISTS field_record (
    capture_id INTEGER NOT NULL,
    field_id INTEGER NOT NULL,
    audio_samples INTEGER,
    decode_faults INTEGER,
    disk_loc REAL,
    efm_t_values INTEGER,
    field_phase_id INTEGER,
    file_loc INTEGER,
    is_first_field INTEGER,
    median_burst_ire REAL,
    pad INTEGER,
    sync_conf INTEGER,
    ntsc_is_fm_code_data_valid INTEGER,
    ntsc_fm_code_data INTEGER,
    ntsc_field_flag INTEGER,
    ntsc_is_video_id_data_valid INTEGER,
    ntsc_video_id_data INTEGER,
    ntsc_white_flag INTEGER
);

CREATE TABLE IF NOT EXISTS drop_outs (
    capture_id INTEGER NOT NULL,
    field_id INTEGER NOT NULL,
    startx INTEGER NOT NULL,
    endx INTEGER NOT NULL,
    field_line INTEGER NOT NULL
);
)";

struct DropoutRow {
    int32_t fieldId;  // 0-based, matches the writeTbcSidecar field loop
    int32_t startx;
    int32_t endx;
    int32_t fieldLine;
};

bool writeTbcSidecar(const std::string &path, int32_t numFields,
                     const std::vector<DropoutRow> &dropouts = {}) {
    if (fs::exists(path)) fs::remove(path);
    sqlite3 *db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) return false;
    if (sqlite3_exec(db, kTbcSchema, nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
    const char *capSql =
        "INSERT INTO capture (capture_id, system, decoder, video_sample_rate, "
        "  active_video_start, active_video_end, field_width, field_height, "
        "  number_of_sequential_fields, colour_burst_start, colour_burst_end, "
        "  is_mapped, is_subcarrier_locked, is_widescreen, "
        "  white_16b_ire, black_16b_ire, blanking_16b_ire) "
        "VALUES (1, 'NTSC', 'ld-decode', 14318181.818, 147, 905, 910, 263, ?, "
        "        92, 119, 1, 1, 0, 51200, 17920, 16384);";
    sqlite3_stmt *cap = nullptr;
    sqlite3_prepare_v2(db, capSql, -1, &cap, nullptr);
    sqlite3_bind_int(cap, 1, numFields);
    sqlite3_step(cap);
    sqlite3_finalize(cap);

    const char *fieldSql =
        "INSERT INTO field_record (capture_id, field_id, is_first_field, "
        "  sync_conf, median_burst_ire, ntsc_is_fm_code_data_valid, "
        "  ntsc_fm_code_data, ntsc_field_flag, ntsc_is_video_id_data_valid, "
        "  ntsc_video_id_data, ntsc_white_flag) "
        "VALUES (1, ?, ?, 100, 25.0, 0, 0, 0, 0, 0, 0);";
    sqlite3_stmt *fst = nullptr;
    sqlite3_prepare_v2(db, fieldSql, -1, &fst, nullptr);
    for (int32_t i = 0; i < numFields; i++) {
        sqlite3_reset(fst);
        sqlite3_bind_int(fst, 1, i);
        sqlite3_bind_int(fst, 2, (i % 2 == 0) ? 1 : 0);
        if (sqlite3_step(fst) != SQLITE_DONE) {
            sqlite3_finalize(fst);
            sqlite3_close(db);
            return false;
        }
    }
    sqlite3_finalize(fst);

    if (!dropouts.empty()) {
        const char *doSql =
            "INSERT INTO drop_outs (capture_id, field_id, startx, endx, field_line) "
            "VALUES (1, ?, ?, ?, ?);";
        sqlite3_stmt *doStmt = nullptr;
        sqlite3_prepare_v2(db, doSql, -1, &doStmt, nullptr);
        for (const auto &dr : dropouts) {
            sqlite3_reset(doStmt);
            sqlite3_bind_int(doStmt, 1, dr.fieldId);
            sqlite3_bind_int(doStmt, 2, dr.startx);
            sqlite3_bind_int(doStmt, 3, dr.endx);
            sqlite3_bind_int(doStmt, 4, dr.fieldLine);
            if (sqlite3_step(doStmt) != SQLITE_DONE) {
                sqlite3_finalize(doStmt);
                sqlite3_close(db);
                return false;
            }
        }
        sqlite3_finalize(doStmt);
    }
    sqlite3_close(db);
    return true;
}

bool writeBlackTbc(const std::string &path, int32_t w, int32_t h, int32_t n) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    const uint16_t black = 17920;
    const std::vector<uint16_t> buf(static_cast<size_t>(w) * h, black);
    for (int32_t i = 0; i < n; i++) {
        f.write(reinterpret_cast<const char *>(buf.data()), buf.size() * 2);
    }
    return true;
}

// Expected output row for a field-relative dropout, mirroring the library's
// mapping: interlace weave then crop by the active-frame origin. firstFrameLine
// is read back from the API rather than hard-coded so the test tracks changes
// to the active-line convention. topPadLines == 0 at padding 1.
int32_t expectY(int32_t fieldLine, bool isFirstField, int32_t firstFrameLine) {
    const int32_t offset = isFirstField ? 0 : 1;
    const int32_t frameRow = (2 * (fieldLine - 1)) + offset;
    return frameRow - firstFrameLine;
}

int testDropoutSpansAndMask(const fs::path &dir) {
    const std::string tbc     = (dir / "do.tbc").string();
    const std::string sidecar = (dir / "do.tbc.db").string();
    constexpr int32_t fieldWidth  = 910;
    constexpr int32_t fieldHeight = 263;
    constexpr int32_t numFields   = 4;  // 2 frames

    // Dropout A: frame 0 first field (field_id 0, is_first_field=1), line 100,
    // samples [300, 400). Dropout B: frame 0 second field (field_id 1,
    // is_first_field=0), line 50, samples [147, 200) — left edge clamps to 0.
    const DropoutRow dropA{/*fieldId=*/0, /*startx=*/300, /*endx=*/400, /*fieldLine=*/100};
    const DropoutRow dropB{/*fieldId=*/1, /*startx=*/147, /*endx=*/200, /*fieldLine=*/50};

    REQUIRE(writeBlackTbc(tbc, fieldWidth, fieldHeight, numFields));
    REQUIRE(writeTbcSidecar(sidecar, numFields, {dropA, dropB}));

    chd_video_t *video = nullptr;
    REQUIRE(chd_video_open_composite(tbc.c_str(), sidecar.c_str(), nullptr, &video) == CHD_OK);

    // The active region is the source of truth for the expected mapping; read it
    // back rather than hard-coding it so the test survives changes to the
    // active-line convention (e.g. inclusive vs exclusive bounds).
    chd_video_info_t vinfo{};
    REQUIRE(chd_video_get_info(video, &vinfo) == CHD_OK);
    const int32_t activeVideoStart = vinfo.first_active_sample;
    const int32_t firstFrameLine   = vinfo.first_active_frame_line;

    chd_decoder_t *dec = nullptr;
    REQUIRE(chd_decoder_create(video, CHD_DEC_NONE, &dec) == CHD_OK);
    REQUIRE(chd_decoder_set_option_i32(dec, CHD_OPT_PADDING_MULTIPLE, 1) == CHD_OK);
    // A chroma-algorithm option must still be rejected on a NONE decoder.
    REQUIRE(chd_decoder_set_option_f64(dec, CHD_OPT_CHROMA_NR_LEVEL, 0.0) == CHD_E_INVALID_ARG);

    // Geometry queries require a committed decoder.
    chd_output_info_t early{};
    REQUIRE(chd_decoder_get_output_info(dec, &early) == CHD_E_INVALID_ARG);

    REQUIRE(chd_decoder_commit(dec) == CHD_OK);

    // ── chd_decoder_get_output_info ───────────────────────────────────────
    chd_output_info_t oi{};
    REQUIRE(chd_decoder_get_output_info(dec, &oi) == CHD_OK);
    REQUIRE(oi.format == CHD_PIXEL_YUV444P16);
    REQUIRE(oi.width > 0);
    REQUIRE(oi.height > 0);
    REQUIRE(oi.num_planes == 3);
    REQUIRE(oi.num_frames == 2);
    const int32_t activeWidth  = oi.width;
    const int32_t outputHeight = oi.height;

    // ── CHD_DEC_NONE must not decode ──────────────────────────────────────
    chd_frame_t *nope = nullptr;
    REQUIRE(chd_decode_frame(dec, 0, &nope) == CHD_E_DECODER_INCOMPATIBLE);
    REQUIRE(nope == nullptr);
    // A real callback is required to reach the kind check (null cb is rejected
    // earlier as a bad argument); the callback must never fire for a NONE kind.
    auto noopCb = +[](void *user, chd_status_t, int64_t, chd_frame_t *) {
        *static_cast<int *>(user) += 1;
    };
    int callbackHits = 0;
    const int64_t idx0[1] = {0};
    REQUIRE(chd_decode_frames_async(dec, idx0, 1, noopCb, &callbackHits, nullptr)
            == CHD_E_DECODER_INCOMPATIBLE);
    REQUIRE(callbackHits == 0);

    // ── chd_decoder_get_dropout_spans (frame 0: two dropouts) ─────────────
    const int32_t yA = expectY(100, /*isFirstField=*/true,  firstFrameLine);
    const int32_t yB = expectY(50,  /*isFirstField=*/false, firstFrameLine);
    const int32_t xA0 = 300 - activeVideoStart;
    const int32_t xA1 = 400 - activeVideoStart;
    const int32_t xB0 = 0;                       // dropB starts at activeVideoStart, clamps to 0
    const int32_t xB1 = 200 - activeVideoStart;

    chd_dropout_span_t *spans = nullptr;
    size_t count = 0;
    REQUIRE(chd_decoder_get_dropout_spans(dec, 0, CHD_DROPOUT_DETECTED, &spans, &count) == CHD_OK);
    REQUIRE(count == 2);
    REQUIRE(spans != nullptr);
    // Sorted by (y, x_start): B (y=59) precedes A (y=158).
    REQUIRE(spans[0].y == yB);
    REQUIRE(spans[0].x_start == xB0);
    REQUIRE(spans[0].x_end == xB1);
    REQUIRE(spans[1].y == yA);
    REQUIRE(spans[1].x_start == xA0);
    REQUIRE(spans[1].x_end == xA1);
    // Every span lands inside the active output framing.
    for (size_t i = 0; i < count; i++) {
        REQUIRE(spans[i].y >= 0 && spans[i].y < outputHeight);
        REQUIRE(spans[i].x_start >= 0);
        REQUIRE(spans[i].x_end > spans[i].x_start);
        REQUIRE(spans[i].x_end <= activeWidth);
    }
    chd_dropout_spans_free(spans);

    // ── CHD_DROPOUT_OVERCORRECT widens each region by ±24 (clamped) ────────
    // Same rows, x extended outward by the overcorrect margin then clamped to
    // the active region: A (300,400) -> (276,424); B starts at activeVideoStart
    // so its left edge still clamps to 0, right edge (200) -> 224.
    const int32_t kDots = 24;
    const int32_t ocA0 = (300 - kDots) - activeVideoStart;
    const int32_t ocA1 = (400 + kDots) - activeVideoStart;
    const int32_t ocB0 = 0;
    const int32_t ocB1 = (200 + kDots) - activeVideoStart;
    chd_dropout_span_t *oc = nullptr;
    size_t ocCount = 0;
    REQUIRE(chd_decoder_get_dropout_spans(dec, 0, CHD_DROPOUT_OVERCORRECT, &oc, &ocCount) == CHD_OK);
    REQUIRE(ocCount == 2);
    REQUIRE(oc[0].y == yB);
    REQUIRE(oc[0].x_start == ocB0);
    REQUIRE(oc[0].x_end == ocB1);
    REQUIRE(oc[1].y == yA);
    REQUIRE(oc[1].x_start == ocA0);
    REQUIRE(oc[1].x_end == ocA1);
    chd_dropout_spans_free(oc);

    // Unknown mode is rejected.
    chd_dropout_span_t *bad = nullptr;
    size_t badCount = 0;
    REQUIRE(chd_decoder_get_dropout_spans(dec, 0, static_cast<chd_dropout_detect_mode_t>(99),
                                          &bad, &badCount) == CHD_E_INVALID_ARG);
    REQUIRE(bad == nullptr);

    // ── chd_decoder_get_dropout_spans (frame 1: clean) ────────────────────
    chd_dropout_span_t *none = reinterpret_cast<chd_dropout_span_t *>(0x1);
    size_t noneCount = 99;
    REQUIRE(chd_decoder_get_dropout_spans(dec, 1, CHD_DROPOUT_DETECTED, &none, &noneCount) == CHD_OK);
    REQUIRE(noneCount == 0);
    REQUIRE(none == nullptr);
    chd_dropout_spans_free(none);  // free(nullptr) is a no-op

    // Out-of-range frame index.
    chd_dropout_span_t *oob = nullptr;
    size_t oobCount = 0;
    REQUIRE(chd_decoder_get_dropout_spans(dec, 99, CHD_DROPOUT_DETECTED, &oob, &oobCount) == CHD_E_OUT_OF_RANGE);

    // ── chd_decode_dropout_mask (frame 0) ─────────────────────────────────
    chd_frame_t *mask = nullptr;
    REQUIRE(chd_decode_dropout_mask(dec, 0, CHD_DROPOUT_DETECTED, &mask) == CHD_OK);
    REQUIRE(mask != nullptr);

    chd_frame_info_t mi{};
    REQUIRE(chd_frame_get_info(mask, &mi) == CHD_OK);
    REQUIRE(mi.format == CHD_PIXEL_GRAY16);
    REQUIRE(mi.width == activeWidth);
    REQUIRE(mi.height == outputHeight);
    REQUIRE(mi.num_planes == 1);
    REQUIRE(mi.frame_index == 0);

    const void *mData = nullptr;
    ptrdiff_t mStride = 0;
    REQUIRE(chd_frame_get_plane(mask, CHD_PLANE_Y, &mData, &mStride) == CHD_OK);
    REQUIRE(mStride == static_cast<ptrdiff_t>(activeWidth) * 2);
    const uint16_t *m = static_cast<const uint16_t *>(mData);
    const auto at = [&](int32_t y, int32_t x) { return m[static_cast<size_t>(y) * activeWidth + x]; };

    // Span A interior set, just past its end clear.
    REQUIRE(at(yA, xA0) == 0xFFFF);
    REQUIRE(at(yA, xA1 - 1) == 0xFFFF);
    REQUIRE(at(yA, xA1) == 0);
    REQUIRE(at(yA, xA0 - 1) == 0);
    // Span B interior set (incl. left edge), just past its end clear.
    REQUIRE(at(yB, xB0) == 0xFFFF);
    REQUIRE(at(yB, xB1 - 1) == 0xFFFF);
    REQUIRE(at(yB, xB1) == 0);
    // A clean row is entirely zero.
    REQUIRE(at(0, 0) == 0);
    REQUIRE(at(0, activeWidth / 2) == 0);

    chd_frame_free(mask);

    // Overcorrect mask paints the widened footprint: the sample just left of A's
    // detected start (clear above) is now set, and it clears just past the
    // widened start.
    chd_frame_t *ocMask = nullptr;
    REQUIRE(chd_decode_dropout_mask(dec, 0, CHD_DROPOUT_OVERCORRECT, &ocMask) == CHD_OK);
    const void *ocData = nullptr;
    ptrdiff_t ocStride = 0;
    REQUIRE(chd_frame_get_plane(ocMask, CHD_PLANE_Y, &ocData, &ocStride) == CHD_OK);
    const uint16_t *om = static_cast<const uint16_t *>(ocData);
    REQUIRE(om[static_cast<size_t>(yA) * activeWidth + (xA0 - 1)] == 0xFFFF);
    REQUIRE(om[static_cast<size_t>(yA) * activeWidth + ocA0] == 0xFFFF);
    REQUIRE(om[static_cast<size_t>(yA) * activeWidth + (ocA0 - 1)] == 0);
    chd_frame_free(ocMask);

    // ── Geometry agreement with a real decode (CHD_DEC_MONO) ──────────────
    chd_decoder_t *mono = nullptr;
    REQUIRE(chd_decoder_create(video, CHD_DEC_MONO, &mono) == CHD_OK);
    REQUIRE(chd_decoder_set_option_i32(mono, CHD_OPT_PADDING_MULTIPLE, 1) == CHD_OK);
    REQUIRE(chd_decoder_commit(mono) == CHD_OK);
    chd_frame_t *mf = nullptr;
    REQUIRE(chd_decode_frame(mono, 0, &mf) == CHD_OK);
    chd_frame_info_t mfi{};
    REQUIRE(chd_frame_get_info(mf, &mfi) == CHD_OK);
    REQUIRE(mfi.width == oi.width);
    REQUIRE(mfi.height == oi.height);
    chd_frame_free(mf);
    chd_decoder_free(mono);

    // ── Mask follows the committed precision domain (GRAYS) ───────────────
    // A float-committed decoder yields a GRAYS mask (1.0 dropped / 0.0 clean)
    // instead of GRAY16, at the same geometry and span coordinates.
    chd_decoder_t *fdec = nullptr;
    REQUIRE(chd_decoder_create(video, CHD_DEC_NONE, &fdec) == CHD_OK);
    REQUIRE(chd_decoder_set_option_i32(fdec, CHD_OPT_PADDING_MULTIPLE, 1) == CHD_OK);
    REQUIRE(chd_decoder_set_option_str(fdec, CHD_OPT_OUTPUT_FORMAT, "grays") == CHD_OK);
    REQUIRE(chd_decoder_commit(fdec) == CHD_OK);

    chd_frame_t *fmask = nullptr;
    REQUIRE(chd_decode_dropout_mask(fdec, 0, CHD_DROPOUT_DETECTED, &fmask) == CHD_OK);
    chd_frame_info_t fmi{};
    REQUIRE(chd_frame_get_info(fmask, &fmi) == CHD_OK);
    REQUIRE(fmi.format == CHD_PIXEL_GRAYS);
    REQUIRE(fmi.width == activeWidth);
    REQUIRE(fmi.height == outputHeight);
    REQUIRE(fmi.num_planes == 1);

    const float *fData = nullptr;
    ptrdiff_t fStride = 0;
    REQUIRE(chd_frame_get_plane_float(fmask, CHD_PLANE_Y, &fData, &fStride) == CHD_OK);
    REQUIRE(fStride == static_cast<ptrdiff_t>(activeWidth) * 4);
    const auto fAt = [&](int32_t y, int32_t x) { return fData[static_cast<size_t>(y) * activeWidth + x]; };
    REQUIRE(fAt(yA, xA0) == 1.0f);
    REQUIRE(fAt(yA, xA1 - 1) == 1.0f);
    REQUIRE(fAt(yA, xA1) == 0.0f);
    REQUIRE(fAt(yB, xB0) == 1.0f);
    REQUIRE(fAt(yB, xB1) == 0.0f);
    REQUIRE(fAt(0, 0) == 0.0f);
    chd_frame_free(fmask);
    chd_decoder_free(fdec);

    chd_decoder_free(dec);
    chd_video_free(video);

    fs::remove(tbc);
    fs::remove(sidecar);
    return 0;
}

}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "chd_dropout_spans_test";
    fs::create_directories(dir);

    if (int rc = testDropoutSpansAndMask(dir); rc != 0) return rc;

    fs::remove(dir);
    std::cout << "test_dropout_spans_abi: PASS\n";
    return 0;
}
