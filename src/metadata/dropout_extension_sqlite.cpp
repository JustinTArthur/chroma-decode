// SPDX-License-Identifier: GPL-3.0-or-later
//
// CVBS dropout extension reader implementation. See header for the
// transcoding shape; the algorithm collapses the
// spec's flat (frame, sample_start, sample_count) rows into the legacy
// per-field per-line DropOuts representation so the corrector body keeps
// one code path.

#include "dropout_extension_sqlite.h"

#include <sqlite3.h>

#include <map>

#include "../common/error_state.h"
#include "sqlite_query.h"

namespace chd::metadata {

namespace {

// Spec doc dropout-extension-format.md mandates user_version 5; older
// version 4 used the same shape but allowed a non-integer severity. We
// accept 5 only — older sidecars are rare in practice and refusing is
// safer than guessing column semantics.
constexpr int kExpectedUserVersion = 5;

const char *kSelectSql =
    "SELECT frame_id, sample_start, sample_count "
    "FROM dropout_run "
    "WHERE cvbs_file_id = ? "
    "ORDER BY frame_id, sample_start";

// Slice one frame-relative dropout run into legacy per-field per-line
// DropOuts entries, appending into the field-specific accumulators. Even
// frame lines go to the first field, odd to the second — both 1-based
// within the field. Lines outside the addressable frame are dropped on the
// floor (consumer requirement: "ignore rows whose ... fall outside known
// capture bounds").
void sliceRunIntoFieldDropouts(int32_t sampleStart, int32_t sampleCount,
                               int32_t samplesPerLine, int32_t linesPerField,
                               DropOuts &firstFieldOut, DropOuts &secondFieldOut)
{
    if (sampleCount <= 0 || samplesPerLine <= 0) return;
    int32_t sampleEnd = sampleStart + sampleCount;  // exclusive
    int32_t cursor = sampleStart;
    while (cursor < sampleEnd) {
        const int32_t frameLineIdx = cursor / samplesPerLine;   // 0-based
        const int32_t xWithinLine = cursor % samplesPerLine;
        // End sample for this slice — earliest of (run end) and (line end).
        const int32_t lineEndSample = (frameLineIdx + 1) * samplesPerLine;
        const int32_t sliceEnd = (sampleEnd < lineEndSample) ? sampleEnd : lineEndSample;
        const int32_t startx = xWithinLine;
        // Convert exclusive sample-index end to legacy DropOuts' endx
        // (also exclusive in chd::metadata::DropOuts semantics, matching
        // how the corrector iterates `for (pixel = startx; pixel < endx;)`).
        const int32_t endx = startx + (sliceEnd - cursor);

        const int32_t fieldIdx = frameLineIdx % 2;                // 0 or 1
        const int32_t lineWithinField = (frameLineIdx / 2) + 1;   // 1-based

        if (lineWithinField <= linesPerField) {
            if (fieldIdx == 0) {
                firstFieldOut.append(startx, endx, lineWithinField);
            } else {
                secondFieldOut.append(startx, endx, lineWithinField);
            }
        }

        cursor = sliceEnd;
    }
}

}  // namespace

std::optional<std::vector<FrameDropouts>>
readCvbsDropoutsExtension(const std::string &metaPath,
                          int32_t cvbsFileId,
                          int32_t samplesPerLine,
                          int32_t linesPerField)
{
    SqliteDb db;
    if (db.open(metaPath, SQLITE_OPEN_READONLY) != SQLITE_OK) {
        chd::detail::set_last_error("CVBS dropouts: could not open " + metaPath + ": " +
                                    db.errmsg());
        return std::nullopt;
    }

    int userVersion = -1;
    {
        sqlite3_stmt *vstmt = nullptr;
        if (sqlite3_prepare_v2(db.handle(), "PRAGMA user_version", -1, &vstmt, nullptr) == SQLITE_OK
            && sqlite3_step(vstmt) == SQLITE_ROW) {
            userVersion = sqlite3_column_int(vstmt, 0);
        }
        if (vstmt != nullptr) sqlite3_finalize(vstmt);
    }
    if (userVersion != kExpectedUserVersion) {
        chd::detail::set_last_error(
            "CVBS dropouts: user_version = " + std::to_string(userVersion) +
            " (expected " + std::to_string(kExpectedUserVersion) + ") in " + metaPath);
        return std::nullopt;
    }

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db.handle(), kSelectSql, -1, &stmt, nullptr) != SQLITE_OK) {
        chd::detail::set_last_error(std::string("CVBS dropouts: prepare failed: ") +
                                    db.errmsg());
        return std::nullopt;
    }
    sqlite3_bind_int(stmt, 1, cvbsFileId);

    // Per-frame accumulator. std::map keeps frames in ascending order so
    // the output vector is also ordered, useful for downstream binary
    // searches and matches the spec's ORDER BY frame_id, sample_start.
    std::map<int32_t, FrameDropouts> byFrame;

    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const int32_t frameId = sqlite3_column_int(stmt, 0);
        const int32_t sampleStart = sqlite3_column_int(stmt, 1);
        const int32_t sampleCount = sqlite3_column_int(stmt, 2);
        if (sampleCount <= 0) continue;     // spec: must be > 0; ignore malformed
        if (sampleStart < 0) continue;
        auto &frame = byFrame[frameId];
        frame.frameIndex = frameId;
        sliceRunIntoFieldDropouts(sampleStart, sampleCount, samplesPerLine,
                                  linesPerField, frame.firstField, frame.secondField);
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        chd::detail::set_last_error("CVBS dropouts: row iteration failed");
        return std::nullopt;
    }

    std::vector<FrameDropouts> out;
    out.reserve(byFrame.size());
    for (auto &kv : byFrame) {
        out.push_back(std::move(kv.second));
    }
    return out;
}

}  // namespace chd::metadata
