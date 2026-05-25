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

const char *kSchema = R"(
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
)";

bool writeSidecar(const std::string &path, int32_t numFields) {
    sqlite3 *db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) return false;
    if (sqlite3_exec(db, kSchema, nullptr, nullptr, nullptr) != SQLITE_OK) {
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

}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "chd_phase_g_test";
    fs::create_directories(dir);

    const std::string tbc     = (dir / "blk.tbc").string();
    const std::string sidecar = (dir / "blk.tbc.db").string();
    constexpr int32_t fieldWidth  = 910;
    constexpr int32_t fieldHeight = 263;
    constexpr int32_t numFields   = 6;  // three frames

    REQUIRE(writeBlackTbc(tbc, fieldWidth, fieldHeight, numFields));
    REQUIRE(writeSidecar(sidecar, numFields));

    // ── Open the video ───────────────────────────────────────────────────
    chd_video_t *video = nullptr;
    REQUIRE(chd_video_open_composite(tbc.c_str(), sidecar.c_str(), &video) == CHD_OK);

    chd_video_info_t info;
    REQUIRE(chd_video_get_info(video, &info) == CHD_OK);
    REQUIRE(info.num_frames == 3);

    // ── Create + configure decoder ───────────────────────────────────────
    chd_decoder_t *dec = nullptr;
    REQUIRE(chd_decoder_create(video, CHD_DEC_MONO, &dec) == CHD_OK);

    // Option-validity: MONO accepts luma_nr_level (f64) but rejects
    // comb_dimensions (i32, comb-only).
    REQUIRE(chd_decoder_has_option(dec, CHD_OPT_LUMA_NR_LEVEL) == CHD_OK);
    REQUIRE(chd_decoder_has_option(dec, CHD_OPT_COMB_DIMENSIONS) == CHD_E_INVALID_ARG);
    // Setting luma_nr_level should succeed; setting comb_dimensions should fail.
    REQUIRE(chd_decoder_set_option_f64(dec, CHD_OPT_LUMA_NR_LEVEL, 0.0) == CHD_OK);
    REQUIRE(chd_decoder_set_option_i32(dec, CHD_OPT_COMB_DIMENSIONS, 2) == CHD_E_INVALID_ARG);

    // padding_multiple = 1 to keep the active region undisturbed.
    REQUIRE(chd_decoder_set_option_i32(dec, CHD_OPT_PADDING_MULTIPLE, 1) == CHD_OK);

    // Before commit, chd_decode_frame must fail.
    {
        chd_frame_t *bad = nullptr;
        REQUIRE(chd_decode_frame(dec, 0, &bad) == CHD_E_INVALID_ARG);
        REQUIRE(bad == nullptr);
    }

    REQUIRE(chd_decoder_commit(dec) == CHD_OK);

    // After commit, set_option must reject.
    REQUIRE(chd_decoder_set_option_f64(dec, CHD_OPT_LUMA_NR_LEVEL, 0.5) == CHD_E_INVALID_ARG);

    // ── Sync decode frame 0 ──────────────────────────────────────────────
    chd_frame_t *frame = nullptr;
    REQUIRE(chd_decode_frame(dec, 0, &frame) == CHD_OK);
    REQUIRE(frame != nullptr);

    chd_frame_info_t finfo;
    REQUIRE(chd_frame_get_info(frame, &finfo) == CHD_OK);
    REQUIRE(finfo.format == CHD_PIXEL_YUV444P16);  // default
    REQUIRE(finfo.frame_index == 0);
    REQUIRE(finfo.width  > 0);
    REQUIRE(finfo.height > 0);
    REQUIRE(finfo.num_planes == 3);

    // Y plane should be black point (Y_ZERO = 4096) for an all-black input.
    const void *yData = nullptr;
    ptrdiff_t yStride = 0;
    REQUIRE(chd_frame_get_plane(frame, CHD_PLANE_Y, &yData, &yStride) == CHD_OK);
    REQUIRE(yData != nullptr);
    REQUIRE(yStride == static_cast<ptrdiff_t>(finfo.width) * 2);
    const uint16_t *yRow = static_cast<const uint16_t *>(yData);
    REQUIRE(yRow[0] == 4096);
    REQUIRE(yRow[finfo.width / 2] == 4096);

    // Cb/Cr at center for mono.
    const void *cbData = nullptr;
    ptrdiff_t cbStride = 0;
    REQUIRE(chd_frame_get_plane(frame, CHD_PLANE_CB, &cbData, &cbStride) == CHD_OK);
    REQUIRE(static_cast<const uint16_t *>(cbData)[0] == 32768);

    chd_frame_free(frame);

    // ── Out-of-range frame_index ─────────────────────────────────────────
    {
        chd_frame_t *oob = nullptr;
        REQUIRE(chd_decode_frame(dec, 99, &oob) == CHD_E_OUT_OF_RANGE);
        REQUIRE(oob == nullptr);
    }

    // ── Async decode + cancel ────────────────────────────────────────────
    struct AsyncSink {
        std::mutex                       mu;
        std::vector<chd_status_t>        statuses;
        std::vector<int64_t>             indices;
        std::vector<chd_frame_t *>       frames;
    } sink;

    auto cb = +[](void *user, chd_status_t s, int64_t idx, chd_frame_t *f) {
        auto *sk = static_cast<AsyncSink *>(user);
        std::lock_guard<std::mutex> lock(sk->mu);
        sk->statuses.push_back(s);
        sk->indices.push_back(idx);
        sk->frames.push_back(f);
    };

    {
        const int64_t idxs[2] = {0, 1};
        REQUIRE(chd_decode_frames_async(dec, idxs, 2, cb, &sink, nullptr) == CHD_OK);
        REQUIRE(sink.statuses.size() == 2);
        for (size_t i = 0; i < 2; i++) {
            REQUIRE(sink.statuses[i] == CHD_OK);
            REQUIRE(sink.frames[i] != nullptr);
            chd_frame_free(sink.frames[i]);
        }
        sink.statuses.clear();
        sink.indices.clear();
        sink.frames.clear();
    }

    // Cancel before dispatch: every callback should land with CHD_E_CANCELLED
    // and no frame.
    {
        chd_cancel_t *cancel = nullptr;
        REQUIRE(chd_cancel_create(&cancel) == CHD_OK);
        chd_cancel_request(cancel);
        REQUIRE(chd_cancel_is_requested(cancel) == 1);

        const int64_t idxs[2] = {0, 1};
        REQUIRE(chd_decode_frames_async(dec, idxs, 2, cb, &sink, cancel) == CHD_OK);
        REQUIRE(sink.statuses.size() == 2);
        for (size_t i = 0; i < 2; i++) {
            REQUIRE(sink.statuses[i] == CHD_E_CANCELLED);
            REQUIRE(sink.frames[i] == nullptr);
        }
        chd_cancel_free(cancel);
    }

    chd_decoder_free(dec);
    chd_video_free(video);

    fs::remove(tbc);
    fs::remove(sidecar);
    fs::remove(dir);

    std::cout << "test_decode_frame_abi: PASS\n";
    return 0;
}
