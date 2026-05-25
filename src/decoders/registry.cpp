// SPDX-License-Identifier: GPL-3.0-or-later
#include "registry.h"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "comb/comb.h"
#include "comb/ntsc_decoder.h"
#include "mono/mono_decoder.h"
#include "palcolour/pal_decoder.h"
#include "palcolour/palcolour.h"

#if defined(CHD_WITH_NN)
#include "ldzeug/ldzeug_color_cnn.h"
#include "ldzeug/ldzeug_luma_sep.h"
#include "../nn/ort_session.h"
#endif

namespace chd::decoders::registry {

namespace {

template <typename M>
auto findOr(const M &m, const std::string &key, typename M::mapped_type fallback) {
    auto it = m.find(key);
    return it == m.end() ? fallback : it->second;
}

// Common option names that apply to most decoders.
bool isLineLayoutOption(const std::string &name, OptionType type) {
    if (type != OptionType::I32) return false;
    return name == CHD_OPT_FIRST_ACTIVE_FIELD_LINE
        || name == CHD_OPT_LAST_ACTIVE_FIELD_LINE
        || name == CHD_OPT_FIRST_ACTIVE_FRAME_LINE
        || name == CHD_OPT_LAST_ACTIVE_FRAME_LINE;
}

bool isOutputOption(const std::string &name, OptionType type) {
    if (type == OptionType::I32 && (name == CHD_OPT_PADDING_MULTIPLE
                                    || name == CHD_OPT_THREAD_COUNT)) return true;
    if (type == OptionType::Str  && name == CHD_OPT_OUTPUT_FORMAT)       return true;
    if (type == OptionType::Bool && (name == CHD_OPT_OUTPUT_Y4M_HEADERS
                                    || name == CHD_OPT_REVERSE_FIELD_ORDER)) return true;
    return false;
}

bool isCombKind(chd_decoder_kind_t k) {
    return k == CHD_DEC_NTSC_1D || k == CHD_DEC_NTSC_2D
        || k == CHD_DEC_NTSC_3D || k == CHD_DEC_NTSC_3D_NO_ADAPT
        || k == CHD_DEC_NN_TRANSFORM3D;
}

bool isPalKind(chd_decoder_kind_t k) {
    return k == CHD_DEC_PAL_2D || k == CHD_DEC_TRANSFORM_2D || k == CHD_DEC_TRANSFORM_3D;
}

bool isLdzeugKind(chd_decoder_kind_t k) {
    return k == CHD_DEC_LDZEUG_COLOR_CNN
        || k == CHD_DEC_LDZEUG_LUMA_SEP
        || k == CHD_DEC_LDZEUG_LUMA_SEP_FRAME;
}

std::vector<double> readTransformThresholds(const std::string &path) {
    std::ifstream in(path);
    std::vector<double> out;
    if (!in.is_open()) return out;
    double v;
    while (in >> v) out.push_back(v);
    return out;
}

}  // namespace

chd_decoder_kind_t resolveAuto(chd_decoder_kind_t kind, chd::metadata::VideoSystem system) {
    if (kind != CHD_DEC_AUTO) return kind;
    switch (system) {
        case chd::metadata::NTSC:  return CHD_DEC_NTSC_2D;
        case chd::metadata::PAL:
        case chd::metadata::PAL_M: return CHD_DEC_PAL_2D;
    }
    return CHD_DEC_NTSC_2D;
}

bool kindUsesNn(chd_decoder_kind_t kind) {
    return kind == CHD_DEC_NN_TRANSFORM3D || isLdzeugKind(kind);
}

bool optionApplies(chd_decoder_kind_t kind, const std::string &name, OptionType type) {
    // Output + pipeline-shape options apply to every kind (including AUTO so
    // setters can land before commit picks a concrete kind).
    if (isOutputOption(name, type)) return true;
    if (isLineLayoutOption(name, type)) return true;

    const bool comb   = isCombKind(kind) || kind == CHD_DEC_AUTO;
    const bool pal    = isPalKind(kind)  || kind == CHD_DEC_AUTO;
    const bool ldzeug = isLdzeugKind(kind);
    const bool nn3d   = kind == CHD_DEC_NN_TRANSFORM3D;

    // Comb / PAL chroma-trim options.
    if (type == OptionType::F64 && name == CHD_OPT_CHROMA_GAIN)     return comb || pal || ldzeug;
    if (type == OptionType::F64 && name == CHD_OPT_CHROMA_PHASE_DEG) return comb || pal || ldzeug;
    if (type == OptionType::F64 && name == CHD_OPT_LUMA_NR_LEVEL) {
        return comb || pal || kind == CHD_DEC_MONO;
    }
    if (type == OptionType::F64 && name == CHD_OPT_CHROMA_NR_LEVEL) return comb;

    // Comb-only options.
    if (type == OptionType::Bool && name == CHD_OPT_PHASE_COMPENSATION) return comb;
    if (type == OptionType::I32  && name == CHD_OPT_COMB_DIMENSIONS)    return comb;
    if (type == OptionType::Bool && name == CHD_OPT_COMB_ADAPTIVE)      return comb;
    if (type == OptionType::F64  && name == CHD_OPT_COMB_ADAPT_THRESHOLD) return comb;
    if (type == OptionType::F64  && name == CHD_OPT_COMB_CHROMA_WEIGHT) return comb;
    if (type == OptionType::Bool && name == CHD_OPT_COMB_SHOW_MAP)      return comb;

    // Transform-PAL options.
    if (type == OptionType::F64 && name == CHD_OPT_TRANSFORM_THRESHOLD)
        return kind == CHD_DEC_TRANSFORM_2D || kind == CHD_DEC_TRANSFORM_3D;
    if (type == OptionType::Str && name == CHD_OPT_TRANSFORM_THRESHOLDS_FILE)
        return kind == CHD_DEC_TRANSFORM_2D || kind == CHD_DEC_TRANSFORM_3D;

    // NN-specific options.
    if (type == OptionType::F64  && name == CHD_OPT_NN_INPUT_MAGNITUDE_SCALE) return nn3d;
    if (type == OptionType::Bool && name == CHD_OPT_NN_CHROMA_BANDPASS)
        return kind == CHD_DEC_LDZEUG_LUMA_SEP || kind == CHD_DEC_LDZEUG_LUMA_SEP_FRAME;

    return false;
}

namespace {

void fillCombConfig(chd::decoders::comb::Comb::Configuration &c, const OptionMaps &o,
                    chd_decoder_kind_t kind) {
    c.chromaGain  = findOr(o.f64, CHD_OPT_CHROMA_GAIN, c.chromaGain);
    c.chromaPhase = findOr(o.f64, CHD_OPT_CHROMA_PHASE_DEG, c.chromaPhase);
    c.yNRLevel    = findOr(o.f64, CHD_OPT_LUMA_NR_LEVEL, c.yNRLevel);
    c.cNRLevel    = findOr(o.f64, CHD_OPT_CHROMA_NR_LEVEL, c.cNRLevel);
    c.adaptThreshold = findOr(o.f64, CHD_OPT_COMB_ADAPT_THRESHOLD, c.adaptThreshold);
    c.chromaWeight   = findOr(o.f64, CHD_OPT_COMB_CHROMA_WEIGHT, c.chromaWeight);
    c.adaptive   = findOr(o.boolean, CHD_OPT_COMB_ADAPTIVE, c.adaptive);
    c.showMap    = findOr(o.boolean, CHD_OPT_COMB_SHOW_MAP, c.showMap);
    c.phaseCompensation = findOr(o.boolean, CHD_OPT_PHASE_COMPENSATION, c.phaseCompensation);

    // Kind-specific defaults override the f64/i32 maps where the kind is
    // sufficiently prescriptive (e.g. NTSC_1D ⇒ dimensions = 1).
    switch (kind) {
        case CHD_DEC_NTSC_1D: c.dimensions = 1; c.adaptive = false; break;
        case CHD_DEC_NTSC_2D: c.dimensions = 2; c.adaptive = false; break;
        case CHD_DEC_NTSC_3D: c.dimensions = 3; c.adaptive = true;  break;
        case CHD_DEC_NTSC_3D_NO_ADAPT: c.dimensions = 3; c.adaptive = false; break;
        case CHD_DEC_NN_TRANSFORM3D:
            c.dimensions = 3;
            c.adaptive = false;
            c.nnTransform3D = true;
            c.nnInputMagnitudeScale =
                findOr(o.f64, CHD_OPT_NN_INPUT_MAGNITUDE_SCALE, c.nnInputMagnitudeScale);
            break;
        default:
            c.dimensions = findOr(o.i32, CHD_OPT_COMB_DIMENSIONS, c.dimensions);
            break;
    }
}

void fillPalConfig(chd::decoders::palcolour::PalColour::Configuration &c, const OptionMaps &o,
                   chd_decoder_kind_t kind) {
    c.chromaGain  = findOr(o.f64, CHD_OPT_CHROMA_GAIN, c.chromaGain);
    c.chromaPhase = findOr(o.f64, CHD_OPT_CHROMA_PHASE_DEG, c.chromaPhase);
    c.yNRLevel    = findOr(o.f64, CHD_OPT_LUMA_NR_LEVEL, c.yNRLevel);
    c.transformThreshold = findOr(o.f64, CHD_OPT_TRANSFORM_THRESHOLD, c.transformThreshold);

    const auto thresholdsFile = findOr(o.str, CHD_OPT_TRANSFORM_THRESHOLDS_FILE, std::string{});
    if (!thresholdsFile.empty()) {
        c.transformThresholds = readTransformThresholds(thresholdsFile);
    }

    switch (kind) {
        case CHD_DEC_PAL_2D:
            c.chromaFilter = chd::decoders::palcolour::PalColour::palColourFilter;
            break;
        case CHD_DEC_TRANSFORM_2D:
            c.chromaFilter = chd::decoders::palcolour::PalColour::transform2DFilter;
            break;
        case CHD_DEC_TRANSFORM_3D:
            c.chromaFilter = chd::decoders::palcolour::PalColour::transform3DFilter;
            break;
        default: break;
    }
}

}  // namespace

std::unique_ptr<chd::decoders::Decoder> build(chd_decoder_kind_t kind, const OptionMaps &opts) {
    switch (kind) {
        case CHD_DEC_MONO: {
            chd::decoders::mono::MonoDecoder::MonoConfiguration mc;
            mc.yNRLevel = findOr(opts.f64, CHD_OPT_LUMA_NR_LEVEL, mc.yNRLevel);
            return std::make_unique<chd::decoders::mono::MonoDecoder>(mc);
        }
        case CHD_DEC_NTSC_1D:
        case CHD_DEC_NTSC_2D:
        case CHD_DEC_NTSC_3D:
        case CHD_DEC_NTSC_3D_NO_ADAPT:
        case CHD_DEC_NN_TRANSFORM3D: {
            chd::decoders::comb::Comb::Configuration cc;
            fillCombConfig(cc, opts, kind);
            return std::make_unique<chd::decoders::comb::NtscDecoder>(cc);
        }
        case CHD_DEC_PAL_2D:
        case CHD_DEC_TRANSFORM_2D:
        case CHD_DEC_TRANSFORM_3D: {
            chd::decoders::palcolour::PalColour::Configuration pc;
            fillPalConfig(pc, opts, kind);
            return std::make_unique<chd::decoders::palcolour::PalDecoder>(pc);
        }
#if defined(CHD_WITH_NN)
        case CHD_DEC_LDZEUG_COLOR_CNN: {
            auto d = std::make_unique<chd::decoders::ldzeug::LdzeugColorCnnDecoder>();
            d->setMode(chd::decoders::ldzeug::LdzeugDecoderBase::Mode::Field);
            d->setChromaGain (findOr(opts.f64, CHD_OPT_CHROMA_GAIN, 1.0));
            d->setChromaPhase(findOr(opts.f64, CHD_OPT_CHROMA_PHASE_DEG, 0.0));
            return d;
        }
        case CHD_DEC_LDZEUG_LUMA_SEP:
        case CHD_DEC_LDZEUG_LUMA_SEP_FRAME: {
            auto d = std::make_unique<chd::decoders::ldzeug::LdzeugLumaSepDecoder>();
            d->setMode(kind == CHD_DEC_LDZEUG_LUMA_SEP_FRAME
                           ? chd::decoders::ldzeug::LdzeugDecoderBase::Mode::Frame
                           : chd::decoders::ldzeug::LdzeugDecoderBase::Mode::Field);
            d->setChromaGain (findOr(opts.f64, CHD_OPT_CHROMA_GAIN, 1.0));
            d->setChromaPhase(findOr(opts.f64, CHD_OPT_CHROMA_PHASE_DEG, 0.0));
            d->setChromaBandpass(findOr(opts.boolean, CHD_OPT_NN_CHROMA_BANDPASS, true));
            return d;
        }
#else
        case CHD_DEC_LDZEUG_COLOR_CNN:
        case CHD_DEC_LDZEUG_LUMA_SEP:
        case CHD_DEC_LDZEUG_LUMA_SEP_FRAME:
            return nullptr;  // NN disabled at build time
#endif
        case CHD_DEC_AUTO:
        default:
            return nullptr;
    }
}

#if defined(CHD_WITH_NN)
bool applyNnModel(chd_decoder_kind_t kind,
                  chd::decoders::Decoder &decoder,
                  std::shared_ptr<chd::nn::OrtSession> session) {
    if (!session) return true;  // nothing to bind is always OK
    if (kind == CHD_DEC_NN_TRANSFORM3D) {
        auto *nd = dynamic_cast<chd::decoders::comb::NtscDecoder *>(&decoder);
        if (!nd) return false;
        nd->setNnModel(std::move(session));
        return true;
    }
    if (kind == CHD_DEC_LDZEUG_COLOR_CNN
     || kind == CHD_DEC_LDZEUG_LUMA_SEP
     || kind == CHD_DEC_LDZEUG_LUMA_SEP_FRAME) {
        auto *ld = dynamic_cast<chd::decoders::ldzeug::LdzeugDecoderBase *>(&decoder);
        if (!ld) return false;
        ld->setNnModel(std::move(session));
        return true;
    }
    // Other kinds don't take a session; supplying one is a caller mistake.
    return false;
}
#endif

}  // namespace chd::decoders::registry
