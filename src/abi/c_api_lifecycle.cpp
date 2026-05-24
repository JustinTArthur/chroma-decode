// SPDX-License-Identifier: GPL-3.0-or-later
#include <chromadec/version.h>
#include <chromadec/video.h>   // chd_init / chd_shutdown declarations
#include <cstring>

extern "C" {

chd_status_t chd_init(void)     { return CHD_OK; }
void         chd_shutdown(void) {}

const char *chd_version_string(void) {
    return CHROMADEC_VERSION_STRING;
}

void chd_version(int *major, int *minor, int *patch) {
    if (major) *major = CHROMADEC_VERSION_MAJOR;
    if (minor) *minor = CHROMADEC_VERSION_MINOR;
    if (patch) *patch = CHROMADEC_VERSION_PATCH;
}

int chd_has_feature(const char *feature) {
    if (!feature) return 0;
    /* Nothing wired up yet. Updated as features land. */
    (void)feature;
    return 0;
}

}  // extern "C"
