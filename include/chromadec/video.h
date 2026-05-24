/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CHROMADEC_VIDEO_H
#define CHROMADEC_VIDEO_H

#include <chromadec/errors.h>
#include <chromadec/types.h>

#ifdef __cplusplus
extern "C" {
#endif

chd_status_t chd_init(void);
void         chd_shutdown(void);

/* Open a single-file composite capture: an ld-decode `.tbc` or a CVBS
 * `.composite`. sidecar_path_or_null:
 *   - NULL  → library auto-locates the sidecar next to path: an ld-decode
 *             `<path>.db` / `<path>.json`, else a CVBS `<basename>.meta`
 *   - explicit path to a `.db`, `.json`, or `.meta` sidecar
 * override_or_null:
 *   - NULL  → all parameters come from the sidecar
 *   - explicit override values used when no sidecar is found, or to override
 * The sidecar flavour (ld-decode vs CVBS) is detected automatically. */
chd_status_t chd_video_open_composite(const char *path,
                                      const char *sidecar_path_or_null,
                                      const chd_video_params_t *override_or_null,
                                      chd_video_t **out);

/* Open a dual-file Y/C capture: a CVBS `.y` + `.c` pair, or a vhs-decode
 * luma `.tbc` + chroma `.tbc` pair. The luma plane is decoded for Y and the
 * chroma plane for U/V, then merged. Sidecar resolution and flavour detection
 * follow chd_video_open_composite. sidecar_path_or_null applies to the luma
 * plane; the chroma plane uses its own sidecar if present, else the luma one
 * (vhs-decode writes a single shared `<base>.tbc.json` for the pair). */
chd_status_t chd_video_open_yc(const char *luma_path,
                               const char *chroma_path,
                               const char *sidecar_path_or_null,
                               const chd_video_params_t *override_or_null,
                               chd_video_t **out);

void         chd_video_free(chd_video_t *v);

chd_status_t chd_video_get_info(const chd_video_t *v, chd_video_info_t *out);

/* Extra sources for multi-source dropout correction. sidecar_path_or_null
 * follows the same resolution as the matching open function. */
chd_status_t chd_video_add_extra_source_composite(chd_video_t *v,
                                                  const char *path,
                                                  const char *sidecar_path_or_null);
chd_status_t chd_video_add_extra_source_yc(chd_video_t *v,
                                           const char *luma_path,
                                           const char *chroma_path,
                                           const char *sidecar_path_or_null);

#ifdef __cplusplus
}
#endif
#endif
