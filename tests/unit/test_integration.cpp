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
// Two fixture specs are exercised: one NTSC (CHD_DEC_NTSC_2D + ntsc2d) and
// one PAL (CHD_DEC_PAL_2D + pal2d). Multi-frame (duration=3) catches any
// per-frame state leakage that a single-frame test would mask.
//
// Env vars gate the integration paths:
//   CHD_ENCODE_ORC          — absolute path to a built encode-orc binary
//                             (integration-test prerequisite). Without it, every
//                             test self-skips with PASS so plain `meson
//                             test` stays green on machines that don't
//                             have the encoder.
//   CHD_ENCODE_ORC_ASSETS   — optional. Directory containing
//                             encode-orc's NTSC/PAL raw assets (the
//                             `525_5994_*.raw` and `625_50_*.raw`
//                             files). Defaults to the sibling `assets/`
//                             directory next to the encode-orc binary's
//                             parent build dir, i.e.
//                             `<dir-of-CHD_ENCODE_ORC>/../assets`.
//   CHD_LD_CHROMA_DECODER   — absolute path to a built ld-chroma-decoder
//                             binary, ideally at ld-decode commit
//                             f39e59e18 (the last good before the
//                             tools/ deletion in a4e403be). When set,
//                             the golden-frame comparison runs and
//                             asserts pixel-identical Y/Cb/Cr planes
//                             against the legacy reference across every
//                             frame of every fixture.

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

// One fixture configuration: chooses the encode-orc source asset +
// format, the chd decoder kind, and the legacy `-f` flag used for
// golden-frame comparison. Both NTSC and PAL drive 75 % colour bars so
// the chroma swing thresholds in the sanity check can stay constant
// across the two fixtures.
struct FixtureSpec {
    const char            *label;             // human-readable for logs
    const char            *encodeOrcFormat;   // "ntsc-composite" / "pal-composite"
    const char            *assetSubpath;      // relative to ENCODE_ORC_ASSETS
    chd_video_standard_t   expectedStandard;
    int32_t                expectedFieldWidth;
    chd_decoder_kind_t     chdKind;
    const char            *legacyDecoderFlag; // -f arg to ld-chroma-decoder
    int                    durationFrames;
};

const FixtureSpec kNtscSpec = {
    "ntsc-2d-bars",
    "ntsc-composite",
    "ntsc-raw/525_5994_75_BARS.raw",
    CHD_STD_NTSC,
    910,
    CHD_DEC_NTSC_2D,
    "ntsc2d",
    /*durationFrames=*/3,
};

const FixtureSpec kPalSpec = {
    "pal-2d-bars",
    "pal-composite",
    "pal-raw/625_50_75_BARS.raw",
    CHD_STD_PAL,
    1135,
    CHD_DEC_PAL_2D,
    "pal2d",
    /*durationFrames=*/3,
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

// Render the encode-orc project YAML for the given fixture. The
// ${ENCODE_ORC_*} placeholders are expanded by encode-orc itself when
// it parses the YAML — we set those vars on the invocation env below.
std::string renderProjectYaml(const FixtureSpec &spec) {
    std::ostringstream s;
    s << "name: \"chd-integration-" << spec.label << "\"\n"
      << "output:\n"
      << "  filename: \"${ENCODE_ORC_OUTPUT_ROOT}/fixture\"\n"
      << "  format: \"" << spec.encodeOrcFormat << "\"\n"
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
      << "    duration: " << spec.durationFrames << "\n"
      << "    source:\n"
      << "      type: \"yuv422-image\"\n"
      << "      file: \"${ENCODE_ORC_ASSETS}/" << spec.assetSubpath << "\"\n";
    return s.str();
}

// Run encode-orc against the given fixture spec. Returns the produced
// fixture paths via out params. Caller owns the temp dir cleanup.
int buildEncodeOrcFixture(const std::string &encodeOrc, const std::string &assets,
                          const FixtureSpec &spec, const fs::path &outDir,
                          std::string *tbcOut, std::string *sidecarOut) {
    fs::create_directories(outDir);
    const std::string yamlPath = (outDir / "project.yaml").string();
    REQUIRE(writeFile(yamlPath, renderProjectYaml(spec)));

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

// Resolve the encode-orc binary + assets dir from env. Returns:
//    0   run    — binaryOut/assetsOut populated
//    1   skip   — CHD_ENCODE_ORC unset; caller should `return 0`
//   -1   fail   — env set but file/dir doesn't exist; caller returns 1
int resolveEncodeOrc(const FixtureSpec &spec,
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
    const std::string assetFile = assets + "/" + spec.assetSubpath;
    if (!fs::exists(assetFile)) {
        std::cerr << "FAIL: missing asset " << assetFile
                  << " (set CHD_ENCODE_ORC_ASSETS to override)\n";
        return -1;
    }
    *binaryOut = encodeOrc;
    *assetsOut = assets;
    return 0;
}

// Decode `count` frames starting at `firstIdx` via the public C ABI.
// Each frame's Y|Cb|Cr planes are concatenated into `yuvOut`; total
// size is count × width × height × 3 × sizeof(uint16_t). Width/height
// (post-padding) are reported for the slice math the caller does after.
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
            // Every frame in a fixture must agree on dimensions — a
            // mismatch would mean OutputWriter padding state leaked
            // between frames, which would corrupt the output.
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
// indicating the carrier was actually demodulated to YUV. Both NTSC
// and PAL 75 % colour bars produce chroma swings well above ±5000 u16
// codes from C_ZERO=32768; the bar is set wide enough to survive
// future decoder tweaks but tight enough that a regression flattening
// chroma to neutral shows up immediately.
int assertChromaSwing(const FixtureSpec &spec, int64_t frameIdx,
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
    std::cout << "  " << spec.label << " frame " << frameIdx
              << "  Y=[" << yMin << "," << yMax << "]"
              << "  Cb=[" << cbMin << "," << cbMax << "]"
              << "  Cr=[" << crMin << "," << crMax << "]\n";
    return 0;
}

// Per-fixture chroma-content test: synthesize the fixture, decode all
// `durationFrames`, assert each frame's chroma planes carry real swing.
int runChromaContentCheck(const std::string &encodeOrc, const std::string &assets,
                          const FixtureSpec &spec) {
    const fs::path dir = fs::temp_directory_path() /
                         (std::string("chd_integration_chroma_") + spec.label);
    fs::remove_all(dir);

    std::string tbc, sidecar;
    if (buildEncodeOrcFixture(encodeOrc, assets, spec, dir, &tbc, &sidecar) != 0) return 1;

    // Confirm chd sees the expected video standard + field geometry up front.
    {
        chd_video_t *v = nullptr;
        REQUIRE(chd_video_open_composite(tbc.c_str(), sidecar.c_str(), &v) == CHD_OK);
        chd_video_info_t info{};
        REQUIRE(chd_video_get_info(v, &info) == CHD_OK);
        REQUIRE(info.standard    == spec.expectedStandard);
        REQUIRE(info.field_width == spec.expectedFieldWidth);
        REQUIRE(info.num_frames  >= spec.durationFrames);
        chd_video_free(v);
    }

    std::vector<uint16_t> yuv;
    int32_t w = 0, h = 0;
    if (decodeFramesViaAbi(tbc, sidecar, spec.chdKind, 0, spec.durationFrames,
                           &yuv, &w, &h) != 0) return 1;
    const size_t plane = static_cast<size_t>(w) * h;

    std::cout << "test_integration: " << spec.label
              << " " << w << "x" << h << " * " << spec.durationFrames << " frames\n";
    for (int64_t k = 0; k < spec.durationFrames; k++) {
        const uint16_t *base = yuv.data() + static_cast<size_t>(k) * plane * 3;
        if (assertChromaSwing(spec, k, base, base + plane, base + 2 * plane, plane) != 0) return 1;
    }

    fs::remove_all(dir);
    return 0;
}

// Per-fixture golden-frame compare: synthesize the fixture, decode
// every frame via chd, run the legacy ld-chroma-decoder against the
// same TBC with -l = durationFrames, diff Y/Cb/Cr planes frame by
// frame. Each frame must match bit-exactly.
int runGoldenCompare(const std::string &encodeOrc, const std::string &assets,
                     const std::string &ldChroma, const FixtureSpec &spec) {
    const fs::path dir = fs::temp_directory_path() /
                         (std::string("chd_integration_golden_") + spec.label);
    fs::remove_all(dir);

    std::string tbc, sidecar;
    if (buildEncodeOrcFixture(encodeOrc, assets, spec, dir, &tbc, &sidecar) != 0) return 1;

    const std::string goldenPath = (dir / "golden.yuv").string();
    std::ostringstream cmd;
    cmd << "\"" << ldChroma << "\""
        << " -f " << spec.legacyDecoderFlag
        << " -p yuv --pad 1 --quiet"
        << " -l " << spec.durationFrames
        << " --input-metadata \"" << sidecar << "\""
        << " \"" << tbc << "\""
        << " \"" << goldenPath << "\"";
    if (runShell(cmd.str()) != 0) {
        std::cerr << "FAIL: ld-chroma-decoder invocation failed: " << cmd.str() << "\n";
        return 1;
    }
    REQUIRE(fs::exists(goldenPath));

    std::vector<uint16_t> chdYuv;
    int32_t w = 0, h = 0;
    if (decodeFramesViaAbi(tbc, sidecar, spec.chdKind, 0, spec.durationFrames,
                           &chdYuv, &w, &h) != 0) return 1;
    const size_t plane = static_cast<size_t>(w) * h;
    const size_t frameU16 = plane * 3;
    const size_t totalU16 = frameU16 * static_cast<size_t>(spec.durationFrames);

    std::vector<uint16_t> goldenYuv(totalU16);
    {
        std::ifstream gf(goldenPath, std::ios::binary);
        REQUIRE(gf.is_open());
        gf.read(reinterpret_cast<char *>(goldenYuv.data()),
                static_cast<std::streamsize>(totalU16 * 2));
        REQUIRE(gf.good());
    }

    std::cout << "test_integration: " << spec.label
              << " golden compare (" << w << "x" << h << ", "
              << spec.durationFrames << " frames)\n";

    for (int64_t k = 0; k < spec.durationFrames; k++) {
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
                std::cerr << "FAIL: " << spec.label << " frame " << k
                          << " " << name << " plane drifted (max|diff|="
                          << maxDiff << ")\n";
                return 1;
            }
            return 0;
        };
        if (checkPlane("Y",  chdBase,             goldenBase))             return 1;
        if (checkPlane("Cb", chdBase + plane,     goldenBase + plane))     return 1;
        if (checkPlane("Cr", chdBase + 2 * plane, goldenBase + 2 * plane)) return 1;
        std::cout << "  frame " << k << ": bit-exact across all planes\n";
    }

    fs::remove_all(dir);
    return 0;
}

}  // namespace

int main() {
    // ── encode-orc → chd chroma-content checks ────────────────────────
    {
        std::string encodeOrc, assets;
        const int rc = resolveEncodeOrc(kNtscSpec, &encodeOrc, &assets);
        if (rc < 0) return 1;
        if (rc > 0) {
            std::cout << "test_integration: CHD_ENCODE_ORC unset — "
                         "skipping encode-orc integration\n";
            std::cout << "test_integration: PASS\n";
            return 0;
        }
        // From here on encode-orc is available; the PAL spec uses the
        // same binary + assets root with a different sub-asset.
        if (runChromaContentCheck(encodeOrc, assets, kNtscSpec) != 0) return 1;
        if (runChromaContentCheck(encodeOrc, assets, kPalSpec)  != 0) return 1;

        // ── golden-frame compare (optional second binary) ─────────────
        const char *ldChroma = std::getenv("CHD_LD_CHROMA_DECODER");
        if (ldChroma == nullptr || ldChroma[0] == 0) {
            std::cout << "test_integration: CHD_LD_CHROMA_DECODER unset — "
                         "skipping golden-frame comparison\n";
        } else {
            if (!fs::exists(ldChroma)) {
                std::cerr << "FAIL: CHD_LD_CHROMA_DECODER=" << ldChroma
                          << " does not exist\n";
                return 1;
            }
            if (runGoldenCompare(encodeOrc, assets, ldChroma, kNtscSpec) != 0) return 1;
            if (runGoldenCompare(encodeOrc, assets, ldChroma, kPalSpec)  != 0) return 1;
        }
    }

    std::cout << "test_integration: PASS\n";
    return 0;
}
