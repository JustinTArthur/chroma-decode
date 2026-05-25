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
// Env vars gate the integration paths:
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
//   CHD_LD_CHROMA_DECODER   — absolute path to a built ld-chroma-decoder
//                             binary, ideally at ld-decode commit
//                             f39e59e18 (the last good before the
//                             tools/ deletion in a4e403be). When set,
//                             the golden-frame comparison runs and
//                             asserts pixel-identical Y/Cb/Cr planes
//                             against the legacy reference.

#include <chromadec/chromadec.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

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

// Run encode-orc against the bundled NTSC 75 % colour-bars asset and
// return the produced fixture paths via out params. Caller owns the
// temp dir cleanup. Returns 0 on success, non-zero on failure.
//
// `outDir` is the temp directory containing the fixture; `tbcOut` and
// `sidecarOut` are the absolute paths of the resulting .tbc + .tbc.db
// pair.
int buildEncodeOrcFixture(const std::string &encodeOrc, const std::string &assets,
                          const fs::path &outDir,
                          std::string *tbcOut, std::string *sidecarOut) {
    fs::create_directories(outDir);

    const std::string yamlPath = (outDir / "project.yaml").string();
    REQUIRE(writeFile(yamlPath, kProjectYaml));

    const std::string cmd =
        "ENCODE_ORC_OUTPUT_ROOT=\"" + outDir.string() + "\" " +
        "ENCODE_ORC_ASSETS=\"" + assets + "\" " +
        "\"" + encodeOrc + "\" \"" + yamlPath + "\" --log-level warn";
    if (runShell(cmd) != 0) {
        std::cerr << "FAIL: encode-orc invocation failed: " << cmd << "\n";
        return 1;
    }

    *tbcOut = (outDir / "fixture.tbc").string();
    *sidecarOut = (outDir / "fixture.tbc.db").string();
    REQUIRE(fs::exists(*tbcOut));
    REQUIRE(fs::exists(*sidecarOut));
    return 0;
}

// Common skip / dispatch logic for tests that need encode-orc. Returns
// 0 (run) or 1 (skip — caller should `return 0` immediately). Sets the
// resolved binary + assets paths on success.
int resolveEncodeOrc(std::string *binaryOut, std::string *assetsOut) {
    const char *encodeOrc = std::getenv("CHD_ENCODE_ORC");
    if (encodeOrc == nullptr || encodeOrc[0] == 0) return 1;  // skip
    if (!fs::exists(encodeOrc)) {
        std::cerr << "FAIL: CHD_ENCODE_ORC=" << encodeOrc << " does not exist\n";
        return -1;
    }
    const char *assetsEnv = std::getenv("CHD_ENCODE_ORC_ASSETS");
    const std::string assets = (assetsEnv && assetsEnv[0])
                                   ? std::string(assetsEnv)
                                   : deriveAssetsPath(encodeOrc);
    if (!fs::exists(assets + "/ntsc-raw/525_5994_75_BARS.raw")) {
        std::cerr << "FAIL: assets dir " << assets
                  << " missing ntsc-raw/525_5994_75_BARS.raw "
                     "(set CHD_ENCODE_ORC_ASSETS to override)\n";
        return -1;
    }
    *binaryOut = encodeOrc;
    *assetsOut = assets;
    return 0;
}

// Decode frame 0 of a TBC via the public C ABI and pack Y|Cb|Cr planes
// into a contiguous u16 vector. Returns the per-plane width/height in
// `widthOut`/`heightOut`. Returns 0 on success, non-zero on failure.
int decodeFrameViaAbi(const std::string &tbc, const std::string &sidecar,
                     chd_decoder_kind_t kind,
                     std::vector<uint16_t> *yuvOut,
                     int32_t *widthOut, int32_t *heightOut) {
    chd_video_t *video = nullptr;
    REQUIRE(chd_video_open_composite(tbc.c_str(), sidecar.c_str(), &video) == CHD_OK);

    chd_decoder_t *dec = nullptr;
    REQUIRE(chd_decoder_create(video, kind, &dec) == CHD_OK);
    REQUIRE(chd_decoder_set_option_i32(dec, CHD_OPT_PADDING_MULTIPLE, 1) == CHD_OK);
    REQUIRE(chd_decoder_commit(dec) == CHD_OK);

    chd_frame_t *frame = nullptr;
    REQUIRE(chd_decode_frame(dec, 0, &frame) == CHD_OK);

    chd_frame_info_t fi{};
    REQUIRE(chd_frame_get_info(frame, &fi) == CHD_OK);
    REQUIRE(fi.format == CHD_PIXEL_YUV444P16);

    const size_t plane = static_cast<size_t>(fi.width) * fi.height;
    yuvOut->resize(plane * 3);

    const void *yp = nullptr, *cbp = nullptr, *crp = nullptr;
    ptrdiff_t s = 0;
    REQUIRE(chd_frame_get_plane(frame, CHD_PLANE_Y,  &yp,  &s) == CHD_OK);
    REQUIRE(chd_frame_get_plane(frame, CHD_PLANE_CB, &cbp, &s) == CHD_OK);
    REQUIRE(chd_frame_get_plane(frame, CHD_PLANE_CR, &crp, &s) == CHD_OK);
    std::memcpy(yuvOut->data(),             yp,  plane * 2);
    std::memcpy(yuvOut->data() + plane,     cbp, plane * 2);
    std::memcpy(yuvOut->data() + 2 * plane, crp, plane * 2);

    *widthOut  = fi.width;
    *heightOut = fi.height;

    chd_frame_free(frame);
    chd_decoder_free(dec);
    chd_video_free(video);
    return 0;
}

int testEncodeOrcColourBars() {
    std::string encodeOrc, assets;
    int rc = resolveEncodeOrc(&encodeOrc, &assets);
    if (rc < 0) return 1;
    if (rc > 0) {
        std::cout << "test_integration: CHD_ENCODE_ORC unset — "
                     "skipping encode-orc integration\n";
        return 0;
    }

    const fs::path dir = fs::temp_directory_path() / "chd_integration_chroma";
    fs::remove_all(dir);

    std::string tbc, sidecar;
    if (buildEncodeOrcFixture(encodeOrc, assets, dir, &tbc, &sidecar) != 0) return 1;

    std::vector<uint16_t> yuv;
    int32_t w = 0, h = 0;
    if (decodeFrameViaAbi(tbc, sidecar, CHD_DEC_NTSC_2D, &yuv, &w, &h) != 0) return 1;

    const size_t plane = static_cast<size_t>(w) * h;
    const uint16_t *y  = yuv.data();
    const uint16_t *cb = yuv.data() + plane;
    const uint16_t *cr = yuv.data() + 2 * plane;

    // Walk every pixel; gather min/max. Color bars produce wide chroma
    // swings — the synthetic-black fixtures elsewhere can't see this.
    int yMin = 65535, yMax = 0;
    int cbMin = 65535, cbMax = 0;
    int crMin = 65535, crMax = 0;
    for (size_t i = 0; i < plane; i++) {
        if (y[i]  < yMin) yMin = y[i];
        if (y[i]  > yMax) yMax = y[i];
        if (cb[i] < cbMin) cbMin = cb[i];
        if (cb[i] > cbMax) cbMax = cb[i];
        if (cr[i] < crMin) crMin = cr[i];
        if (cr[i] > crMax) crMax = cr[i];
    }

    // Sanity thresholds — wide enough to survive future decoder tweaks,
    // tight enough that a regression flattening chroma to C_ZERO shows
    // up immediately. 75 % colour bars give chroma swings on the order
    // of 20000 u16 codes in each direction from C_ZERO; ±5000 leaves
    // plenty of headroom.
    REQUIRE(yMax  - yMin  > 30000);
    REQUIRE(cbMax - cbMin > 10000);
    REQUIRE(crMax - crMin > 10000);
    // Chroma must straddle the neutral point — pure-positive or
    // pure-negative would mean we lost the carrier demod.
    REQUIRE(cbMin < 32768 && cbMax > 32768);
    REQUIRE(crMin < 32768 && crMax > 32768);

    std::cout << "test_integration: encode-orc colour bars OK"
              << "  Y=[" << yMin << "," << yMax << "]"
              << "  Cb=[" << cbMin << "," << cbMax << "]"
              << "  Cr=[" << crMin << "," << crMax << "]\n";

    fs::remove_all(dir);
    return 0;
}

// Golden-frame comparison: run the legacy ld-chroma-decoder (at the
// pinned f39e59e18 commit) against the encode-orc fixture and assert
// chd's YUV444P16 output matches pixel-for-pixel. The port of
// the upstream Comb / PALColour / OutputWriter chain should produce
// bit-identical results to the reference; any drift here means a
// regression in the chroma pipeline.
int testGoldenFrameComparison() {
    const char *ldChroma = std::getenv("CHD_LD_CHROMA_DECODER");
    if (ldChroma == nullptr || ldChroma[0] == 0) {
        std::cout << "test_integration: CHD_LD_CHROMA_DECODER unset — "
                     "skipping golden-frame comparison\n";
        return 0;
    }
    if (!fs::exists(ldChroma)) {
        std::cerr << "FAIL: CHD_LD_CHROMA_DECODER=" << ldChroma
                  << " does not exist\n";
        return 1;
    }

    std::string encodeOrc, assets;
    int rc = resolveEncodeOrc(&encodeOrc, &assets);
    if (rc < 0) return 1;
    if (rc > 0) {
        // Golden compare needs encode-orc too; quietly skip in that case
        // rather than failing — the encode-orc test above already
        // explains the situation.
        std::cout << "test_integration: CHD_ENCODE_ORC unset — "
                     "skipping golden-frame comparison\n";
        return 0;
    }

    const fs::path dir = fs::temp_directory_path() / "chd_integration_golden";
    fs::remove_all(dir);

    std::string tbc, sidecar;
    if (buildEncodeOrcFixture(encodeOrc, assets, dir, &tbc, &sidecar) != 0) return 1;

    // Drive the legacy decoder with the same options the chd ABI uses
    // (NTSC 2D comb, padding=1, YUV output). The reference reads the
    // sidecar from `--input-metadata` by default.
    const std::string goldenPath = (dir / "golden.yuv").string();
    const std::string cmd =
        "\"" + std::string(ldChroma) + "\""
        " -f ntsc2d -p yuv --pad 1 --quiet"
        " --input-metadata \"" + sidecar + "\""
        " \"" + tbc + "\""
        " \"" + goldenPath + "\"";
    if (runShell(cmd) != 0) {
        std::cerr << "FAIL: ld-chroma-decoder invocation failed: " << cmd << "\n";
        return 1;
    }
    REQUIRE(fs::exists(goldenPath));

    std::vector<uint16_t> chdYuv;
    int32_t w = 0, h = 0;
    if (decodeFrameViaAbi(tbc, sidecar, CHD_DEC_NTSC_2D, &chdYuv, &w, &h) != 0) return 1;

    const size_t plane = static_cast<size_t>(w) * h;
    std::vector<uint16_t> goldenYuv(plane * 3);
    {
        std::ifstream gf(goldenPath, std::ios::binary);
        REQUIRE(gf.is_open());
        gf.read(reinterpret_cast<char *>(goldenYuv.data()),
                static_cast<std::streamsize>(plane * 3 * 2));
        REQUIRE(gf.good());
    }

    // Per-plane comparison: assert exact match. The chd port descended
    // from the same Comb / PALColour / OutputWriter source as the
    // legacy binary, so identity is the right bar; any drift signals
    // a regression that needs investigating.
    auto checkPlane = [&](const char *name,
                           const uint16_t *a, const uint16_t *b) -> int {
        int maxDiff = 0;
        size_t exact = 0;
        for (size_t i = 0; i < plane; i++) {
            int d = static_cast<int>(a[i]) - static_cast<int>(b[i]);
            if (d < 0) d = -d;
            if (d > maxDiff) maxDiff = d;
            if (d == 0) exact++;
        }
        std::cout << "  " << name << " plane: max|diff|=" << maxDiff
                  << "  exact=" << exact << "/" << plane << "\n";
        if (maxDiff != 0) {
            std::cerr << "FAIL: " << name
                      << " plane drifted from ld-chroma-decoder reference\n";
            return 1;
        }
        return 0;
    };

    std::cout << "test_integration: golden-frame comparison ("
              << w << "x" << h << " YUV444P16)\n";
    if (checkPlane("Y ", chdYuv.data(),             goldenYuv.data()))             return 1;
    if (checkPlane("Cb", chdYuv.data() + plane,     goldenYuv.data() + plane))     return 1;
    if (checkPlane("Cr", chdYuv.data() + 2 * plane, goldenYuv.data() + 2 * plane)) return 1;
    std::cout << "test_integration: golden frame OK (bit-exact)\n";

    fs::remove_all(dir);
    return 0;
}

}  // namespace

int main() {
    if (int rc = testEncodeOrcColourBars();   rc != 0) return rc;
    if (int rc = testGoldenFrameComparison(); rc != 0) return rc;
    std::cout << "test_integration: PASS\n";
    return 0;
}
