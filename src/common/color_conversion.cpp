// SPDX-License-Identifier: GPL-3.0-or-later

#include "color_conversion.h"

namespace chd::color {

namespace {

// Composite U/V reduction factors: U = uReduction*(E'B - E'Y) and
// V = vReduction*(E'R - E'Y).
struct BroadcastScaling {
    double uReduction;
    double vReduction;
};

BroadcastScaling resolveBroadcastScaling(BroadcastScalingPrecision precision)
{
    switch (precision) {
        case BroadcastScalingPrecision::Classic:
            return {0.493, 0.877};
        case BroadcastScalingPrecision::Modern:
            return {0.492111, 0.877283};
        case BroadcastScalingPrecision::Scientific:
        default:
            // sqrt(209556997 / 96146491) / 3 and sqrt(221990474 / 288439473).
            return {0.49211104112248356308804691718185,
                    0.87728321993817866838972487283129};
    }
}

}  // namespace

ColorConversion resolveColorConversion(ColorDifferencePrecision colorDifference,
                               BroadcastScalingPrecision broadcastScaling)
{
    ColorConversion m{};

    switch (colorDifference) {
        case ColorDifferencePrecision::Classic:
            m.kr = 0.30;
            m.kb = 0.11;
            break;
        case ColorDifferencePrecision::Modern:
        default:
            m.kr = 0.299;
            m.kb = 0.114;
            break;
    }
    m.kg = 1.0 - m.kr - m.kb;

    const BroadcastScaling scaling = resolveBroadcastScaling(broadcastScaling);
    m.uReduction = scaling.uReduction;
    m.vReduction = scaling.vReduction;

    m.blueDifferenceScale = 2.0 * (1.0 - m.kb);
    m.redDifferenceScale  = 2.0 * (1.0 - m.kr);

    m.ecbFromU = 1.0 / (m.blueDifferenceScale * m.uReduction);
    m.ecrFromV = 1.0 / (m.redDifferenceScale  * m.vReduction);

    m.rFromV =  1.0 / m.vReduction;
    m.gFromU = -(m.kb / m.kg) / m.uReduction;
    m.gFromV = -(m.kr / m.kg) / m.vReduction;
    m.bFromU =  1.0 / m.uReduction;

    return m;
}

std::optional<ColorDifferencePrecision> parseColorDifferencePrecision(const std::string &name)
{
    if (name == "classic") return ColorDifferencePrecision::Classic;
    if (name == "modern")  return ColorDifferencePrecision::Modern;
    return std::nullopt;
}

std::optional<BroadcastScalingPrecision> parseBroadcastScalingPrecision(const std::string &name)
{
    if (name == "classic")    return BroadcastScalingPrecision::Classic;
    if (name == "modern")     return BroadcastScalingPrecision::Modern;
    if (name == "scientific") return BroadcastScalingPrecision::Scientific;
    return std::nullopt;
}

const char *colorDifferencePrecisionName(ColorDifferencePrecision precision)
{
    switch (precision) {
        case ColorDifferencePrecision::Classic: return "classic";
        case ColorDifferencePrecision::Modern:  return "modern";
    }
    return "modern";
}

const char *broadcastScalingPrecisionName(BroadcastScalingPrecision precision)
{
    switch (precision) {
        case BroadcastScalingPrecision::Classic:    return "classic";
        case BroadcastScalingPrecision::Modern:     return "modern";
        case BroadcastScalingPrecision::Scientific: return "scientific";
    }
    return "scientific";
}

}  // namespace chd::color