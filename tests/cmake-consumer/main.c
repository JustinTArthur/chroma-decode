/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Minimal consumer of the installed libchromadec C ABI. Calls a few exported
 * symbols so the link is real, prints the version, and reports a feature
 * probe. Returns non-zero if the library reports nonsense. */
#include <chromadec/chromadec.h>
#include <stdio.h>

int main(void)
{
    int major = -1, minor = -1, patch = -1;
    chd_version(&major, &minor, &patch);
    const char *version = chd_version_string();

    printf("libchromadec %d.%d.%d (%s); nn=%d fftw=%d sqlite=%d\n",
           major, minor, patch, version ? version : "(null)",
           chd_has_feature("nn"), chd_has_feature("fftw"),
           chd_has_feature("sqlite"));

    if (version == NULL || major < 0) {
        fprintf(stderr, "chromadec version probe failed\n");
        return 1;
    }
    return 0;
}