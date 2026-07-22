/******************************************************************************
 * sqliteio.cpp
 * ld-decode-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025 Simon Inns
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "ld_metadata_sqlite.h"

#include <sqlite3.h>

#include "../common/log.h"

namespace chd::metadata {

// SQL schema as per documentation
static const char *SCHEMA_SQL = R"(
PRAGMA user_version = 1;

CREATE TABLE IF NOT EXISTS capture (
    capture_id INTEGER PRIMARY KEY,
    system TEXT NOT NULL
        CHECK (system IN ('NTSC','PAL','PAL_M')),
    decoder TEXT NOT NULL
        CHECK (decoder IN ('ld-decode','vhs-decode')),
    git_branch TEXT,
    git_commit TEXT,

    video_sample_rate REAL,
    active_video_start INTEGER,
    active_video_end INTEGER,
    field_width INTEGER,
    field_height INTEGER,
    number_of_sequential_fields INTEGER,

    colour_burst_start INTEGER,
    colour_burst_end INTEGER,
    is_mapped INTEGER
        CHECK (is_mapped IN (0,1)),
    is_subcarrier_locked INTEGER
        CHECK (is_subcarrier_locked IN (0,1)),
    is_widescreen INTEGER
        CHECK (is_widescreen IN (0,1)),
    white_16b_ire INTEGER,
    black_16b_ire INTEGER,
    blanking_16b_ire INTEGER,

    capture_notes TEXT
);

CREATE TABLE IF NOT EXISTS pcm_audio_parameters (
    capture_id INTEGER PRIMARY KEY
        REFERENCES capture(capture_id) ON DELETE CASCADE,
    bits INTEGER,
    is_signed INTEGER
        CHECK (is_signed IN (0,1)),
    is_little_endian INTEGER
        CHECK (is_little_endian IN (0,1)),
    sample_rate REAL
);

CREATE TABLE IF NOT EXISTS field_record (
    capture_id INTEGER NOT NULL
        REFERENCES capture(capture_id) ON DELETE CASCADE,
    field_id INTEGER NOT NULL,
    audio_samples INTEGER,
    decode_faults INTEGER,
    disk_loc REAL,
    efm_t_values INTEGER,
    field_phase_id INTEGER,
    file_loc INTEGER,
    is_first_field INTEGER
        CHECK (is_first_field IN (0,1)),
    median_burst_ire REAL,
    pad INTEGER
        CHECK (pad IN (0,1)),
    sync_conf INTEGER,

    ntsc_is_fm_code_data_valid INTEGER
        CHECK (ntsc_is_fm_code_data_valid IN (0,1)),
    ntsc_fm_code_data INTEGER,
    ntsc_field_flag INTEGER
        CHECK (ntsc_field_flag IN (0,1)),
    ntsc_is_video_id_data_valid INTEGER
        CHECK (ntsc_is_video_id_data_valid IN (0,1)),
    ntsc_video_id_data INTEGER,
    ntsc_white_flag INTEGER
        CHECK (ntsc_white_flag IN (0,1)),

    PRIMARY KEY (capture_id, field_id)
);

CREATE TABLE IF NOT EXISTS vits_metrics (
    capture_id INTEGER NOT NULL,
    field_id INTEGER NOT NULL,
    w_snr REAL,
    b_psnr REAL,
    FOREIGN KEY (capture_id, field_id)
        REFERENCES field_record(capture_id, field_id) ON DELETE CASCADE,
    PRIMARY KEY (capture_id, field_id)
);

CREATE TABLE IF NOT EXISTS vbi (
    capture_id INTEGER NOT NULL,
    field_id INTEGER NOT NULL,
    vbi0 INTEGER,
    vbi1 INTEGER,
    vbi2 INTEGER,
    FOREIGN KEY (capture_id, field_id)
        REFERENCES field_record(capture_id, field_id) ON DELETE CASCADE,
    PRIMARY KEY (capture_id, field_id)
);

CREATE TABLE IF NOT EXISTS drop_outs (
    capture_id INTEGER NOT NULL,
    field_id INTEGER NOT NULL,
    startx INTEGER NOT NULL,
    endx INTEGER NOT NULL,
    field_line INTEGER NOT NULL,
    FOREIGN KEY (capture_id, field_id)
        REFERENCES field_record(capture_id, field_id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS vitc (
    capture_id INTEGER NOT NULL,
    field_id INTEGER NOT NULL,
    vitc0 INTEGER,
    vitc1 INTEGER,
    vitc2 INTEGER,
    vitc3 INTEGER,
    vitc4 INTEGER,
    vitc5 INTEGER,
    vitc6 INTEGER,
    vitc7 INTEGER,
    FOREIGN KEY (capture_id, field_id)
        REFERENCES field_record(capture_id, field_id) ON DELETE CASCADE,
    PRIMARY KEY (capture_id, field_id)
);

CREATE TABLE IF NOT EXISTS closed_caption (
    capture_id INTEGER NOT NULL,
    field_id INTEGER NOT NULL,
    data0 INTEGER,
    data1 INTEGER,
    FOREIGN KEY (capture_id, field_id)
        REFERENCES field_record(capture_id, field_id) ON DELETE CASCADE,
    PRIMARY KEY (capture_id, field_id)
);
)";

SqliteReader::SqliteReader(const std::string &fileName)
{
    // Open the database in read-only mode; each instance owns its own handle
    // so cross-thread coordination is the caller's responsibility.
    const int rc = db.open(fileName, SQLITE_OPEN_READONLY);
    if (rc != SQLITE_OK) {
        const std::string msg = db.isOpen() ? db.errmsg() : "unknown error";
        throwError("Failed to open database: " + msg);
    }
}

SqliteReader::~SqliteReader() = default;

void SqliteReader::close()
{
    db.close();
}

bool SqliteReader::readCaptureMetadata(int &captureId, std::string &system, std::string &decoder,
                                     std::string &gitBranch, std::string &gitCommit,
                                     double &videoSampleRate, int &activeVideoStart, int &activeVideoEnd,
                                     int &fieldWidth, int &fieldHeight, int &numberOfSequentialFields,
                                     int &colourBurstStart, int &colourBurstEnd,
                                     bool &isMapped, bool &isSubcarrierLocked, bool &isWidescreen,
                                     int &white16bIre, int &black16bIre, int &blanking16bIre, std::string &captureNotes,
                                     int &firstActiveFieldLine, int &lastActiveFieldLine,
                                     int &firstActiveFrameLine, int &lastActiveFrameLine)
{
    // Detect optional columns via a single PRAGMA table_info pass.
    // blanking_16b_ire was added upstream in ld-decode commit 1e4a33db (2025);
    // the four active-line columns were added by tbc-tools at v4 (commit a0f45b0).
    // We accept files with or without any of these.
    bool hasBlankingColumn = false;
    bool hasFirstActiveFieldLine = false;
    bool hasLastActiveFieldLine = false;
    bool hasFirstActiveFrameLine = false;
    bool hasLastActiveFrameLine = false;
    {
        SqliteQuery checkQuery(db);
        checkQuery.prepare("PRAGMA table_info(capture)");
        if (checkQuery.exec()) {
            while (checkQuery.next()) {
                const std::string columnName = checkQuery.value("name").toString();
                if      (columnName == "blanking_16b_ire")        hasBlankingColumn        = true;
                else if (columnName == "first_active_field_line") hasFirstActiveFieldLine  = true;
                else if (columnName == "last_active_field_line")  hasLastActiveFieldLine   = true;
                else if (columnName == "first_active_frame_line") hasFirstActiveFrameLine  = true;
                else if (columnName == "last_active_frame_line")  hasLastActiveFrameLine   = true;
            }
        }
    }

    SqliteQuery query(db);
    std::string queryStr = "SELECT capture_id, system, decoder, git_branch, git_commit, "
                       "video_sample_rate, active_video_start, active_video_end, "
                       "field_width, field_height, number_of_sequential_fields, "
                       "colour_burst_start, colour_burst_end, is_mapped, is_subcarrier_locked, "
                       "is_widescreen, white_16b_ire, black_16b_ire";
    if (hasBlankingColumn)        queryStr += ", blanking_16b_ire";
    if (hasFirstActiveFieldLine)  queryStr += ", first_active_field_line";
    if (hasLastActiveFieldLine)   queryStr += ", last_active_field_line";
    if (hasFirstActiveFrameLine)  queryStr += ", first_active_frame_line";
    if (hasLastActiveFrameLine)   queryStr += ", last_active_frame_line";
    queryStr += ", capture_notes FROM capture LIMIT 1";

    query.prepare(queryStr);

    if (!query.exec()) {
        chd::log::fail() << "Failed to execute capture metadata query:" << query.lastError().text();
        return false;
    }

    if (!query.next()) {
        chd::log::fail() << "No capture metadata found in database - capture table may be empty";
        return false;
    }

    captureId = query.value("capture_id").toInt();
    system = query.value("system").toString();
    decoder = query.value("decoder").toString();
    gitBranch = query.value("git_branch").toString();
    gitCommit = query.value("git_commit").toString();
    videoSampleRate = SqliteValue::toDoubleOrDefault(query, "video_sample_rate");
    activeVideoStart = SqliteValue::toIntOrDefault(query, "active_video_start");
    activeVideoEnd = SqliteValue::toIntOrDefault(query, "active_video_end");
    fieldWidth = SqliteValue::toIntOrDefault(query, "field_width");
    fieldHeight = SqliteValue::toIntOrDefault(query, "field_height");
    numberOfSequentialFields = SqliteValue::toIntOrDefault(query, "number_of_sequential_fields");
    colourBurstStart = SqliteValue::toIntOrDefault(query, "colour_burst_start");
    colourBurstEnd = SqliteValue::toIntOrDefault(query, "colour_burst_end");
    isMapped = SqliteValue::toBoolOrDefault(query, "is_mapped");
    isSubcarrierLocked = SqliteValue::toBoolOrDefault(query, "is_subcarrier_locked");
    isWidescreen = SqliteValue::toBoolOrDefault(query, "is_widescreen");
    white16bIre = SqliteValue::toIntOrDefault(query, "white_16b_ire");
    black16bIre = SqliteValue::toIntOrDefault(query, "black_16b_ire");

    // Handle blanking_16b_ire field (may not exist in old metadata files)
    if (hasBlankingColumn) {
        blanking16bIre = SqliteValue::toIntOrDefault(query, "blanking_16b_ire");
    } else {
        chd::log::warn() << "blanking_16b_ire field not found in metadata - using black_16b_ire value";
        blanking16bIre = black16bIre;
    }

    // Active-line range overrides — tbc-tools v4+. -1 signals "absent" so
    // the caller leaves the standard default in place.
    //
    // tbc-tools writes the *last* active field/frame line as an EXCLUSIVE bound
    // (one past the last active line). We carry both inclusively, so translate
    // those two columns to our scheme on ingest: a real value becomes value-1;
    // the -1 "absent" sentinel passes through. The *first* active field/frame
    // line is inclusive in both schemes and is not adjusted.
    firstActiveFieldLine = hasFirstActiveFieldLine ? SqliteValue::toIntOrDefault(query, "first_active_field_line", -1) : -1;
    if (hasLastActiveFieldLine) {
        const int exclusiveLast = SqliteValue::toIntOrDefault(query, "last_active_field_line", -1);
        lastActiveFieldLine = (exclusiveLast > 0) ? (exclusiveLast - 1) : -1;
    } else {
        lastActiveFieldLine = -1;
    }
    firstActiveFrameLine = hasFirstActiveFrameLine ? SqliteValue::toIntOrDefault(query, "first_active_frame_line", -1) : -1;
    if (hasLastActiveFrameLine) {
        const int exclusiveLast = SqliteValue::toIntOrDefault(query, "last_active_frame_line", -1);
        lastActiveFrameLine = (exclusiveLast > 0) ? (exclusiveLast - 1) : -1;
    } else {
        lastActiveFrameLine = -1;
    }

    captureNotes = query.value("capture_notes").toString();

    return true;
}

bool SqliteReader::readPcmAudioParameters(int captureId, int &bits, bool &isSigned,
                                        bool &isLittleEndian, double &sampleRate)
{
    SqliteQuery query(db);
    query.prepare("SELECT bits, is_signed, is_little_endian, sample_rate "
                 "FROM pcm_audio_parameters WHERE capture_id = ?");
    query.addBindValue(captureId);

    if (!query.exec() || !query.next()) {
        return false;
    }

    bits = SqliteValue::toIntOrDefault(query, "bits");
    isSigned = SqliteValue::toBoolOrDefault(query, "is_signed");
    isLittleEndian = SqliteValue::toBoolOrDefault(query, "is_little_endian");
    sampleRate = SqliteValue::toDoubleOrDefault(query, "sample_rate");

    return true;
}

bool SqliteReader::readFields(int captureId, SqliteQuery &fieldsQuery)
{
    fieldsQuery = SqliteQuery(db);
    fieldsQuery.prepare("SELECT field_id, audio_samples, decode_faults, disk_loc, "
                       "efm_t_values, field_phase_id, file_loc, is_first_field, "
                       "median_burst_ire, pad, sync_conf, ntsc_is_fm_code_data_valid, "
                       "ntsc_fm_code_data, ntsc_field_flag, ntsc_is_video_id_data_valid, "
                       "ntsc_video_id_data, ntsc_white_flag "
                       "FROM field_record WHERE capture_id = ? ORDER BY field_id");
    fieldsQuery.addBindValue(captureId);

    return fieldsQuery.exec();
}

bool SqliteReader::readFieldVitsMetrics(int captureId, int fieldId, double &wSnr, double &bPsnr)
{
    SqliteQuery query(db);
    query.prepare("SELECT w_snr, b_psnr FROM vits_metrics WHERE capture_id = ? AND field_id = ?");
    query.addBindValue(captureId);
    query.addBindValue(fieldId);

    if (!query.exec() || !query.next()) {
        return false;
    }

    wSnr = query.value("w_snr").toDouble();
    bPsnr = query.value("b_psnr").toDouble();

    return true;
}

bool SqliteReader::readFieldVbi(int captureId, int fieldId, int &vbi0, int &vbi1, int &vbi2)
{
    SqliteQuery query(db);
    query.prepare("SELECT vbi0, vbi1, vbi2 FROM vbi WHERE capture_id = ? AND field_id = ?");
    query.addBindValue(captureId);
    query.addBindValue(fieldId);

    if (!query.exec() || !query.next()) {
        return false;
    }

    vbi0 = query.value("vbi0").toInt();
    vbi1 = query.value("vbi1").toInt();
    vbi2 = query.value("vbi2").toInt();

    return true;
}

bool SqliteReader::readFieldVitc(int captureId, int fieldId, int vitcData[8])
{
    SqliteQuery query(db);
    query.prepare("SELECT vitc0, vitc1, vitc2, vitc3, vitc4, vitc5, vitc6, vitc7 "
                 "FROM vitc WHERE capture_id = ? AND field_id = ?");
    query.addBindValue(captureId);
    query.addBindValue(fieldId);

    if (!query.exec() || !query.next()) {
        return false;
    }

    vitcData[0] = query.value("vitc0").toInt();
    vitcData[1] = query.value("vitc1").toInt();
    vitcData[2] = query.value("vitc2").toInt();
    vitcData[3] = query.value("vitc3").toInt();
    vitcData[4] = query.value("vitc4").toInt();
    vitcData[5] = query.value("vitc5").toInt();
    vitcData[6] = query.value("vitc6").toInt();
    vitcData[7] = query.value("vitc7").toInt();

    return true;
}

bool SqliteReader::readFieldClosedCaption(int captureId, int fieldId, int &data0, int &data1)
{
    SqliteQuery query(db);
    query.prepare("SELECT data0, data1 FROM closed_caption WHERE capture_id = ? AND field_id = ?");
    query.addBindValue(captureId);
    query.addBindValue(fieldId);

    if (!query.exec() || !query.next()) {
        return false;
    }

    data0 = query.value("data0").toInt();
    data1 = query.value("data1").toInt();

    return true;
}

bool SqliteReader::readFieldDropouts(int captureId, int fieldId, SqliteQuery &dropoutsQuery)
{
    dropoutsQuery = SqliteQuery(db);
    dropoutsQuery.prepare("SELECT startx, endx, field_line FROM drop_outs "
                         "WHERE capture_id = ? AND field_id = ? ORDER BY startx");
    dropoutsQuery.addBindValue(captureId);
    dropoutsQuery.addBindValue(fieldId);

    return dropoutsQuery.exec();
}

// Optimized bulk read methods for better performance
bool SqliteReader::readAllFieldVitsMetrics(int captureId, SqliteQuery &vitsQuery)
{
    vitsQuery = SqliteQuery(db);
    vitsQuery.prepare("SELECT field_id, w_snr, b_psnr FROM vits_metrics "
                     "WHERE capture_id = ? ORDER BY field_id");
    vitsQuery.addBindValue(captureId);
    return vitsQuery.exec();
}

bool SqliteReader::readAllFieldVbi(int captureId, SqliteQuery &vbiQuery)
{
    vbiQuery = SqliteQuery(db);
    vbiQuery.prepare("SELECT field_id, vbi0, vbi1, vbi2 FROM vbi "
                    "WHERE capture_id = ? ORDER BY field_id");
    vbiQuery.addBindValue(captureId);
    return vbiQuery.exec();
}

bool SqliteReader::readAllFieldVitc(int captureId, SqliteQuery &vitcQuery)
{
    vitcQuery = SqliteQuery(db);
    vitcQuery.prepare("SELECT field_id, vitc0, vitc1, vitc2, vitc3, vitc4, vitc5, vitc6, vitc7 FROM vitc "
                     "WHERE capture_id = ? ORDER BY field_id");
    vitcQuery.addBindValue(captureId);
    return vitcQuery.exec();
}

bool SqliteReader::readAllFieldClosedCaptions(int captureId, SqliteQuery &ccQuery)
{
    ccQuery = SqliteQuery(db);
    ccQuery.prepare("SELECT field_id, data0, data1 FROM closed_caption "
                   "WHERE capture_id = ? ORDER BY field_id");
    ccQuery.addBindValue(captureId);
    return ccQuery.exec();
}

bool SqliteReader::readAllFieldDropouts(int captureId, SqliteQuery &dropoutsQuery)
{
    dropoutsQuery = SqliteQuery(db);
    dropoutsQuery.prepare("SELECT field_id, startx, endx, field_line FROM drop_outs "
                         "WHERE capture_id = ? ORDER BY field_id, startx");
    dropoutsQuery.addBindValue(captureId);
    return dropoutsQuery.exec();
}

SqliteWriter::SqliteWriter(const std::string &fileName)
{
    // Open the database read-write; create if missing. Each instance owns its
    // own handle, so writer/reader coordination is the caller's responsibility.
    const int rc = db.open(fileName, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    if (rc != SQLITE_OK) {
        const std::string msg = db.isOpen() ? db.errmsg() : "unknown error";
        throwError("Failed to open database: " + msg);
    }
    // Enable foreign keys for cascade-delete semantics in the schema.
    db.exec("PRAGMA foreign_keys = ON;");
}

SqliteWriter::~SqliteWriter() = default;

void SqliteWriter::close()
{
    db.close();
}

bool SqliteWriter::createSchema()
{
    chd::log::debug() << "SqliteWriter::createSchema(): Starting schema creation";

    // Run the entire schema script in one call; sqlite3_exec handles multiple
    // statements separated by ';'.
    char *errMsg = nullptr;
    const int rc = db.exec(SCHEMA_SQL, &errMsg);
    if (rc != SQLITE_OK) {
        chd::log::fail() << "Failed to execute schema:" << (errMsg ? errMsg : "(no message)");
        sqlite3_free(errMsg);
        return false;
    }

    chd::log::debug() << "Schema creation completed successfully";
    return true;
}

int SqliteWriter::writeCaptureMetadata(const std::string &system, const std::string &decoder,
                                     const std::string &gitBranch, const std::string &gitCommit,
                                     double videoSampleRate, int activeVideoStart, int activeVideoEnd,
                                     int fieldWidth, int fieldHeight, int numberOfSequentialFields,
                                     int colourBurstStart, int colourBurstEnd,
                                     bool isMapped, bool isSubcarrierLocked, bool isWidescreen,
                                     int white16bIre, int black16bIre, int blanking16bIre, const std::string &captureNotes)
{
    SqliteQuery query(db);
    query.prepare("INSERT INTO capture (system, decoder, git_branch, git_commit, "
                 "video_sample_rate, active_video_start, active_video_end, "
                 "field_width, field_height, number_of_sequential_fields, "
                 "colour_burst_start, colour_burst_end, is_mapped, is_subcarrier_locked, "
                 "is_widescreen, white_16b_ire, black_16b_ire, blanking_16b_ire, capture_notes) "
                 "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");

    query.addBindValue(system);
    query.addBindValue(decoder);
    query.addBindValueOrNull(gitBranch);
    query.addBindValueOrNull(gitCommit);
    query.addBindValue(videoSampleRate);
    query.addBindValue(activeVideoStart);
    query.addBindValue(activeVideoEnd);
    query.addBindValue(fieldWidth);
    query.addBindValue(fieldHeight);
    query.addBindValue(numberOfSequentialFields);
    query.addBindValue(colourBurstStart);
    query.addBindValue(colourBurstEnd);
    query.addBindValue(isMapped ? 1 : 0);
    query.addBindValue(isSubcarrierLocked ? 1 : 0);
    query.addBindValue(isWidescreen ? 1 : 0);
    query.addBindValue(white16bIre);
    query.addBindValue(black16bIre);
    query.addBindValue(blanking16bIre);
    query.addBindValueOrNull(captureNotes);

    if (!query.exec()) {
        chd::log::debug() << "Failed to insert capture metadata:" << query.lastError().text();
        return -1;
    }

    return query.lastInsertId().toInt();
}

bool SqliteWriter::updateCaptureMetadata(int captureId, const std::string &system, const std::string &decoder,
                                       const std::string &gitBranch, const std::string &gitCommit,
                                       double videoSampleRate, int activeVideoStart, int activeVideoEnd,
                                       int fieldWidth, int fieldHeight, int numberOfSequentialFields,
                                       int colourBurstStart, int colourBurstEnd,
                                       bool isMapped, bool isSubcarrierLocked, bool isWidescreen,
                                       int white16bIre, int black16bIre, int blanking16bIre, const std::string &captureNotes)
{
    SqliteQuery query(db);
    query.prepare("UPDATE capture SET system=?, decoder=?, git_branch=?, git_commit=?, "
                 "video_sample_rate=?, active_video_start=?, active_video_end=?, "
                 "field_width=?, field_height=?, number_of_sequential_fields=?, "
                 "colour_burst_start=?, colour_burst_end=?, is_mapped=?, is_subcarrier_locked=?, "
                 "is_widescreen=?, white_16b_ire=?, black_16b_ire=?, blanking_16b_ire=?, capture_notes=? "
                 "WHERE capture_id=?");

    query.addBindValue(system);
    query.addBindValue(decoder);
    query.addBindValueOrNull(gitBranch);
    query.addBindValueOrNull(gitCommit);
    query.addBindValue(videoSampleRate);
    query.addBindValue(activeVideoStart);
    query.addBindValue(activeVideoEnd);
    query.addBindValue(fieldWidth);
    query.addBindValue(fieldHeight);
    query.addBindValue(numberOfSequentialFields);
    query.addBindValue(colourBurstStart);
    query.addBindValue(colourBurstEnd);
    query.addBindValue(isMapped ? 1 : 0);
    query.addBindValue(isSubcarrierLocked ? 1 : 0);
    query.addBindValue(isWidescreen ? 1 : 0);
    query.addBindValue(white16bIre);
    query.addBindValue(black16bIre);
    query.addBindValue(blanking16bIre);
    query.addBindValueOrNull(captureNotes);
    query.addBindValue(captureId);

    if (!query.exec()) {
        chd::log::debug() << "Failed to update capture metadata:" << query.lastError().text();
        return false;
    }

    return true;
}

bool SqliteWriter::writePcmAudioParameters(int captureId, int bits, bool isSigned,
                                         bool isLittleEndian, double sampleRate)
{
    SqliteQuery query(db);
    query.prepare("INSERT OR REPLACE INTO pcm_audio_parameters (capture_id, bits, is_signed, "
                 "is_little_endian, sample_rate) VALUES (?, ?, ?, ?, ?)");

    query.addBindValue(captureId);
    query.addBindValue(bits);
    query.addBindValue(isSigned ? 1 : 0);
    query.addBindValue(isLittleEndian ? 1 : 0);
    query.addBindValue(sampleRate);

    if (!query.exec()) {
        chd::log::debug() << "Failed to insert PCM audio parameters:" << query.lastError().text();
        return false;
    }

    return true;
}

bool SqliteWriter::writeField(int captureId, int fieldId, int audioSamples, int decodeFaults,
                            double diskLoc, int efmTValues, int fieldPhaseId, int fileLoc,
                            bool isFirstField, double medianBurstIre, bool pad, int syncConf,
                            bool ntscIsFmCodeDataValid, int ntscFmCodeData, bool ntscFieldFlag,
                            bool ntscIsVideoIdDataValid, int ntscVideoIdData, bool ntscWhiteFlag)
{
    SqliteQuery query(db);
    query.prepare("INSERT OR REPLACE INTO field_record (capture_id, field_id, audio_samples, decode_faults, "
                 "disk_loc, efm_t_values, field_phase_id, file_loc, is_first_field, "
                 "median_burst_ire, pad, sync_conf, ntsc_is_fm_code_data_valid, "
                 "ntsc_fm_code_data, ntsc_field_flag, ntsc_is_video_id_data_valid, "
                 "ntsc_video_id_data, ntsc_white_flag) "
                 "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");

    query.addBindValue(captureId);
    query.addBindValue(fieldId);
    query.addBindValue(audioSamples);
    query.addBindValue(decodeFaults);
    query.addBindValue(diskLoc);
    query.addBindValue(efmTValues);
    query.addBindValue(fieldPhaseId);
    query.addBindValue(fileLoc);
    query.addBindValue(isFirstField ? 1 : 0);
    query.addBindValue(medianBurstIre);
    query.addBindValue(pad ? 1 : 0);
    query.addBindValue(syncConf);
    query.addBindValue(ntscIsFmCodeDataValid ? 1 : 0);
    query.addBindValue(ntscFmCodeData);
    query.addBindValue(ntscFieldFlag ? 1 : 0);
    query.addBindValue(ntscIsVideoIdDataValid ? 1 : 0);
    query.addBindValue(ntscVideoIdData);
    query.addBindValue(ntscWhiteFlag ? 1 : 0);

    if (!query.exec()) {
        chd::log::debug() << "Failed to insert field record:" << query.lastError().text();
        return false;
    }

    return true;
}

bool SqliteWriter::writeFieldVitsMetrics(int captureId, int fieldId, double wSnr, double bPsnr)
{
    SqliteQuery query(db);
    query.prepare("INSERT OR REPLACE INTO vits_metrics (capture_id, field_id, w_snr, b_psnr) VALUES (?, ?, ?, ?)");

    query.addBindValue(captureId);
    query.addBindValue(fieldId);
    query.addBindValue(wSnr);
    query.addBindValue(bPsnr);

    if (!query.exec()) {
        chd::log::debug() << "Failed to insert VITS metrics:" << query.lastError().text();
        return false;
    }

    return true;
}

bool SqliteWriter::writeFieldVbi(int captureId, int fieldId, int vbi0, int vbi1, int vbi2)
{
    SqliteQuery query(db);
    query.prepare("INSERT OR REPLACE INTO vbi (capture_id, field_id, vbi0, vbi1, vbi2) VALUES (?, ?, ?, ?, ?)");

    query.addBindValue(captureId);
    query.addBindValue(fieldId);
    query.addBindValue(vbi0);
    query.addBindValue(vbi1);
    query.addBindValue(vbi2);

    if (!query.exec()) {
        chd::log::debug() << "Failed to insert VBI:" << query.lastError().text();
        return false;
    }

    return true;
}

bool SqliteWriter::writeFieldVitc(int captureId, int fieldId, const int vitcData[8])
{
    SqliteQuery query(db);
    query.prepare("INSERT OR REPLACE INTO vitc (capture_id, field_id, vitc0, vitc1, vitc2, vitc3, "
                 "vitc4, vitc5, vitc6, vitc7) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");

    query.addBindValue(captureId);
    query.addBindValue(fieldId);
    for (int i = 0; i < 8; i++) {
        query.addBindValue(vitcData[i]);
    }

    if (!query.exec()) {
        chd::log::debug() << "Failed to insert VITC:" << query.lastError().text();
        return false;
    }

    return true;
}

bool SqliteWriter::writeFieldClosedCaption(int captureId, int fieldId, int data0, int data1)
{
    SqliteQuery query(db);
    query.prepare("INSERT OR REPLACE INTO closed_caption (capture_id, field_id, data0, data1) VALUES (?, ?, ?, ?)");

    query.addBindValue(captureId);
    query.addBindValue(fieldId);
    query.addBindValue(data0);
    query.addBindValue(data1);

    if (!query.exec()) {
        chd::log::debug() << "Failed to insert closed caption:" << query.lastError().text();
        return false;
    }

    return true;
}

bool SqliteWriter::writeFieldDropouts(int captureId, int fieldId, int startx, int endx, int fieldLine)
{
    SqliteQuery query(db);
    query.prepare("INSERT OR REPLACE INTO drop_outs (capture_id, field_id, startx, endx, field_line) VALUES (?, ?, ?, ?, ?)");

    query.addBindValue(captureId);
    query.addBindValue(fieldId);
    query.addBindValue(startx);
    query.addBindValue(endx);
    query.addBindValue(fieldLine);

    if (!query.exec()) {
        chd::log::debug() << "Failed to insert dropout:" << query.lastError().text();
        return false;
    }

    return true;
}

bool SqliteWriter::beginTransaction()
{
    return db.exec("BEGIN") == SQLITE_OK;
}

bool SqliteWriter::commitTransaction()
{
    return db.exec("COMMIT") == SQLITE_OK;
}

bool SqliteWriter::rollbackTransaction()
{
    return db.exec("ROLLBACK") == SQLITE_OK;
}

}  // namespace chd::metadata
