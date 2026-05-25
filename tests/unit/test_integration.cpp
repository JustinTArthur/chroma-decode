// SPDX-License-Identifier: GPL-3.0-or-later
//
// Integration tests: drive end-to-end fixtures via the public C ABI.
//
// The synthetic-black test in test_decode_frame_abi.cpp confirms the ABI
// plumbing is sound, but it can't catch chroma-decode regressions because
// every plane comes back at the YUV neutral point. This test fills the gap
// by running the real encode-orc generator to synthesize an NTSC 75 %
// colour-bars TBC, then verifying the resulting chd_frame_t carries real
// luma + chroma energy (wide Y range + non-trivial Cb/Cr swing).
//
// Two env vars gate the integration paths:
//   CHD_ENCODE_ORC          — absolute path to a built encode-orc binary
//                             (integration-test prerequisite). Without it, every
//                             test self-skips with PASS so plain `meson
//                             test` stays green on machines that don't
//                             have the encoder.
//   CHD_ENCODE_ORC_ASSETS   — optional. Directory containing
//                             encode-orc's NTSC/PAL raw assets (the
//                             `525_5994_*.raw` files). Defaults to the
//                             sibling `assets/` directory next to the
//                             encode-orc binary's parent build dir, i.e.
//                             `<dir-of-CHD_ENCODE_ORC>/../assets`.

#include <chromadec/chromadec.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

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

// Minimal NTSC 75 % color-bars project. duration=1 produces a single frame,
// which is enough to exercise the SourceField → Decoder → OutputWriter
// → chd_frame path with real chroma content. encode-orc expands the two
// ${ENCODE_ORC_*} variables from the environment when it parses the YAML.
const char *kProjectYaml = R"YAML(
name: "chd-integration-integration"
description: "Single-frame NTSC 75 % colour bars for chromadec integration tests"

output:
  filename: "${ENCODE_ORC_OUTPUT_ROOT}/fixture"
  format: "ntsc-composite"
  writer: "tbc"
  metadata_decoder: "encode-orc"

laserdisc:
  mode: "cav"

pipeline:
  preprocessing:
    filters:
      chroma:
        enabled: true
      luma:
        enabled: false

sections:
  - name: "Bars"
    duration: 1
    source:
      type: "yuv422-image"
      file: "${ENCODE_ORC_ASSETS}/ntsc-raw/525_5994_75_BARS.raw"
)YAML";

std::string deriveAssetsPath(const std::string &binaryPath) {
    // encode-orc lives at $repo/build/encode-orc; assets at $repo/assets.
    // The grandparent of the binary is the repo root.
    fs::path p(binaryPath);
    return (p.parent_path().parent_path() / "assets").string();
}

// Run a shell command and return its exit status. Output goes to the
// child's stdout/stderr; we don't capture it because encode-orc writes
// useful progress messages we want to see when something fails locally.
int runShell(const std::string &cmd) {
    return std::system(cmd.c_str());
}

bool writeFile(const std::string &path, const std::string &content) {
    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << content;
    return f.good();
}

int testEncodeOrcColourBars() {
    const char *encodeOrc = std::getenv("CHD_ENCODE_ORC");
    if (encodeOrc == nullptr || encodeOrc[0] == 0) {
        std::cout << "test_integration: CHD_ENCODE_ORC unset — "
                     "skipping encode-orc integration\n";
        return 0;
    }
    if (!fs::exists(encodeOrc)) {
        std::cerr << "FAIL: CHD_ENCODE_ORC=" << encodeOrc << " does not exist\n";
        return 1;
    }

    const char *assetsEnv = std::getenv("CHD_ENCODE_ORC_ASSETS");
    const std::string assets = (assetsEnv && assetsEnv[0])
                                   ? std::string(assetsEnv)
                                   : deriveAssetsPath(encodeOrc);
    if (!fs::exists(assets + "/ntsc-raw/525_5994_75_BARS.raw")) {
        std::cerr << "FAIL: assets dir " << assets
                  << " missing ntsc-raw/525_5994_75_BARS.raw "
                     "(set CHD_ENCODE_ORC_ASSETS to override)\n";
        return 1;
    }

    const fs::path dir = fs::temp_directory_path() / "chd_integration";
    fs::remove_all(dir);
    fs::create_directories(dir);

    const std::string yamlPath = (dir / "project.yaml").string();
    REQUIRE(writeFile(yamlPath, kProjectYaml));

    const std::string cmd =
        "ENCODE_ORC_OUTPUT_ROOT=\"" + dir.string() + "\" " +
        "ENCODE_ORC_ASSETS=\"" + assets + "\" " +
        "\"" + encodeOrc + "\" \"" + yamlPath + "\" --log-level warn";
    if (runShell(cmd) != 0) {
        std::cerr << "FAIL: encode-orc invocation failed: " << cmd << "\n";
        return 1;
    }

    const std::string tbcPath = (dir / "fixture.tbc").string();
    const std::string sidecarPath = (dir / "fixture.tbc.db").string();
    REQUIRE(fs::exists(tbcPath));
    REQUIRE(fs::exists(sidecarPath));

    chd_video_t *video = nullptr;
    REQUIRE(chd_video_open_composite(tbcPath.c_str(), sidecarPath.c_str(), &video) == CHD_OK);

    chd_video_info_t info{};
    REQUIRE(chd_video_get_info(video, &info) == CHD_OK);
    REQUIRE(info.standard == CHD_STD_NTSC);
    REQUIRE(info.field_width == 910);
    REQUIRE(info.num_frames >= 1);

    chd_decoder_t *dec = nullptr;
    REQUIRE(chd_decoder_create(video, CHD_DEC_NTSC_2D, &dec) == CHD_OK);
    REQUIRE(chd_decoder_set_option_i32(dec, CHD_OPT_PADDING_MULTIPLE, 1) == CHD_OK);
    REQUIRE(chd_decoder_commit(dec) == CHD_OK);

    chd_frame_t *frame = nullptr;
    REQUIRE(chd_decode_frame(dec, 0, &frame) == CHD_OK);

    chd_frame_info_t finfo{};
    REQUIRE(chd_frame_get_info(frame, &finfo) == CHD_OK);
    REQUIRE(finfo.format == CHD_PIXEL_YUV444P16);
    REQUIRE(finfo.width > 0 && finfo.height > 0);
    REQUIRE(finfo.num_planes == 3);

    // Walk every pixel in every plane; gather min/max. Color bars produce:
    //   Y  — full swing from black (Y_ZERO=4096) up toward Y_MAX (~65000)
    //   Cb — wide swing well outside the C_ZERO=32768 neutral point
    //   Cr — same
    const void *yp = nullptr, *cbp = nullptr, *crp = nullptr;
    ptrdiff_t ys = 0, cbs = 0, crs = 0;
    REQUIRE(chd_frame_get_plane(frame, CHD_PLANE_Y,  &yp,  &ys)  == CHD_OK);
    REQUIRE(chd_frame_get_plane(frame, CHD_PLANE_CB, &cbp, &cbs) == CHD_OK);
    REQUIRE(chd_frame_get_plane(frame, CHD_PLANE_CR, &crp, &crs) == CHD_OK);
    const uint16_t *y  = static_cast<const uint16_t *>(yp);
    const uint16_t *cb = static_cast<const uint16_t *>(cbp);
    const uint16_t *cr = static_cast<const uint16_t *>(crp);

    int yMin = 65535, yMax = 0;
    int cbMin = 65535, cbMax = 0;
    int crMin = 65535, crMax = 0;
    const size_t nPixels = static_cast<size_t>(finfo.width) * finfo.height;
    for (size_t i = 0; i < nPixels; i++) {
        if (y[i]  < yMin) yMin = y[i];
        if (y[i]  > yMax) yMax = y[i];
        if (cb[i] < cbMin) cbMin = cb[i];
        if (cb[i] > cbMax) cbMax = cb[i];
        if (cr[i] < crMin) crMin = cr[i];
        if (cr[i] > crMax) crMax = cr[i];
    }

    // Sanity thresholds — wide enough to be robust against any small
    // decoder tweak, tight enough that a regression flattening the
    // chroma to C_ZERO (the bug an all-black fixture would miss) shows
    // up immediately. 75 % colour bars on a real NTSC decoder give
    // chroma swings on the order of 20000 u16 codes in each direction
    // from C_ZERO; setting the bar at ±5000 leaves a lot of headroom.
    REQUIRE(yMax  - yMin  > 30000);
    REQUIRE(cbMax - cbMin > 10000);
    REQUIRE(crMax - crMin > 10000);
    // The chroma planes must straddle the neutral point — pure-positive
    // or pure-negative would mean we lost the carrier demod.
    REQUIRE(cbMin < 32768 && cbMax > 32768);
    REQUIRE(crMin < 32768 && crMax > 32768);

    std::cout << "test_integration: encode-orc colour bars OK"
              << "  Y=[" << yMin << "," << yMax << "]"
              << "  Cb=[" << cbMin << "," << cbMax << "]"
              << "  Cr=[" << crMin << "," << crMax << "]\n";

    chd_frame_free(frame);
    chd_decoder_free(dec);
    chd_video_free(video);
    fs::remove_all(dir);
    return 0;
}

}  // namespace

int main() {
    if (int rc = testEncodeOrcColourBars(); rc != 0) return rc;
    std::cout << "test_integration: PASS\n";
    return 0;
}
