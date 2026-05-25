// SPDX-License-Identifier: GPL-3.0-or-later
//
// CVBS dropout extension reader (`<basename>.dropouts.meta`).
//
// The CVBS spec defines this as an OPTIONAL sidecar (separate SQLite file
// with PRAGMA user_version = 5; see
// cvbs-file-format-specification/docs/extensions/dropout-extension-format.md).
// The schema is one row per contiguous dropout run, addressed by
// (cvbs_file_id, frame_id, sample_start, sample_count).
//
// The pre-decode corrector consumes the legacy per-field per-line shape
// (chd::metadata::DropOuts: startx, endx, fieldLine), so
// the recommended bridge is to transcode the spec's frame-relative sample
// ranges into the legacy in-memory shape at load time so the corrector has
// ONE code path. This reader does that transcoding.
//
// One row in the spec can straddle multiple lines (or even multiple fields)
// when the sample range crosses line boundaries. The transcoder slices the
// run at line boundaries and emits one legacy DropOuts entry per field-line
// segment, with `startx`/`endx` clamped to per-line bounds.

#ifndef CHD_METADATA_DROPOUT_EXTENSION_SQLITE_H
#define CHD_METADATA_DROPOUT_EXTENSION_SQLITE_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "dropouts.h"

namespace chd::metadata {

// One frame's per-field dropouts after transcoding. The spec's `frame_id` is
// 0-based; the legacy in-memory shape addresses fields not frames, so the
// reader splits a frame's dropouts into (first-field, second-field) using
// the standard's lines-per-field count.
struct FrameDropouts {
    int32_t frameIndex;     // 0-based, matches spec `frame_id`
    DropOuts firstField;
    DropOuts secondField;
};

// Read the dropout extension sidecar and transcode every row into the
// legacy DropOuts shape, grouped by frame. Returns nullopt and sets the
// thread-local error detail on failure (file unreadable, wrong schema
// version, malformed rows). An empty extension file is a valid return with
// an empty vector — the spec mandates consumers treat the extension as
// optional.
//
// `cvbsFileId`         — which capture's rows to load. The spec allows the
//                        sidecar to apply to a single CVBS basename, but
//                        the table still keys on cvbs_file_id; default 1
//                        per the spec's "implicit default" rule.
// `samplesPerLine`     — the standard's orthogonal line size (NTSC 910,
//                        PAL ~1135, PAL_M 909). Spec sample positions are
//                        frame-relative so the reader needs this to map
//                        them to (lineWithinFrame, xWithinLine).
// `linesPerField`      — number of lines in one field (NTSC 262/263 mean
//                        ≈ 262 or 263; use the per-standard table value).
//                        Used to assign each frame-line to either the
//                        first or second field, with lines numbered 1-based
//                        within each field per the legacy convention.
std::optional<std::vector<FrameDropouts>>
readCvbsDropoutsExtension(const std::string &metaPath,
                          int32_t cvbsFileId,
                          int32_t samplesPerLine,
                          int32_t linesPerField);

}  // namespace chd::metadata

#endif  // CHD_METADATA_DROPOUT_EXTENSION_SQLITE_H
