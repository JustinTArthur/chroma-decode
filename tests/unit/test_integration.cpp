// SPDX-License-Identifier: GPL-3.0-or-later
//
// Integration tests: drive end-to-end fixtures via the public C ABI.
//
// The synthetic-black test in test_decode_frame_abi.cpp confirms the ABI
// plumbing is sound, but it can't catch chroma-decode regressions because
// every plane comes back at the YUV neutral point. This test fills the gap
// by running the real encode-orc generator to synthesize NTSC and PAL
// 75 % colour-bars TBCs (three frames each), then verifying every decoded
// chd_frame_t carries real luma + chroma energy and — when the legacy
// reference is available — matches it pixel-for-pixel.
//
// Two encode-orc fixtures, six chd decoder variants exercised against them:
//
//   Encoder fixture        chd decoder kind          legacy -f flag
//   ─────────────────────  ────────────────────────  ──────────────
//   NTSC 75 % colour bars  CHD_DEC_NTSC_2D            ntsc2d
//   NTSC 75 % colour bars  CHD_DEC_NTSC_3D            ntsc3d
//   NTSC 75 % colour bars  CHD_DEC_NTSC_3D_NO_ADAPT   ntsc3dnoadapt
//   PAL  75 % colour bars  CHD_DEC_PAL_2D             pal2d
//   PAL  75 % colour bars  CHD_DEC_TRANSFORM_2D       transform2d
//   PAL  75 % colour bars  CHD_DEC_TRANSFORM_3D       transform3d
//
// Each variant is exercised across all three frames of its fixture. The
// 3-frame width is the minimum that gives the 3D variants real
// lookbehind + lookahead for the middle frame while still exercising the
// black-boundary fallback at frame 0 and frame 2. Multi-frame catches
// per-frame state leakage that single-frame tests would mask.
//
// Env vars gate the integration paths:
//   CHD_ENCODE_ORC          — absolute path to a built encode-orc binary
//                             (integration-test prerequisite). Without it, every
//                             test self-skips with PASS so plain `meson
//                             test` stays green on machines that don't
//                             have the encoder.
//   CHD_ENCODE_ORC_ASSETS   — optional. Directory containing
//                             encode-orc's NTSC/PAL raw assets. Defaults
//                             to the sibling `assets/` of the encode-orc
//                             binary's grandparent.
//   CHD_LD_CHROMA_DECODER   — absolute path to a built ld-chroma-decoder
//                             binary, ideally at ld-decode commit
//                             f39e59e18 (last good before the tools/
//                             deletion in a4e403be). When set, the
//                             golden-frame comparison runs and asserts
//                             pixel-identical Y/Cb/Cr planes against the
//                             legacy reference for every (encoder ×
//                             variant × frame) combination.

#include <chromadec/chromadec.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
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

// One encode-orc fixture configuration. Multiple chd decoder variants
// run against the same fixture so we don't pay the encode cost more
// than once per video standard. NTSC and PAL each get one fixture.
struct Encoder {
    const char            *label;             // for temp dir naming
    const char            *encodeOrcFormat;   // "ntsc-composite" / "pal-composite"
    const char            *assetSubpath;      // relative to ENCODE_ORC_ASSETS
    chd_video_standard_t   expectedStandard;
    int32_t                expectedFieldWidth;
    int                    durationFrames;
};

// One chd decoder kind to exercise against a fixture, paired with the
// matching legacy `-f` flag so the golden-frame compare runs the same
// algorithm on the reference side.
struct Variant {
    const char         *label;        // for logging
    chd_decoder_kind_t  chdKind;
    const char         *legacyFlag;
};

const Encoder kNtscBars = {
    /*label=*/             "ntsc-bars",
    /*encodeOrcFormat=*/   "ntsc-composite",
    /*assetSubpath=*/      "ntsc-raw/525_5994_75_BARS.raw",
    /*expectedStandard=*/  CHD_STD_NTSC,
    /*expectedFieldWidth=*/910,
    /*durationFrames=*/    3,
};

const Encoder kPalBars = {
    /*label=*/             "pal-bars",
    /*encodeOrcFormat=*/   "pal-composite",
    /*assetSubpath=*/      "pal-raw/625_50_75_BARS.raw",
    /*expectedStandard=*/  CHD_STD_PAL,
    /*expectedFieldWidth=*/1135,
    /*durationFrames=*/    3,
};

const Variant kNtscVariants[] = {
    {"ntsc-2d",         CHD_DEC_NTSC_2D,          "ntsc2d"},
    {"ntsc-3d",         CHD_DEC_NTSC_3D,          "ntsc3d"},
    {"ntsc-3d-noadapt", CHD_DEC_NTSC_3D_NO_ADAPT, "ntsc3dnoadapt"},
};

const Variant kPalVariants[] = {
    {"pal-2d",       CHD_DEC_PAL_2D,       "pal2d"},
    {"transform-2d", CHD_DEC_TRANSFORM_2D, "transform2d"},
    {"transform-3d", CHD_DEC_TRANSFORM_3D, "transform3d"},
};

std::string deriveAssetsPath(const std::string &binaryPath) {
    // encode-orc lives at $repo/build/encode-orc; assets at $repo/assets.
    fs::path p(binaryPath);
    return (p.parent_path().parent_path() / "assets").string();
}

int runShell(const std::string &cmd) {
    return std::system(cmd.c_str());
}

bool writeFile(const std::string &path, const std::string &content) {
    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << content;
    return f.good();
}

std::string renderProjectYaml(const Encoder &enc) {
    std::ostringstream s;
    s << "name: \"chd-integration-" << enc.label << "\"\n"
      << "output:\n"
      << "  filename: \"${ENCODE_ORC_OUTPUT_ROOT}/fixture\"\n"
      << "  format: \"" << enc.encodeOrcFormat << "\"\n"
      << "  writer: \"tbc\"\n"
      << "  metadata_decoder: \"encode-orc\"\n"
      << "laserdisc:\n"
      << "  mode: \"cav\"\n"
      << "pipeline:\n"
      << "  preprocessing:\n"
      << "    filters:\n"
      << "      chroma:\n"
      << "        enabled: true\n"
      << "      luma:\n"
      << "        enabled: false\n"
      << "sections:\n"
      << "  - name: \"Bars\"\n"
      << "    duration: " << enc.durationFrames << "\n"
      << "    source:\n"
      << "      type: \"yuv422-image\"\n"
      << "      file: \"${ENCODE_ORC_ASSETS}/" << enc.assetSubpath << "\"\n";
    return s.str();
}

int buildEncodeOrcFixture(const std::string &encodeOrc, const std::string &assets,
                          const Encoder &enc, const fs::path &outDir,
                          std::string *tbcOut, std::string *sidecarOut) {
    fs::create_directories(outDir);
    const std::string yamlPath = (outDir / "project.yaml").string();
    REQUIRE(writeFile(yamlPath, renderProjectYaml(enc)));

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

int resolveEncodeOrc(const Encoder &enc,
                     std::string *binaryOut, std::string *assetsOut) {
    const char *encodeOrc = std::getenv("CHD_ENCODE_ORC");
    if (encodeOrc == nullptr || encodeOrc[0] == 0) return 1;
    if (!fs::exists(encodeOrc)) {
        std::cerr << "FAIL: CHD_ENCODE_ORC=" << encodeOrc << " does not exist\n";
        return -1;
    }
    const char *assetsEnv = std::getenv("CHD_ENCODE_ORC_ASSETS");
    const std::string assets = (assetsEnv && assetsEnv[0])
                                   ? std::string(assetsEnv)
                                   : deriveAssetsPath(encodeOrc);
    const std::string assetFile = assets + "/" + enc.assetSubpath;
    if (!fs::exists(assetFile)) {
        std::cerr << "FAIL: missing asset " << assetFile
                  << " (set CHD_ENCODE_ORC_ASSETS to override)\n";
        return -1;
    }
    *binaryOut = encodeOrc;
    *assetsOut = assets;
    return 0;
}

int decodeFramesViaAbi(const std::string &tbc, const std::string &sidecar,
                       chd_decoder_kind_t kind, int64_t firstIdx, int64_t count,
                       std::vector<uint16_t> *yuvOut,
                       int32_t *widthOut, int32_t *heightOut) {
    chd_video_t *video = nullptr;
    REQUIRE(chd_video_open_composite(tbc.c_str(), sidecar.c_str(), &video) == CHD_OK);

    chd_decoder_t *dec = nullptr;
    REQUIRE(chd_decoder_create(video, kind, &dec) == CHD_OK);
    REQUIRE(chd_decoder_set_option_i32(dec, CHD_OPT_PADDING_MULTIPLE, 1) == CHD_OK);
    REQUIRE(chd_decoder_commit(dec) == CHD_OK);

    int32_t w = 0, h = 0;
    size_t planeSize = 0;
    for (int64_t k = 0; k < count; k++) {
        chd_frame_t *frame = nullptr;
        REQUIRE(chd_decode_frame(dec, firstIdx + k, &frame) == CHD_OK);

        chd_frame_info_t fi{};
        REQUIRE(chd_frame_get_info(frame, &fi) == CHD_OK);
        REQUIRE(fi.format == CHD_PIXEL_YUV444P16);

        if (k == 0) {
            w = fi.width;
            h = fi.height;
            planeSize = static_cast<size_t>(w) * h;
            yuvOut->resize(planeSize * 3 * static_cast<size_t>(count));
        } else {
            REQUIRE(fi.width == w && fi.height == h);
        }

        const void *yp = nullptr, *cbp = nullptr, *crp = nullptr;
        ptrdiff_t s = 0;
        REQUIRE(chd_frame_get_plane(frame, CHD_PLANE_Y,  &yp,  &s) == CHD_OK);
        REQUIRE(chd_frame_get_plane(frame, CHD_PLANE_CB, &cbp, &s) == CHD_OK);
        REQUIRE(chd_frame_get_plane(frame, CHD_PLANE_CR, &crp, &s) == CHD_OK);
        const size_t base = static_cast<size_t>(k) * planeSize * 3;
        std::memcpy(yuvOut->data() + base,                 yp,  planeSize * 2);
        std::memcpy(yuvOut->data() + base + planeSize,     cbp, planeSize * 2);
        std::memcpy(yuvOut->data() + base + 2 * planeSize, crp, planeSize * 2);

        chd_frame_free(frame);
    }
    *widthOut = w;
    *heightOut = h;

    chd_decoder_free(dec);
    chd_video_free(video);
    return 0;
}

// Examine one frame's three planes; assert chroma swing thresholds
// indicating the carrier was actually demodulated to YUV.
int assertChromaSwing(const Variant &v, int64_t frameIdx,
                      const uint16_t *y, const uint16_t *cb, const uint16_t *cr,
                      size_t plane) {
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
    REQUIRE(yMax  - yMin  > 30000);
    REQUIRE(cbMax - cbMin > 10000);
    REQUIRE(crMax - crMin > 10000);
    REQUIRE(cbMin < 32768 && cbMax > 32768);
    REQUIRE(crMin < 32768 && crMax > 32768);
    std::cout << "    " << v.label << " frame " << frameIdx
              << "  Y=[" << yMin << "," << yMax << "]"
              << "  Cb=[" << cbMin << "," << cbMax << "]"
              << "  Cr=[" << crMin << "," << crMax << "]\n";
    return 0;
}

// Per-(encoder,variant) chroma-content + (optional) golden-frame
// comparison. Caller passes the prebuilt encode-orc fixture so the
// encode cost is amortised across all the variants of the same
// encoder.
int runVariantChecks(const Encoder &enc, const Variant &variant,
                     const std::string &tbc, const std::string &sidecar,
                     const char *ldChroma /* nullable */) {
    std::vector<uint16_t> chdYuv;
    int32_t w = 0, h = 0;
    if (decodeFramesViaAbi(tbc, sidecar, variant.chdKind, 0, enc.durationFrames,
                           &chdYuv, &w, &h) != 0) return 1;
    const size_t plane = static_cast<size_t>(w) * h;

    std::cout << "  " << variant.label << " " << w << "x" << h
              << " * " << enc.durationFrames << " frames:\n";
    for (int64_t k = 0; k < enc.durationFrames; k++) {
        const uint16_t *base = chdYuv.data() + static_cast<size_t>(k) * plane * 3;
        if (assertChromaSwing(variant, k, base, base + plane, base + 2 * plane,
                              plane) != 0) return 1;
    }

    if (ldChroma == nullptr) return 0;

    // Golden compare: run legacy at the same -f flag and diff frame by
    // frame. Bit-exact match is the right bar — the chd port descended
    // from the same upstream sources (per-line blame preserved via the
    // structural-move chain in Phases C–F), so any drift here signals
    // a regression that needs investigating.
    const fs::path goldenDir = fs::path(tbc).parent_path() /
                               (std::string("golden-") + variant.label);
    fs::create_directories(goldenDir);
    const std::string goldenPath = (goldenDir / "golden.yuv").string();
    std::ostringstream cmd;
    cmd << "\"" << ldChroma << "\""
        << " -f " << variant.legacyFlag
        << " -p yuv --pad 1 --quiet"
        << " -l " << enc.durationFrames
        << " --input-metadata \"" << sidecar << "\""
        << " \"" << tbc << "\""
        << " \"" << goldenPath << "\"";
    if (runShell(cmd.str()) != 0) {
        std::cerr << "FAIL: ld-chroma-decoder invocation failed: " << cmd.str() << "\n";
        return 1;
    }
    REQUIRE(fs::exists(goldenPath));

    const size_t frameU16 = plane * 3;
    const size_t totalU16 = frameU16 * static_cast<size_t>(enc.durationFrames);
    std::vector<uint16_t> goldenYuv(totalU16);
    {
        std::ifstream gf(goldenPath, std::ios::binary);
        REQUIRE(gf.is_open());
        gf.read(reinterpret_cast<char *>(goldenYuv.data()),
                static_cast<std::streamsize>(totalU16 * 2));
        REQUIRE(gf.good());
    }

    for (int64_t k = 0; k < enc.durationFrames; k++) {
        const uint16_t *chdBase    = chdYuv.data()    + static_cast<size_t>(k) * frameU16;
        const uint16_t *goldenBase = goldenYuv.data() + static_cast<size_t>(k) * frameU16;
        auto checkPlane = [&](const char *name,
                              const uint16_t *a, const uint16_t *b) -> int {
            int maxDiff = 0;
            for (size_t i = 0; i < plane; i++) {
                int d = static_cast<int>(a[i]) - static_cast<int>(b[i]);
                if (d < 0) d = -d;
                if (d > maxDiff) maxDiff = d;
            }
            if (maxDiff != 0) {
                std::cerr << "FAIL: " << variant.label << " frame " << k
                          << " " << name << " plane drifted (max|diff|="
                          << maxDiff << ")\n";
                return 1;
            }
            return 0;
        };
        if (checkPlane("Y",  chdBase,             goldenBase))             return 1;
        if (checkPlane("Cb", chdBase + plane,     goldenBase + plane))     return 1;
        if (checkPlane("Cr", chdBase + 2 * plane, goldenBase + 2 * plane)) return 1;
    }
    std::cout << "    golden compare: bit-exact across "
              << enc.durationFrames << " frames\n";

    fs::remove_all(goldenDir);
    return 0;
}

// Build the encode-orc fixture for `enc`, then run every variant in
// `variants` against it (content check + optional golden compare).
template <size_t N>
int processEncoder(const std::string &encodeOrc, const std::string &assets,
                   const char *ldChroma,
                   const Encoder &enc, const Variant (&variants)[N]) {
    // Confirm chd's view of the produced fixture matches the encoder
    // metadata.
    const fs::path dir = fs::temp_directory_path() /
                         (std::string("chd_integration_") + enc.label);
    fs::remove_all(dir);

    std::string tbc, sidecar;
    if (buildEncodeOrcFixture(encodeOrc, assets, enc, dir, &tbc, &sidecar) != 0) return 1;
    {
        chd_video_t *v = nullptr;
        REQUIRE(chd_video_open_composite(tbc.c_str(), sidecar.c_str(), &v) == CHD_OK);
        chd_video_info_t info{};
        REQUIRE(chd_video_get_info(v, &info) == CHD_OK);
        REQUIRE(info.standard    == enc.expectedStandard);
        REQUIRE(info.field_width == enc.expectedFieldWidth);
        REQUIRE(info.num_frames  >= enc.durationFrames);
        chd_video_free(v);
    }

    std::cout << "test_integration: encoder " << enc.label
              << "  (" << enc.durationFrames << " frames)\n";

    for (size_t i = 0; i < N; i++) {
        if (runVariantChecks(enc, variants[i], tbc, sidecar, ldChroma) != 0) return 1;
    }

    fs::remove_all(dir);
    return 0;
}

}  // namespace

int main() {
    std::string encodeOrc, assets;
    const int rc = resolveEncodeOrc(kNtscBars, &encodeOrc, &assets);
    if (rc < 0) return 1;
    if (rc > 0) {
        std::cout << "test_integration: CHD_ENCODE_ORC unset — "
                     "skipping encode-orc integration\n";
        std::cout << "test_integration: PASS\n";
        return 0;
    }
    // PAL spec needs the same encode-orc binary but a different sub-asset;
    // confirm that one too before we start work.
    {
        std::string dummy1, dummy2;
        if (resolveEncodeOrc(kPalBars, &dummy1, &dummy2) < 0) return 1;
    }

    const char *ldChroma = std::getenv("CHD_LD_CHROMA_DECODER");
    if (ldChroma != nullptr && ldChroma[0] != 0 && !fs::exists(ldChroma)) {
        std::cerr << "FAIL: CHD_LD_CHROMA_DECODER=" << ldChroma
                  << " does not exist\n";
        return 1;
    }
    if (ldChroma == nullptr || ldChroma[0] == 0) {
        std::cout << "test_integration: CHD_LD_CHROMA_DECODER unset — "
                     "skipping golden-frame comparison\n";
        ldChroma = nullptr;  // sentinel for runVariantChecks
    }

    if (processEncoder(encodeOrc, assets, ldChroma, kNtscBars, kNtscVariants) != 0) return 1;
    if (processEncoder(encodeOrc, assets, ldChroma, kPalBars,  kPalVariants)  != 0) return 1;

    std::cout << "test_integration: PASS\n";
    return 0;
}
