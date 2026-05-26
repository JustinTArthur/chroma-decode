/******************************************************************************
 * sqliteio.h
 * ld-decode-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025 Simon Inns
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef CHD_METADATA_LD_METADATA_SQLITE_H
#define CHD_METADATA_LD_METADATA_SQLITE_H

#include <stdexcept>
#include <string>

#include "sqlite_query.h"

struct sqlite3;

namespace chd::metadata {

class SqliteReader
{
public:
    SqliteReader(const std::string &fileName);
    ~SqliteReader();
    
    // Explicitly close the database connection
    void close();

    // Exception class to be thrown when parsing fails
    class Error : public std::runtime_error
    {
    public:
        Error(std::string message) : std::runtime_error(message) {}
    };

    // Throw an Error exception with the given message
    [[noreturn]] void throwError(std::string message) {
        throw Error(message);
    }

    // Read capture-level metadata.
    //
    // The four trailing active-line outputs are populated only when the
    // corresponding columns exist in the capture table (tbc-tools v4+
    // schema bump, commit a0f45b0); they're set to -1 otherwise. -1
    // signals "no sidecar override" — the caller should leave whatever
    // standard default getSystemDefaults() supplies in place.
    bool readCaptureMetadata(int &captureId, std::string &system, std::string &decoder,
                           std::string &gitBranch, std::string &gitCommit,
                           double &videoSampleRate, int &activeVideoStart, int &activeVideoEnd,
                           int &fieldWidth, int &fieldHeight, int &numberOfSequentialFields,
                           int &colourBurstStart, int &colourBurstEnd,
                           bool &isMapped, bool &isSubcarrierLocked, bool &isWidescreen,
                           int &white16bIre, int &black16bIre, int &blanking16bIre, std::string &captureNotes,
                           int &firstActiveFieldLine, int &lastActiveFieldLine,
                           int &firstActiveFrameLine, int &lastActiveFrameLine);

    // Read PCM audio parameters
    bool readPcmAudioParameters(int captureId, int &bits, bool &isSigned,
                              bool &isLittleEndian, double &sampleRate);

    // Read field metadata
    bool readFields(int captureId, SqliteQuery &fieldsQuery);

    // Read field-specific data (individual queries - slower)
    bool readFieldVitsMetrics(int captureId, int fieldId, double &wSnr, double &bPsnr);
    bool readFieldVbi(int captureId, int fieldId, int &vbi0, int &vbi1, int &vbi2);
    bool readFieldVitc(int captureId, int fieldId, int vitcData[8]);
    bool readFieldClosedCaption(int captureId, int fieldId, int &data0, int &data1);
    bool readFieldDropouts(int captureId, int fieldId, SqliteQuery &dropoutsQuery);

    // Optimized bulk read methods for all fields (much faster)
    bool readAllFieldVitsMetrics(int captureId, SqliteQuery &vitsQuery);
    bool readAllFieldVbi(int captureId, SqliteQuery &vbiQuery);
    bool readAllFieldVitc(int captureId, SqliteQuery &vitcQuery);
    bool readAllFieldClosedCaptions(int captureId, SqliteQuery &ccQuery);
    bool readAllFieldDropouts(int captureId, SqliteQuery &dropoutsQuery);

private:
    sqlite3 *db = nullptr;
};

class SqliteWriter
{
public:
    SqliteWriter(const std::string &fileName);
    ~SqliteWriter();

    // Explicitly close the database connection
    void close();

    // Exception class to be thrown when writing fails
    class Error : public std::runtime_error
    {
    public:
        Error(std::string message) : std::runtime_error(message) {}
    };

    // Throw an Error exception with the given message
    [[noreturn]] void throwError(std::string message) {
        throw Error(message);
    }

    // Initialize database with schema
    bool createSchema();

    // Write capture-level metadata
    int writeCaptureMetadata(const std::string &system, const std::string &decoder,
                           const std::string &gitBranch, const std::string &gitCommit,
                           double videoSampleRate, int activeVideoStart, int activeVideoEnd,
                           int fieldWidth, int fieldHeight, int numberOfSequentialFields,
                           int colourBurstStart, int colourBurstEnd,
                           bool isMapped, bool isSubcarrierLocked, bool isWidescreen,
                           int white16bIre, int black16bIre, int blanking16bIre, const std::string &captureNotes);

    // Update existing capture metadata
    bool updateCaptureMetadata(int captureId, const std::string &system, const std::string &decoder,
                             const std::string &gitBranch, const std::string &gitCommit,
                             double videoSampleRate, int activeVideoStart, int activeVideoEnd,
                             int fieldWidth, int fieldHeight, int numberOfSequentialFields,
                             int colourBurstStart, int colourBurstEnd,
                             bool isMapped, bool isSubcarrierLocked, bool isWidescreen,
                             int white16bIre, int black16bIre, int blanking16bIre, const std::string &captureNotes);

    // Write PCM audio parameters
    bool writePcmAudioParameters(int captureId, int bits, bool isSigned,
                               bool isLittleEndian, double sampleRate);

    // Write field metadata
    bool writeField(int captureId, int fieldId, int audioSamples, int decodeFaults,
                   double diskLoc, int efmTValues, int fieldPhaseId, int fileLoc,
                   bool isFirstField, double medianBurstIre, bool pad, int syncConf,
                   bool ntscIsFmCodeDataValid, int ntscFmCodeData, bool ntscFieldFlag,
                   bool ntscIsVideoIdDataValid, int ntscVideoIdData, bool ntscWhiteFlag);

    // Write field-specific data
    bool writeFieldVitsMetrics(int captureId, int fieldId, double wSnr, double bPsnr);
    bool writeFieldVbi(int captureId, int fieldId, int vbi0, int vbi1, int vbi2);
    bool writeFieldVitc(int captureId, int fieldId, const int vitcData[8]);
    bool writeFieldClosedCaption(int captureId, int fieldId, int data0, int data1);
    bool writeFieldDropouts(int captureId, int fieldId, int startx, int endx, int fieldLine);

    // Transaction support
    bool beginTransaction();
    bool commitTransaction();
    bool rollbackTransaction();

private:
    sqlite3 *db = nullptr;
};

}  // namespace chd::metadata

#endif // CHD_METADATA_LD_METADATA_SQLITE_H