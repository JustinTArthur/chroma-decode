// SPDX-License-Identifier: GPL-3.0-or-later
//
// End-to-end smoke tests for the CVBS readers.
//
//   1. Synthesise a `.composite` file with two NTSC fields' worth of
//      uniform CVBS_U10_4FSC samples + a `.meta` sqlite sidecar with
//      preset = NTSC, encoding = CVBS_U10_4FSC, state = STANDARD_TBC_LOCKED.
//      Exercise chd_video_open_composite + chd_video_get_info; verify
//      the field count and sample-encoding fold (× 64).
//
//   2. Synthesise a YC pair (`.y` + `.c`) with the same metadata; verify
//      composite synthesis = luma + (chroma − 512).
//
//   3. Synthesise a meta sidecar with an unknown preset; verify the open
//      fails with CHD_E_METADATA_CORRUPT and a descriptive error.

#include <chromadec/video.h>

#include <sqlite3.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

// Internal-only headers — pulled in via the static lib link in meson.build
// (matches the test_encode_orc_color_bars pattern).
#include "../../src/reader/cvbs_composite_source.h"
#include "../../src/reader/cvbs_yc_source.h"
#include "../../src/format/video_standards.h"

namespace fs = std::filesystem;

namespace {

#define REQUIRE(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #cond \
                  << " (last_error: " << (chd_last_error() ? chd_last_error() : "(none)") << ")\n"; \
        return 1; \
    } \
} while (0)

const char *kCvbsSchema = R"(
PRAGMA user_version = 7;

CREATE TABLE cvbs_file (
    cvbs_file_id                INTEGER PRIMARY KEY,
    preset                      TEXT    NOT NULL,
    sample_encoding_preset      TEXT    NOT NULL,
    signal_state_preset         TEXT    NOT NULL,
    signal_type                 TEXT    NOT NULL,
    decoder                     TEXT    NOT NULL,
    git_branch                  TEXT,
    git_commit                  TEXT,
    number_of_sequential_frames INTEGER,
    black_level                 INTEGER,
    has_nonstandard_values      BOOLEAN,
    capture_notes               TEXT
);
)";

bool writeMetaSidecar(const std::string &metaPath, const std::string &preset,
                      const std::string &encoding, const std::string &state,
                      const std::string &signalType) {
    if (fs::exists(metaPath)) fs::remove(metaPath);
    sqlite3 *db = nullptr;
    if (sqlite3_open(metaPath.c_str(), &db) != SQLITE_OK) return false;
    char *err = nullptr;
    if (sqlite3_exec(db, kCvbsSchema, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "schema error: " << err << "\n";
        sqlite3_free(err);
        sqlite3_close(db);
        return false;
    }
    std::string sql =
        "INSERT INTO cvbs_file (cvbs_file_id, preset, sample_encoding_preset, "
        "signal_state_preset, signal_type, decoder) VALUES (1, '" + preset + "', '" +
        encoding + "', '" + state + "', '" + signalType + "', 'cvbs-encode')";
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "insert error: " << err << "\n";
        sqlite3_free(err);
        sqlite3_close(db);
        return false;
    }
    sqlite3_close(db);
    return true;
}

// Write a binary file of `numSamples` int16_t values, all equal to `value`.
bool writeUniformSamples(const std::string &path, size_t numSamples, int16_t value) {
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;
    std::vector<int16_t> buf(numSamples, value);
    out.write(reinterpret_cast<const char *>(buf.data()),
              static_cast<std::streamsize>(buf.size() * sizeof(int16_t)));
    return out.good();
}

int testCompositeOpen() {
    fs::path tmpDir = fs::temp_directory_path() / "chd_phase_d_test";
    fs::create_directories(tmpDir);
    const std::string composite = (tmpDir / "test.composite").string();
    const std::string meta      = (tmpDir / "test.meta").string();

    // Two NTSC fields = 2 × 910 × 263 = 478,660 samples of value 256 (blanking
    // in 10-bit CVBS_U10_4FSC). After conversion: 256 × 64 = 16384.
    const size_t samplesPerField = 910 * 263;
    REQUIRE(writeUniformSamples(composite, samplesPerField * 2, 256));
    REQUIRE(writeMetaSidecar(meta, "NTSC", "CVBS_U10_4FSC", "STANDARD_TBC_LOCKED", "composite"));

    chd_video_t *v = nullptr;
    REQUIRE(chd_video_open_composite(composite.c_str(), meta.c_str(), nullptr, &v) == CHD_OK);
    REQUIRE(v != nullptr);

    chd_video_info_t info{};
    REQUIRE(chd_video_get_info(v, &info) == CHD_OK);
    REQUIRE(info.standard     == CHD_STD_NTSC);
    REQUIRE(info.encoding     == CHD_ENC_CVBS_U10_4FSC);
    REQUIRE(info.signal_state == CHD_SIG_STANDARD_TBC_LOCKED);
    REQUIRE(info.field_width  == 910);
    REQUIRE(info.field_height == 263);
    REQUIRE(info.num_frames   == 1);  // 2 fields = 1 frame

    chd_video_free(v);
    return 0;
}

int testCompositeMissingMeta() {
    fs::path tmpDir = fs::temp_directory_path() / "chd_phase_d_test";
    fs::create_directories(tmpDir);
    const std::string composite = (tmpDir / "nometa.composite").string();
    const std::string meta      = (tmpDir / "nometa.meta").string();
    if (fs::exists(meta)) fs::remove(meta);
    REQUIRE(writeUniformSamples(composite, 910 * 263 * 2, 256));

    // No metadata sidecar, no override → CHD_E_METADATA_MISSING.
    chd_video_t *v = nullptr;
    const chd_status_t rc = chd_video_open_composite(composite.c_str(), nullptr, nullptr, &v);
    REQUIRE(rc == CHD_E_METADATA_MISSING);
    REQUIRE(v == nullptr);
    return 0;
}

int testCompositeUnknownPreset() {
    fs::path tmpDir = fs::temp_directory_path() / "chd_phase_d_test";
    fs::create_directories(tmpDir);
    const std::string composite = (tmpDir / "bad.composite").string();
    const std::string meta      = (tmpDir / "bad.meta").string();
    REQUIRE(writeUniformSamples(composite, 910 * 263 * 2, 256));
    REQUIRE(writeMetaSidecar(meta, "DOES_NOT_EXIST", "CVBS_U10_4FSC", "STANDARD_TBC_LOCKED", "composite"));

    chd_video_t *v = nullptr;
    const chd_status_t rc = chd_video_open_composite(composite.c_str(), meta.c_str(), nullptr, &v);
    REQUIRE(rc == CHD_E_METADATA_CORRUPT);
    REQUIRE(v == nullptr);
    return 0;
}

int testCompositeOverride() {
    // No sidecar; supply chd_video_params_t override.
    fs::path tmpDir = fs::temp_directory_path() / "chd_phase_d_test";
    fs::create_directories(tmpDir);
    const std::string composite = (tmpDir / "override.composite").string();
    const std::string meta      = (tmpDir / "override.meta").string();
    if (fs::exists(meta)) fs::remove(meta);
    REQUIRE(writeUniformSamples(composite, 910 * 263 * 2, 256));

    chd_video_params_t params{};
    params.standard     = CHD_STD_NTSC;
    params.encoding     = CHD_ENC_CVBS_U10_4FSC;
    params.signal_state = CHD_SIG_STANDARD_TBC_UNLOCKED;

    chd_video_t *v = nullptr;
    REQUIRE(chd_video_open_composite(composite.c_str(), nullptr, &params, &v) == CHD_OK);

    chd_video_info_t info{};
    REQUIRE(chd_video_get_info(v, &info) == CHD_OK);
    REQUIRE(info.signal_state == CHD_SIG_STANDARD_TBC_UNLOCKED);
    chd_video_free(v);
    return 0;
}

int testCompositeSampleConversion() {
    // Read a field via the internal source (the C ABI doesn't yet expose
    // field access). Confirms the per-encoding ×64 fold.
    using namespace chd::format;
    fs::path tmpDir = fs::temp_directory_path() / "chd_phase_d_test";
    fs::create_directories(tmpDir);
    const std::string composite = (tmpDir / "samples.composite").string();
    const size_t samplesPerField = 910 * 263;
    // Write samples with int16 value 282 (10-bit "black" for PAL — testing
    // the conversion path itself).
    REQUIRE(writeUniformSamples(composite, samplesPerField, 282));

    chd::reader::CvbsCompositeSource src;
    REQUIRE(src.open(composite, getVideoStandard(VideoStandard::NTSC),
                     SampleEncoding::CVBS_U10_4FSC, SignalState::STANDARD_TBC_LOCKED));
    REQUIRE(src.isSourceValid());
    REQUIRE(src.getNumberOfAvailableFields() == 1);

    auto data = src.getVideoField(1);
    REQUIRE(data.size() == samplesPerField);
    REQUIRE(data[0] == 282 * 64);
    REQUIRE(data[samplesPerField - 1] == 282 * 64);
    return 0;
}

int testYcSynthesis() {
    using namespace chd::format;
    fs::path tmpDir = fs::temp_directory_path() / "chd_phase_d_test";
    fs::create_directories(tmpDir);
    const std::string yPath = (tmpDir / "yc.y").string();
    const std::string cPath = (tmpDir / "yc.c").string();

    // One NTSC field of luma at value 282 (10-bit "black") + chroma at value
    // 600 (excursion of +88 from centre 512). After synthesis:
    //   composite = 282 × 64 + (600 − 512) × 64 = 18048 + 5632 = 23680.
    const size_t samplesPerField = 910 * 263;
    REQUIRE(writeUniformSamples(yPath, samplesPerField, 282));
    REQUIRE(writeUniformSamples(cPath, samplesPerField, 600));

    chd::reader::CvbsYcSource src;
    REQUIRE(src.open(yPath, cPath, getVideoStandard(VideoStandard::NTSC),
                     SampleEncoding::CVBS_U10_4FSC, SignalState::STANDARD_TBC_LOCKED));
    auto data = src.getVideoField(1);
    REQUIRE(data.size() == samplesPerField);
    REQUIRE(data[0] == 18048 + 5632);
    return 0;
}

}  // namespace

int main() {
    int rc = 0;
    rc |= testCompositeOpen();
    rc |= testCompositeMissingMeta();
    rc |= testCompositeUnknownPreset();
    rc |= testCompositeOverride();
    rc |= testCompositeSampleConversion();
    rc |= testYcSynthesis();
    if (rc == 0) std::cout << "All CVBS reader tests passed.\n";
    return rc;
}
