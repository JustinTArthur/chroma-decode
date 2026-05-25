// SPDX-License-Identifier: GPL-3.0-or-later

#include "signal_state.h"

namespace chd::format {

static constexpr SignalStatePreset PRESETS[] = {
    { SignalState::STANDARD_TBC_LOCKED,       "STANDARD_TBC_LOCKED",       true,  true,  true  },
    { SignalState::STANDARD_TBC_UNLOCKED,     "STANDARD_TBC_UNLOCKED",     true,  true,  false },
    { SignalState::STANDARD_RAW,              "STANDARD_RAW",              true,  false, false },
    { SignalState::NONSTANDARD_TBC_LOCKED,    "NONSTANDARD_TBC_LOCKED",    false, true,  true  },
    { SignalState::NONSTANDARD_TBC_UNLOCKED,  "NONSTANDARD_TBC_UNLOCKED",  false, true,  false },
    { SignalState::NONSTANDARD_RAW,           "NONSTANDARD_RAW",           false, false, false },
};

const SignalStatePreset *findSignalStateByName(const std::string &name)
{
    for (const auto &preset : PRESETS) {
        if (name == preset.name) return &preset;
    }
    return nullptr;
}

const SignalStatePreset &getSignalState(SignalState state)
{
    return PRESETS[static_cast<size_t>(state)];
}

bool isDecoderCompatible(SignalState state)
{
    return getSignalState(state).tbcApplied;
}

}  // namespace chd::format
