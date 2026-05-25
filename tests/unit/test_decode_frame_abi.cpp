// SPDX-License-Identifier: GPL-3.0-or-later
//
// End-to-end test: drives the public C ABI through a full decode
// loop. Reuses the synthetic-TBC fixture pattern from
// test_encode_orc_color_bars (black frames + a minimal sqlite sidecar), and
// then exercises:
//   - chd_decoder_set_option_* + chd_decoder_has_option (validity by kind)
//   - chd_decoder_commit on a CHD_DEC_MONO decoder
//   - chd_decode_frame returning a YUV444P16 chd_frame_t
//   - chd_frame_get_info + chd_frame_get_plane (Y plane)
//   - chd_decode_frame out-of-range and not-committed error paths
//   - chd_decode_frames_async + chd_cancel_t cancel-before-dispatch
//
// Hardening additions:
//   - chd_video_add_extra_source_composite against a CVBS primary (previously
//     rejected with "primary source has no TBC metadata")
//   - Multi-source dropout path through chd_decode_frame with both primary
//     and extra TBC sources, verifying chd_decoder_get_last_dropout_stats
//
// Black input means the Y plane should sit at the YUV444P16 black point
// (Y_ZERO = 16 * 256 = 4096) and the Cb/Cr planes at C_ZERO = 128 * 256
// = 32768. We assert that to catch any future regression in the
// SourceField → Decoder → OutputWriter → chd_frame plumbing.

#include <chromadec/chromadec.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
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

// One drop_outs row to inject into the synthetic sidecar. field_id is
// 0-based to match the sqlite-stored field index from the writeSidecar
// loop. (capture_id == 1 implicitly.)
struct DropoutRow {
    int32_t fieldId;
    int32_t startx;
    int32_t endx;
    int32_t fieldLine;
};

bool writeTbcSidecar(const std::string &path, int32_t numFields,
                     const std::vector<DropoutRow> &dropouts = {}) {
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
        "VALUES (1, 'NTSC', 'ld-decode', 14318181.818, 192, 1791, 910, 263, ?, "
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
    sqlite3_stmt *fs = nullptr;
    sqlite3_prepare_v2(db, fieldSql, -1, &fs, nullptr);
    for (int32_t i = 0; i < numFields; i++) {
        sqlite3_reset(fs);
        sqlite3_bind_int(fs, 1, i);
        sqlite3_bind_int(fs, 2, (i % 2 == 0) ? 1 : 0);
        if (sqlite3_step(fs) != SQLITE_DONE) {
            sqlite3_finalize(fs);
            sqlite3_close(db);
            return false;
        }
    }
    sqlite3_finalize(fs);

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
    const uint16_t black = 17920;  // blackIre from the sidecar
    const std::vector<uint16_t> buf(static_cast<size_t>(w) * h, black);
    for (int32_t i = 0; i < n; i++) {
        f.write(reinterpret_cast<const char *>(buf.data()), buf.size() * 2);
    }
    return true;
}

// Minimal CVBS .meta sidecar — one capture row in the cvbs_file table.
// Mirrors the helper in test_cvbs_reader.cpp.
bool writeCvbsMeta(const std::string &path, const std::string &preset,
                   const std::string &encoding, const std::string &state,
                   const std::string &signalType) {
    if (fs::exists(path)) fs::remove(path);
    sqlite3 *db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) return false;
    const char *schema = R"(
        PRAGMA user_version = 7;
        CREATE TABLE cvbs_file (
            cvbs_file_id INTEGER PRIMARY KEY,
            preset TEXT NOT NULL,
            sample_encoding_preset TEXT NOT NULL,
            signal_state_preset TEXT NOT NULL,
            signal_type TEXT NOT NULL,
            decoder TEXT NOT NULL,
            git_branch TEXT,
            git_commit TEXT,
            number_of_sequential_frames INTEGER,
            black_level INTEGER,
            has_nonstandard_values BOOLEAN,
            capture_notes TEXT
        );
    )";
    if (sqlite3_exec(db, schema, nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
    std::string sql =
        "INSERT INTO cvbs_file (cvbs_file_id, preset, sample_encoding_preset, "
        "signal_state_preset, signal_type, decoder) VALUES (1, '" + preset + "', '" +
        encoding + "', '" + state + "', '" + signalType + "', 'cvbs-encode')";
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
    sqlite3_close(db);
    return true;
}

bool writeUniformCvbs(const std::string &path, size_t numSamples, int16_t value) {
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;
    const std::vector<int16_t> buf(numSamples, value);
    out.write(reinterpret_cast<const char *>(buf.data()),
              static_cast<std::streamsize>(buf.size() * sizeof(int16_t)));
    return out.good();
}

// ─── Test 1: original ABI decode loop on a black NTSC TBC fixture. ─────────
int testSyncDecodeBlackMono(const fs::path &dir) {
    const std::string tbc     = (dir / "blk.tbc").string();
    const std::string sidecar = (dir / "blk.tbc.db").string();
    constexpr int32_t fieldWidth  = 910;
    constexpr int32_t fieldHeight = 263;
    constexpr int32_t numFields   = 6;

    REQUIRE(writeBlackTbc(tbc, fieldWidth, fieldHeight, numFields));
    REQUIRE(writeTbcSidecar(sidecar, numFields));

    chd_video_t *video = nullptr;
    REQUIRE(chd_video_open_composite(tbc.c_str(), sidecar.c_str(), &video) == CHD_OK);

    chd_video_info_t info;
    REQUIRE(chd_video_get_info(video, &info) == CHD_OK);
    REQUIRE(info.num_frames == 3);

    chd_decoder_t *dec = nullptr;
    REQUIRE(chd_decoder_create(video, CHD_DEC_MONO, &dec) == CHD_OK);

    REQUIRE(chd_decoder_has_option(dec, CHD_OPT_LUMA_NR_LEVEL) == CHD_OK);
    REQUIRE(chd_decoder_has_option(dec, CHD_OPT_COMB_DIMENSIONS) == CHD_E_INVALID_ARG);
    REQUIRE(chd_decoder_set_option_f64(dec, CHD_OPT_LUMA_NR_LEVEL, 0.0) == CHD_OK);
    REQUIRE(chd_decoder_set_option_i32(dec, CHD_OPT_COMB_DIMENSIONS, 2) == CHD_E_INVALID_ARG);
    REQUIRE(chd_decoder_set_option_i32(dec, CHD_OPT_PADDING_MULTIPLE, 1) == CHD_OK);

    chd_frame_t *bad = nullptr;
    REQUIRE(chd_decode_frame(dec, 0, &bad) == CHD_E_INVALID_ARG);
    REQUIRE(bad == nullptr);

    REQUIRE(chd_decoder_commit(dec) == CHD_OK);
    REQUIRE(chd_decoder_set_option_f64(dec, CHD_OPT_LUMA_NR_LEVEL, 0.5) == CHD_E_INVALID_ARG);

    chd_frame_t *frame = nullptr;
    REQUIRE(chd_decode_frame(dec, 0, &frame) == CHD_OK);
    REQUIRE(frame != nullptr);

    chd_frame_info_t finfo;
    REQUIRE(chd_frame_get_info(frame, &finfo) == CHD_OK);
    REQUIRE(finfo.format == CHD_PIXEL_YUV444P16);
    REQUIRE(finfo.frame_index == 0);
    REQUIRE(finfo.num_planes == 3);

    const void *yData = nullptr;
    ptrdiff_t yStride = 0;
    REQUIRE(chd_frame_get_plane(frame, CHD_PLANE_Y, &yData, &yStride) == CHD_OK);
    const uint16_t *yRow = static_cast<const uint16_t *>(yData);
    REQUIRE(yRow[0] == 4096);
    REQUIRE(yRow[finfo.width / 2] == 4096);

    const void *cbData = nullptr;
    ptrdiff_t cbStride = 0;
    REQUIRE(chd_frame_get_plane(frame, CHD_PLANE_CB, &cbData, &cbStride) == CHD_OK);
    REQUIRE(static_cast<const uint16_t *>(cbData)[0] == 32768);

    chd_frame_free(frame);

    chd_frame_t *oob = nullptr;
    REQUIRE(chd_decode_frame(dec, 99, &oob) == CHD_E_OUT_OF_RANGE);
    REQUIRE(oob == nullptr);

    // Async + cancel
    struct AsyncSink {
        std::mutex                   mu;
        std::vector<chd_status_t>    statuses;
        std::vector<chd_frame_t *>   frames;
    } sink;
    auto cb = +[](void *user, chd_status_t s, int64_t /*idx*/, chd_frame_t *f) {
        auto *sk = static_cast<AsyncSink *>(user);
        std::lock_guard<std::mutex> lock(sk->mu);
        sk->statuses.push_back(s);
        sk->frames.push_back(f);
    };
    const int64_t idxs[2] = {0, 1};
    REQUIRE(chd_decode_frames_async(dec, idxs, 2, cb, &sink, nullptr) == CHD_OK);
    REQUIRE(sink.statuses.size() == 2);
    for (size_t i = 0; i < 2; i++) {
        REQUIRE(sink.statuses[i] == CHD_OK);
        REQUIRE(sink.frames[i] != nullptr);
        chd_frame_free(sink.frames[i]);
    }
    sink.statuses.clear();
    sink.frames.clear();

    chd_cancel_t *cancel = nullptr;
    REQUIRE(chd_cancel_create(&cancel) == CHD_OK);
    chd_cancel_request(cancel);
    REQUIRE(chd_cancel_is_requested(cancel) == 1);
    REQUIRE(chd_decode_frames_async(dec, idxs, 2, cb, &sink, cancel) == CHD_OK);
    REQUIRE(sink.statuses.size() == 2);
    for (size_t i = 0; i < 2; i++) {
        REQUIRE(sink.statuses[i] == CHD_E_CANCELLED);
        REQUIRE(sink.frames[i] == nullptr);
    }
    chd_cancel_free(cancel);

    chd_decoder_free(dec);
    chd_video_free(video);

    fs::remove(tbc);
    fs::remove(sidecar);
    return 0;
}

// ─── Test 2 (hardening): CVBS primary + TBC extra source. ──────────────────
//
// Previously, chd_video_add_extra_source_composite rejected this
// case with "primary source has no TBC metadata" because CVBS opens left
// v->metadata == nullptr. Hardening synthesizes metadata at open time and
// drops the check.
int testCvbsPrimaryWithTbcExtra(const fs::path &dir) {
    const std::string composite  = (dir / "cvbs.composite").string();
    const std::string compositeM = (dir / "cvbs.meta").string();
    const std::string extraTbc   = (dir / "extra.tbc").string();
    const std::string extraDb    = (dir / "extra.tbc.db").string();

    constexpr int32_t fieldWidth  = 910;
    constexpr int32_t fieldHeight = 263;
    constexpr int32_t numFields   = 4;

    // CVBS primary: 4 NTSC fields, uniform blanking at 256 (CVBS_U10_4FSC
    // ⇒ × 64 ⇒ 16384 which matches the TBC blanking16bIre default).
    REQUIRE(writeUniformCvbs(composite,
                             static_cast<size_t>(fieldWidth) * fieldHeight * numFields,
                             256));
    REQUIRE(writeCvbsMeta(compositeM, "NTSC", "CVBS_U10_4FSC",
                          "STANDARD_TBC_LOCKED", "composite"));

    // TBC extra
    REQUIRE(writeBlackTbc(extraTbc, fieldWidth, fieldHeight, numFields));
    REQUIRE(writeTbcSidecar(extraDb, numFields));

    chd_video_t *video = nullptr;
    REQUIRE(chd_video_open_composite(composite.c_str(), compositeM.c_str(),
                                          nullptr, &video) == CHD_OK);

    chd_video_info_t info{};
    REQUIRE(chd_video_get_info(video, &info) == CHD_OK);
    // The CVBS discriminator path — should report the actual encoding, not
    // CHD_ENC_CVBS_U16_4FSC (which was the pre-hardening behaviour for any
    // video that had metadata).
    REQUIRE(info.encoding == CHD_ENC_CVBS_U10_4FSC);
    REQUIRE(info.num_frames == 2);  // 4 fields / 2

    // This used to fail with "primary source has no TBC metadata".
    REQUIRE(chd_video_add_extra_source_composite(video, extraTbc.c_str()) == CHD_OK);

    chd_video_free(video);

    fs::remove(composite);
    fs::remove(compositeM);
    fs::remove(extraTbc);
    fs::remove(extraDb);
    return 0;
}

// ─── Test 3 (hardening): multi-source dropout actually fires. ──────────────
//
// Inject a drop_outs row on a specific line of the primary's first field.
// Add an extra TBC source. Decode with chd_dropout_opts.enabled=1. Expect
// chd_decoder_get_last_dropout_stats to show corrected ≥ 1.
int testMultiSourceDropoutAbi(const fs::path &dir) {
    const std::string primTbc = (dir / "prim.tbc").string();
    const std::string primDb  = (dir / "prim.tbc.db").string();
    const std::string exTbc   = (dir / "ex.tbc").string();
    const std::string exDb    = (dir / "ex.tbc.db").string();

    constexpr int32_t fieldWidth  = 910;
    constexpr int32_t fieldHeight = 263;
    constexpr int32_t numFields   = 4;

    REQUIRE(writeBlackTbc(primTbc, fieldWidth, fieldHeight, numFields));
    REQUIRE(writeBlackTbc(exTbc,   fieldWidth, fieldHeight, numFields));
    REQUIRE(writeTbcSidecar(exDb, numFields));

    // Drop on first field (field_id = 0 in the 0-based seqNo column), line
    // 100, samples [200, 280). Inside the active region [192, 1791).
    const DropoutRow drop{/*fieldId=*/0, /*startx=*/200, /*endx=*/280, /*fieldLine=*/100};
    REQUIRE(writeTbcSidecar(primDb, numFields, {drop}));

    chd_video_t *video = nullptr;
    REQUIRE(chd_video_open_composite(primTbc.c_str(), primDb.c_str(), &video) == CHD_OK);
    REQUIRE(chd_video_add_extra_source_composite(video, exTbc.c_str()) == CHD_OK);

    chd_decoder_t *dec = nullptr;
    REQUIRE(chd_decoder_create(video, CHD_DEC_MONO, &dec) == CHD_OK);
    REQUIRE(chd_decoder_set_option_i32(dec, CHD_OPT_PADDING_MULTIPLE, 1) == CHD_OK);

    chd_dropout_opts_t dops{};
    dops.enabled = 1;
    dops.overcorrect = 0;
    dops.intra_field_only = 0;
    REQUIRE(chd_decoder_set_dropout(dec, &dops) == CHD_OK);

    REQUIRE(chd_decoder_commit(dec) == CHD_OK);

    chd_frame_t *frame = nullptr;
    REQUIRE(chd_decode_frame(dec, 0, &frame) == CHD_OK);
    REQUIRE(frame != nullptr);

    chd_dropout_stats_t stats{};
    REQUIRE(chd_decoder_get_last_dropout_stats(dec, &stats) == CHD_OK);
    // One drop_outs row → at least one corrected dropout region. failed
    // should be zero since the extra has the same line cleanly.
    REQUIRE(stats.corrected >= 1);
    REQUIRE(stats.failed == 0);

    chd_frame_free(frame);
    chd_decoder_free(dec);
    chd_video_free(video);

    fs::remove(primTbc);
    fs::remove(primDb);
    fs::remove(exTbc);
    fs::remove(exDb);
    return 0;
}

// ─── Test 4 (hardening follow-up): parallel async dispatch. ────────────────
//
// thread_count=4, 12 indices. Each worker owns its own decoder instance,
// pulls next-index from an atomic counter, fires the callback with its
// own frame. We verify all 12 callbacks land with CHD_OK and a valid Y
// plane (black point Y_ZERO=4096). The result set must cover every
// requested index exactly once — proves the atomic counter doesn't
// double-dispatch and the workers actually drain the queue.
int testParallelAsyncDispatch(const fs::path &dir) {
    const std::string tbc     = (dir / "par.tbc").string();
    const std::string sidecar = (dir / "par.tbc.db").string();
    constexpr int32_t fieldWidth  = 910;
    constexpr int32_t fieldHeight = 263;
    constexpr int32_t numFrames   = 16;
    constexpr int32_t numFields   = numFrames * 2;

    REQUIRE(writeBlackTbc(tbc, fieldWidth, fieldHeight, numFields));
    REQUIRE(writeTbcSidecar(sidecar, numFields));

    chd_video_t *video = nullptr;
    REQUIRE(chd_video_open_composite(tbc.c_str(), sidecar.c_str(), &video) == CHD_OK);

    chd_decoder_t *dec = nullptr;
    REQUIRE(chd_decoder_create(video, CHD_DEC_MONO, &dec) == CHD_OK);
    REQUIRE(chd_decoder_set_option_i32(dec, CHD_OPT_PADDING_MULTIPLE, 1) == CHD_OK);
    // Force a concrete worker count so the test exercises the
    // multi-worker dispatch (thread_count=0 would also work but its
    // effective W depends on std::thread::hardware_concurrency()).
    REQUIRE(chd_decoder_set_option_i32(dec, CHD_OPT_THREAD_COUNT, 4) == CHD_OK);
    REQUIRE(chd_decoder_commit(dec) == CHD_OK);

    constexpr size_t N = 12;
    std::vector<int64_t> idxs(N);
    for (size_t i = 0; i < N; i++) idxs[i] = static_cast<int64_t>(i);

    struct Sink {
        std::mutex                 mu;
        std::vector<int64_t>       returned;
        std::vector<chd_status_t>  statuses;
        std::vector<chd_frame_t *> frames;
    } sink;

    auto cb = +[](void *user, chd_status_t s, int64_t idx, chd_frame_t *f) {
        auto *sk = static_cast<Sink *>(user);
        std::lock_guard<std::mutex> lock(sk->mu);
        sk->returned.push_back(idx);
        sk->statuses.push_back(s);
        sk->frames.push_back(f);
    };

    REQUIRE(chd_decode_frames_async(dec, idxs.data(), N, cb, &sink, nullptr) == CHD_OK);

    REQUIRE(sink.returned.size() == N);
    // Build a presence vector keyed by returned frame index; the order of
    // callbacks is not deterministic across W workers but the set must
    // exactly cover [0, N).
    std::vector<int> seen(N, 0);
    for (size_t i = 0; i < N; i++) {
        REQUIRE(sink.statuses[i] == CHD_OK);
        REQUIRE(sink.frames[i] != nullptr);
        const int64_t idx = sink.returned[i];
        REQUIRE(idx >= 0);
        REQUIRE(idx < static_cast<int64_t>(N));
        seen[idx]++;

        // Spot-check each frame's Y plane: black input ⇒ Y_ZERO=4096
        // across every pixel.
        chd_frame_info_t finfo{};
        REQUIRE(chd_frame_get_info(sink.frames[i], &finfo) == CHD_OK);
        REQUIRE(finfo.frame_index == idx);
        const void *yData = nullptr;
        ptrdiff_t yStride = 0;
        REQUIRE(chd_frame_get_plane(sink.frames[i], CHD_PLANE_Y, &yData, &yStride) == CHD_OK);
        const uint16_t *yRow = static_cast<const uint16_t *>(yData);
        REQUIRE(yRow[0] == 4096);
        REQUIRE(yRow[finfo.width / 2] == 4096);

        chd_frame_free(sink.frames[i]);
    }
    for (size_t i = 0; i < N; i++) REQUIRE(seen[i] == 1);

    chd_decoder_free(dec);
    chd_video_free(video);

    fs::remove(tbc);
    fs::remove(sidecar);
    return 0;
}

}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "chd_phase_g_test";
    fs::create_directories(dir);

    if (int rc = testSyncDecodeBlackMono(dir);    rc != 0) return rc;
    if (int rc = testCvbsPrimaryWithTbcExtra(dir); rc != 0) return rc;
    if (int rc = testMultiSourceDropoutAbi(dir);  rc != 0) return rc;
    if (int rc = testParallelAsyncDispatch(dir);  rc != 0) return rc;

    fs::remove(dir);
    std::cout << "test_decode_frame_abi: PASS\n";
    return 0;
}
