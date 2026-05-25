// SPDX-License-Identifier: GPL-3.0-or-later
//
// Signal State Preset definitions, per the CVBS file format specification
// (cvbs-file-format-specification/docs/signal-state-presets.md).
//
// A Signal State Preset captures three independent axes of the signal's
// processing state at the time of storage:
//   - sample rate (standard 4×fsc vs. non-standard)
//   - TBC applied (yes vs. no)
//   - burst locked (yes vs. no)
//
// Together they govern whether normative sample-count constraints apply,
// whether signal level compliance is meaningful, and whether sample-domain
// dropout coordinates are stable.

#ifndef CHD_FORMAT_SIGNAL_STATE_H
#define CHD_FORMAT_SIGNAL_STATE_H

#include <string>

namespace chd::format {

enum class SignalState {
    STANDARD_TBC_LOCKED = 0,
    STANDARD_TBC_UNLOCKED,
    STANDARD_RAW,
    NONSTANDARD_TBC_LOCKED,
    NONSTANDARD_TBC_UNLOCKED,
    NONSTANDARD_RAW,
};

struct SignalStatePreset {
    SignalState state;
    const char *name;
    bool standardRate;     // true ⇒ exactly 4×fsc for the declared video standard
    bool tbcApplied;       // true ⇒ fixed samples/line, stable timing
    bool burstLocked;      // true ⇒ subcarrier phase known and stable
};

// Look up by name (uppercase ASCII, exact match). Returns nullptr if
// unrecognised; an unrecognised preset MUST NOT be silently interpreted
// (spec §4.2).
const SignalStatePreset *findSignalStateByName(const std::string &name);

// Look up by enum.
const SignalStatePreset &getSignalState(SignalState state);

// True if the decoder pipeline can meaningfully interpret a signal in this
// state. By design, decoders refuse raw-uncorrected input
// (STANDARD_RAW, NONSTANDARD_RAW). They can in principle handle the four
// _TBC_ states; non-standard sample rates require non-default decoder
// parameters but aren't structurally rejected here.
bool isDecoderCompatible(SignalState state);

}  // namespace chd::format

#endif  // CHD_FORMAT_SIGNAL_STATE_H
