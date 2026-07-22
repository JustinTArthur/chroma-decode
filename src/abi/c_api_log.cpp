// SPDX-License-Identifier: GPL-3.0-or-later
#include <chromadec/log.h>

#include "../common/log.h"

extern "C" {

void chd_set_log_callback(chd_log_fn fn_or_null, void *user_data) {
    chd::log::setSink(fn_or_null, user_data);
}

void chd_log_to_stderr(void) {
    chd::log::installStderrSink();
}

void chd_set_log_level(chd_log_level_t min_level) {
    chd::log::setMinLevel(min_level);
}

chd_log_level_t chd_get_log_level(void) {
    return chd::log::minLevel();
}

int chd_log_is_enabled(chd_log_level_t level) {
    return chd::log::isEnabled(level) ? 1 : 0;
}

const char *chd_log_level_str(chd_log_level_t level) {
    switch (level) {
        case CHD_LOG_DEBUG: return "DEBUG";
        case CHD_LOG_INFO:  return "INFO";
        case CHD_LOG_WARN:  return "WARN";
        case CHD_LOG_ERROR: return "ERROR";
        case CHD_LOG_OFF:   return "OFF";
    }
    return "UNKNOWN";
}

}  // extern "C"