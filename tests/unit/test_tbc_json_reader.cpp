// SPDX-License-Identifier: GPL-3.0-or-later
//
// JSON sidecar back-compat sanity test: synthesise a legacy `.tbc.json` pair
// on disk and exercise chd_video_open_composite, which should fall through from
// the missing `.tbc.db` to `.tbc.json` and load the legacy schema.

#include <chromadec/video.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Legacy ld-decode JSON schema with the keys readJsonImpl/per-struct
// read() methods walk. Fields array has 4 sequential fields (2 frames).
const char *kJsonSidecar = R"({
  "videoParameters": {
    "activeVideoEnd": 1791,
    "activeVideoStart": 192,
    "black16bIre": 17920,
    "colourBurstEnd": 119,
    "colourBurstStart": 92,
    "fieldHeight": 263,
    "fieldWidth": 910,
    "isMapped": true,
    "isSubcarrierLocked": true,
    "isWidescreen": false,
    "numberOfSequentialFields": 4,
    "sampleRate": 14318181.818,
    "system": "NTSC",
    "white16bIre": 51200
  },
  "fields": [
    {"seqNo": 1, "isFirstField": true,  "syncConf": 100, "medianBurstIRE": 25.0},
    {"seqNo": 2, "isFirstField": false, "syncConf": 100, "medianBurstIRE": 25.0},
    {"seqNo": 3, "isFirstField": true,  "syncConf": 100, "medianBurstIRE": 25.0},
    {"seqNo": 4, "isFirstField": false, "syncConf": 100, "medianBurstIRE": 25.0}
  ]
})";

bool writeFakeTbc(const std::string &path, int32_t fieldWidth, int32_t fieldHeight, int32_t numFields) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    const std::vector<uint16_t> buffer(fieldWidth * fieldHeight, 0);
    for (int32_t i = 0; i < numFields; i++) {
        f.write(reinterpret_cast<const char *>(buffer.data()), buffer.size() * 2);
    }
    return true;
}

bool writeJsonSidecar(const std::string &path) {
    std::ofstream f(path);
    if (!f) return false;
    f << kJsonSidecar;
    return f.good();
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
    const fs::path dir = fs::temp_directory_path() / "chd_json_sidecar_test";
    fs::create_directories(dir);
    const std::string tbc     = (dir / "test.tbc").string();
    const std::string sidecar = (dir / "test.tbc.json").string();
    const std::string dbPath  = (dir / "test.tbc.db").string();

    if (fs::exists(tbc))     fs::remove(tbc);
    if (fs::exists(sidecar)) fs::remove(sidecar);
    if (fs::exists(dbPath))  fs::remove(dbPath);

    REQUIRE(writeFakeTbc(tbc, 910, 263, 4));
    REQUIRE(writeJsonSidecar(sidecar));

    // No .tbc.db on disk, so resolveSidecar should fall through to .tbc.json.
    chd_video_t *video = nullptr;
    chd_status_t st = chd_video_open_composite(tbc.c_str(), nullptr, nullptr, &video);
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

    // Explicit-path form should also work when caller passes the .json path.
    chd_video_t *video2 = nullptr;
    st = chd_video_open_composite(tbc.c_str(), sidecar.c_str(), nullptr, &video2);
    REQUIRE(st == CHD_OK);
    REQUIRE(video2 != nullptr);
    chd_video_free(video2);

    fs::remove(tbc);
    fs::remove(sidecar);
    fs::remove(dir);

    std::cout << "test_tbc_json_reader: PASS\n";
    return 0;
}
