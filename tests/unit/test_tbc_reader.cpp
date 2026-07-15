// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sanity test: synthesise a minimal TBC + sidecar pair on disk and
// exercise chd_video_open_composite / chd_video_get_info / chd_video_free.

#include <chromadec/video.h>

#include <sqlite3.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Same schema bytes the writer would emit; pasted in line so the test does
// not link against internal symbols.
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
    ntsc_white_flag INTEGER,
    PRIMARY KEY (capture_id, field_id)
);
)";

bool writeSidecar(const std::string &path) {
    sqlite3 *db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) return false;

    char *errMsg = nullptr;
    if (sqlite3_exec(db, kSchema, nullptr, nullptr, &errMsg) != SQLITE_OK) {
        std::cerr << "schema exec failed: " << (errMsg ? errMsg : "(null)") << "\n";
        sqlite3_free(errMsg);
        sqlite3_close(db);
        return false;
    }

    // Insert one NTSC capture with widescreen=0, sample_rate=315e6/88.
    const char *capSql =
        "INSERT INTO capture (capture_id, system, decoder, video_sample_rate, "
        "  active_video_start, active_video_end, field_width, field_height, "
        "  number_of_sequential_fields, colour_burst_start, colour_burst_end, "
        "  is_mapped, is_subcarrier_locked, is_widescreen, "
        "  white_16b_ire, black_16b_ire, blanking_16b_ire) "
        "VALUES (1, 'NTSC', 'ld-decode', 14318181.818, 192, 1791, 910, 263, "
        "        4, 92, 119, 1, 1, 0, 51200, 17920, 16384);";
    if (sqlite3_exec(db, capSql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
        std::cerr << "capture insert failed: " << (errMsg ? errMsg : "(null)") << "\n";
        sqlite3_free(errMsg);
        sqlite3_close(db);
        return false;
    }

    // Insert four sequential fields (two frames).
    const char *fieldSql =
        "INSERT INTO field_record (capture_id, field_id, is_first_field, "
        "  sync_conf, median_burst_ire, ntsc_is_fm_code_data_valid, "
        "  ntsc_fm_code_data, ntsc_field_flag, ntsc_is_video_id_data_valid, "
        "  ntsc_video_id_data, ntsc_white_flag) "
        "VALUES (1, ?, ?, 100, 25.0, 0, 0, 0, 0, 0, 0);";

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, fieldSql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "field prepare failed\n";
        sqlite3_close(db);
        return false;
    }
    for (int i = 0; i < 4; i++) {
        sqlite3_reset(stmt);
        sqlite3_bind_int(stmt, 1, i);
        sqlite3_bind_int(stmt, 2, (i % 2 == 0) ? 1 : 0);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            std::cerr << "field step failed: " << sqlite3_errmsg(db) << "\n";
            sqlite3_finalize(stmt);
            sqlite3_close(db);
            return false;
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return true;
}

bool writeFakeTbc(const std::string &path, int32_t fieldWidth, int32_t fieldHeight, int32_t numFields) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    const std::vector<uint16_t> buffer(fieldWidth * fieldHeight, 0);
    for (int32_t i = 0; i < numFields; i++) {
        f.write(reinterpret_cast<const char *>(buffer.data()), buffer.size() * 2);
    }
    return true;
}

#define REQUIRE(cond)                                                            \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " << #cond \
                      << "\n";                                                   \
            return 1;                                                            \
        }                                                                        \
    } while (0)

}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "chd_phase_b_test";
    fs::create_directories(dir);
    const std::string tbc     = (dir / "test.tbc").string();
    const std::string sidecar = (dir / "test.tbc.db").string();

    if (fs::exists(tbc))     fs::remove(tbc);
    if (fs::exists(sidecar)) fs::remove(sidecar);

    REQUIRE(writeFakeTbc(tbc, 910, 263, 4));
    REQUIRE(writeSidecar(sidecar));

    chd_video_t *video = nullptr;
    chd_status_t st = chd_video_open_composite(tbc.c_str(), sidecar.c_str(), nullptr, &video);
    REQUIRE(st == CHD_OK);
    REQUIRE(video != nullptr);

    chd_video_info_t info{};
    st = chd_video_get_info(video, &info);
    REQUIRE(st == CHD_OK);
    REQUIRE(info.standard == CHD_STD_NTSC);
    REQUIRE(info.encoding == CHD_ENC_CVBS_U16_4FSC);
    REQUIRE(info.field_width == 910);
    REQUIRE(info.field_height == 263);
    REQUIRE(info.first_active_sample == 192);
    REQUIRE(info.last_active_sample == 1790);
    REQUIRE(info.is_widescreen == 0);
    REQUIRE(info.is_subcarrier_locked == 1);
    REQUIRE(info.num_frames == 2);

    chd_video_free(video);

    fs::remove(tbc);
    fs::remove(sidecar);
    fs::remove(dir);

    std::cout << "test_tbc_reader: PASS\n";
    return 0;
}
