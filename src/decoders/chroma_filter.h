// SPDX-License-Identifier: GPL-3.0-or-later
//
// Cross-system chroma-filter intent (the chroma_filter option) and the single
// (intent, system) resolution table. A chroma decode is configured by two
// separate things: this enum is the *mode* (what to do with the band), shared
// across NTSC and PAL where applicable; the other is the upper-sideband cutoff
// +X (the numeric chroma_upper_sideband_hz option), which only the recovery
// modes read.
//
// resolveChromaFilter() is the one place the per-system validity and the
// standards-derived cutoffs live, consulted by both the commit-time validation
// (reject invalid cells) and the PAL decoder (read the cutoff). The NTSC comb
// selects a precomputed FIR by mode rather than a runtime cutoff, so for NTSC
// the cutoffHz here is informational; the validity/symmetry flags are universal.

#ifndef CHD_DECODERS_CHROMA_FILTER_H
#define CHD_DECODERS_CHROMA_FILTER_H

#include <optional>
#include <string>
#include <vector>

#include "../metadata/core.h"

namespace chd::decoders {

// Chroma-filter mode. compat resolves per system to the decoder's current
// default; equiband and color_under are standards-clean and shared; the two
// recovery modes are named by method because the algorithms differ (NTSC
// Hilbert SSB vs PAL amplitude EQ) and split by system.
enum class ChromaFilter {
    Compat,        // system-resolved legacy default (valid on every system)
    EquibandWide,  // NTSC only: ~2.2 MHz, the comb's loose legacy default
    Equiband,      // both: 1.3 MHz (SMPTE ST 170 Annex A.4 / ITU-R BT.1700)
    ColorUnder,    // both: ~0.5 MHz (VHS/S-VHS colour-under, IEC 60774-1 §6.2)
    WidebandISSB,  // NTSC only: NTSC-1953 wideband-I + Hilbert SSB recovery
    EquibandVsb,   // PAL only: equiband + vestigial-sideband amplitude recovery
};

struct ChromaFilterResolution {
    bool valid = false;
    // false ⇒ a recovery mode (wideband_i_ssb / equiband_vsb); these consume
    // the chroma_upper_sideband_hz geometry, the symmetric modes ignore it.
    bool symmetric = true;
    // Raised-cosine -6 dB corner in Hz. Consumed directly by the PALcolour 2D
    // filter; informational for the NTSC comb (which picks a precomputed FIR).
    // For a recovery mode this is the equiband baseband ceiling (1.3 MHz) the
    // recovery acts up to.
    double cutoffHz = 0.0;
    // Human-readable reason set when valid == false, for the commit error.
    const char *invalidReason = nullptr;
};

// Parse the chroma_filter option string to an intent. nullopt on an
// unrecognised string (commit rejects it with CHD_E_INVALID_ARG).
std::optional<ChromaFilter> parseChromaFilter(const std::string &name);

// The canonical option-string for an intent (for diagnostics / round-trips).
const char *chromaFilterName(ChromaFilter f);

// Resolve (intent, system) → validity + symmetry + cutoff. The single source
// of the per-system applicability matrix and the standards cutoffs.
ChromaFilterResolution resolveChromaFilter(ChromaFilter f, chd::metadata::VideoSystem system);

// The equiband baseband ceiling (Hz): the lower-sideband chroma tops out here
// for every system (the −1300 kHz row of BT.470-6 §2.12), so it is the cutoff
// the recovery modes pass up to and the upper bound of the equiband ladder.
inline constexpr double kEquibandCeilingHz = 1300000.0;

// The PAL / PAL-M compat/legacy chroma cutoff: PALcolour's 1.1/0.93
// dot-pattern-tuned value, shared by the resolution table and PalColour
// config default.
inline constexpr double kCompatPalChromaHz = 1100000.0 / 0.93;

// equiband_vsb vestige-recovery EQ (PAL). A PAL channel sends both U/V
// double-sideband to the ~1.3 MHz baseband ceiling, then clips the *upper*
// sideband at the video-band edge, leaving the band from the upper-sideband
// room +X up to the ceiling as lower-sideband-only (recovered at half amplitude
// by synchronous demodulation). The PAL line-alternating V already
// cancels the resulting U/V quadrature crosstalk, so all that remains is an
// amplitude EQ: unity below upperSidebandHz, a raised-cosine ramp to +6 dB
// (×2) at ceilingHz and held there above, with DC gain normalised to exactly 1
// so the bulk chroma is untouched. Returns a linear-phase (symmetric),
// odd-length, Hann-windowed FIR built for the given sample rate. (The NTSC
// counterpart is synthesizeSsbCorrections in beta_calibration.h, which differs
// because NTSC recovery is Hilbert SSB, not amplitude EQ.)
std::vector<double> synthesizeVsbEq(double upperSidebandHz, double ceilingHz, double sampleRate);

}  // namespace chd::decoders

#endif  // CHD_DECODERS_CHROMA_FILTER_H
