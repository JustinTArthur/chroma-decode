// SPDX-License-Identifier: GPL-3.0-or-later
#include <chromadec/errors.h>

#include "../common/error_state.h"

extern "C" {

const char *chd_status_str(chd_status_t s) {
    switch (s) {
        case CHD_OK:                        return "CHD_OK";
        case CHD_E_INVALID_ARG:             return "CHD_E_INVALID_ARG";
        case CHD_E_FILE_NOT_FOUND:          return "CHD_E_FILE_NOT_FOUND";
        case CHD_E_IO:                      return "CHD_E_IO";
        case CHD_E_FORMAT_UNSUPPORTED:      return "CHD_E_FORMAT_UNSUPPORTED";
        case CHD_E_METADATA_MISSING:        return "CHD_E_METADATA_MISSING";
        case CHD_E_METADATA_CORRUPT:        return "CHD_E_METADATA_CORRUPT";
        case CHD_E_PRESET_UNKNOWN:          return "CHD_E_PRESET_UNKNOWN";
        case CHD_E_DECODER_UNKNOWN:         return "CHD_E_DECODER_UNKNOWN";
        case CHD_E_DECODER_INCOMPATIBLE:    return "CHD_E_DECODER_INCOMPATIBLE";
        case CHD_E_NN_MODEL_LOAD:           return "CHD_E_NN_MODEL_LOAD";
        case CHD_E_NN_BACKEND_UNAVAILABLE:  return "CHD_E_NN_BACKEND_UNAVAILABLE";
        case CHD_E_NN_INFERENCE:            return "CHD_E_NN_INFERENCE";
        case CHD_E_OUT_OF_RANGE:            return "CHD_E_OUT_OF_RANGE";
        case CHD_E_CANCELLED:               return "CHD_E_CANCELLED";
        case CHD_E_INTERNAL:                return "CHD_E_INTERNAL";
        case CHD_E_OOM:                     return "CHD_E_OOM";
    }
    return "CHD_E_UNKNOWN";
}

const char *chd_last_error(void) {
    return chd::detail::get_last_error().c_str();
}

void chd_clear_last_error(void) {
    chd::detail::clear_last_error();
}

}  // extern "C"
