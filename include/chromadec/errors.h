/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CHROMADEC_ERRORS_H
#define CHROMADEC_ERRORS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum chd_status {
    CHD_OK                         = 0,
    CHD_E_INVALID_ARG              = 1,
    CHD_E_FILE_NOT_FOUND           = 2,
    CHD_E_IO                       = 3,
    CHD_E_FORMAT_UNSUPPORTED       = 4,
    CHD_E_METADATA_MISSING         = 5,
    CHD_E_METADATA_CORRUPT         = 6,
    CHD_E_PRESET_UNKNOWN           = 7,
    CHD_E_DECODER_UNKNOWN          = 8,
    CHD_E_DECODER_INCOMPATIBLE     = 9,
    CHD_E_NN_MODEL_LOAD            = 10,
    CHD_E_NN_PROVIDER_UNAVAILABLE  = 11,
    CHD_E_NN_INFERENCE             = 12,
    CHD_E_OUT_OF_RANGE             = 13,
    CHD_E_CANCELLED                = 14,
    CHD_E_INTERNAL                 = 15,
    CHD_E_OOM                      = 16
} chd_status_t;

const char *chd_status_str(chd_status_t s);
const char *chd_last_error(void);
void        chd_clear_last_error(void);

#ifdef __cplusplus
}
#endif
#endif
