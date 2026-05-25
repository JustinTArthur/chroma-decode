// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sample Encoding Preset definitions, per the CVBS file format specification
// (cvbs-file-format-specification/docs/sample-encoding-presets.md).
//
// A Sample Encoding Preset defines the physical word format and amplitude
// mapping of every sample in a CVBS file: integer width/signedness, byte
// order, and the relationship between stored values and analogue signal
// levels.
//
// The library's decoder pipeline always sees samples in the canonical TBC
// convention: uint16_t per sample, with the underlying 10-bit value scaled
// ×64 (i.e. blanking ≡ 256·64 = 16384). This header defines per-encoding
// conversion that maps the file's on-disk word into that canonical domain.

#ifndef CHD_FORMAT_SAMPLE_ENCODING_H
#define CHD_FORMAT_SAMPLE_ENCODING_H

#include <cstdint>
#include <string>

namespace chd::format {

// Identifies the Sample Encoding Preset. Names match the CVBS spec's
// `sample_encoding_preset` column values.
enum class SampleEncoding {
    CVBS_U10_4FSC = 0,   // signed int16, 10-bit values 0..1023 stored directly
    CVBS_U16_4FSC,       // unsigned int16, 10-bit values shifted left 6 bits (×64)
    RAW_S16_28M,         // raw ADC at ~28.6 MHz, signed int16 full range
    RAW_S16_40M,         // raw ADC at 40 MHz, signed int16 full range
    CVBS_TPG21_4FSC,     // signed int16 with TPG21 offset: int16 = (val10 - 508) × 64
};

struct SampleEncodingPreset {
    SampleEncoding encoding;
    const char *name;

    // Byte width of one on-disk sample word. All current encodings are 2.
    int32_t bytesPerSample;

    // Whether the on-disk integer is signed (CVBS_U10_4FSC, CVBS_TPG21_4FSC,
    // RAW_S16_*) or unsigned (CVBS_U16_4FSC).
    bool isSigned;

    // True if the encoding has a defined amplitude mapping to the Video
    // Standard Preset's level table. RAW_S16_28M / RAW_S16_40M do not, so
    // decoders cannot interpret their level compliance.
    bool hasStandardAmplitudeMapping;
};

// Look up by name (uppercase ASCII, exact match). Returns nullptr if
// unrecognised; an unrecognised preset MUST NOT be silently interpreted
// (spec §4.2).
const SampleEncodingPreset *findSampleEncodingByName(const std::string &name);

// Look up by enum.
const SampleEncodingPreset &getSampleEncoding(SampleEncoding encoding);

// Convert one on-disk sample word into the canonical decoder-pipeline domain
// (uint16_t, 10-bit value × 64, blanking at 256·64 = 16384). Raw encodings
// (RAW_S16_28M, RAW_S16_40M) pass through unchanged — the decoders will
// reject them via signal-state checking.
//
// For luma samples in YC files, the same function is used (luma uses the
// same level table as composite per the spec).
uint16_t convertCompositeSampleToCanonical(SampleEncoding encoding, int16_t raw);

// Convert one on-disk chroma sample word (only meaningful for .c files in
// CVBS_U10_4FSC / CVBS_U16_4FSC / CVBS_TPG21_4FSC encodings) into the
// canonical decoder-pipeline domain *as a signed excursion around zero,
// scaled to ×64*.
//
// Chroma in YC files is centred at 10-bit value 512: the spec says
// "Chroma excursion is represented as oscillation around this centre point"
// (sample-encoding-presets.md). The returned int16_t is the centred 10-bit
// value × 64.
int16_t convertChromaSampleToCenteredCanonical(SampleEncoding encoding, int16_t raw);

}  // namespace chd::format

#endif  // CHD_FORMAT_SAMPLE_ENCODING_H
