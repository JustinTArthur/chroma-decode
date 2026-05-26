/******************************************************************************
 * ld_metadata_json.cpp
 * ld-decode-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2018-2025 Simon Inns
 * SPDX-FileCopyrightText: 2022 Ryan Holtz
 * SPDX-FileCopyrightText: 2022-2023 Adam Sampson
 *
 * This file is part of ld-decode-tools.
 *
 * Per-struct JSON read methods + the LdDecodeMetaData::readJsonImpl
 * dispatcher. Ported by inspection from the JSON-era ld-decode
 * `tools/library/tbc/lddecodemetadata.cpp` (commit df39ad1d^), which was
 * removed upstream when metadata storage migrated to SQLite. We keep JSON
 * back-compat for legacy `.tbc.json` sidecars; the parser itself
 * (JsonReader) lives in json_io.cpp and walks back to Adam Sampson via the
 * structural-move chain. The schema matches the legacy ld-decode JSON
 * output exactly so existing `.tbc.json` files from old captures still
 * load.
 ******************************************************************************/

#include "core.h"

#include "../common/log.h"
#include "dropouts.h"
#include "json_io.h"

#include <fstream>

namespace chd::metadata {

// Read VBI from JSON
void LdDecodeMetaData::Vbi::read(JsonReader &reader)
{
    reader.beginObject();

    std::string member;
    while (reader.readMember(member)) {
        if (member == "vbiData") {
            reader.beginArray();

            // There should be exactly 3 values, but handle more or less
            unsigned int i = 0;
            while (reader.readElement()) {
                int value;
                reader.read(value);

                if (i < vbiData.size()) vbiData[i++] = value;
            }
            while (i < vbiData.size()) vbiData[i++] = 0;

            reader.endArray();
        } else {
            reader.discard();
        }
    }

    reader.endObject();

    inUse = true;
}

// Read VideoParameters from JSON
void LdDecodeMetaData::VideoParameters::read(JsonReader &reader)
{
    bool isSourcePal = false;
    std::string systemString = "";

    reader.beginObject();

    std::string member;
    while (reader.readMember(member)) {
        if (member == "activeVideoEnd") reader.read(activeVideoEnd);
        else if (member == "activeVideoStart") reader.read(activeVideoStart);
        else if (member == "black16bIre") reader.read(black16bIre);
        else if (member == "blanking16bIre") reader.read(blanking16bIre);
        else if (member == "colourBurstEnd") reader.read(colourBurstEnd);
        else if (member == "colourBurstStart") reader.read(colourBurstStart);
        else if (member == "fieldHeight") reader.read(fieldHeight);
        else if (member == "fieldWidth") reader.read(fieldWidth);
        else if (member == "gitBranch") reader.read(gitBranch);
        else if (member == "gitCommit") reader.read(gitCommit);
        else if (member == "isMapped") reader.read(isMapped);
        else if (member == "isSourcePal") reader.read(isSourcePal); // obsolete
        else if (member == "isSubcarrierLocked") reader.read(isSubcarrierLocked);
        else if (member == "isWidescreen") reader.read(isWidescreen);
        else if (member == "numberOfSequentialFields") reader.read(numberOfSequentialFields);
        else if (member == "sampleRate") reader.read(sampleRate);
        else if (member == "system") reader.read(systemString);
        else if (member == "white16bIre") reader.read(white16bIre);
        else if (member == "tapeFormat") reader.read(tapeFormat);
        else reader.discard();
    }

    reader.endObject();

    // Work out which video system is being used
    if (systemString == "") {
        // Not specified -- detect based on isSourcePal and fieldHeight
        if (isSourcePal) {
            if (fieldHeight < 300) system = PAL_M;
            else system = PAL;
        } else system = NTSC;
    } else if (!parseVideoSystemName(systemString, system)) {
        reader.throwError("unknown value for videoParameters.system");
    }

    // blanking16bIre was added in 2025 (commit 1e4a33db). For older sidecars
    // that don't have it, default to black16bIre per upstream behaviour.
    if (blanking16bIre == -1) {
        blanking16bIre = black16bIre;
    }

    isValid = true;
}

// Read VitsMetrics from JSON
void LdDecodeMetaData::VitsMetrics::read(JsonReader &reader)
{
    reader.beginObject();

    std::string member;
    while (reader.readMember(member)) {
        if (member == "bPSNR") reader.read(bPSNR);
        else if (member == "wSNR") reader.read(wSNR);
        else reader.discard();
    }

    reader.endObject();

    inUse = true;
}

// Read Ntsc from JSON
void LdDecodeMetaData::Ntsc::read(JsonReader &reader, ClosedCaption &closedCaption)
{
    reader.beginObject();

    std::string member;
    while (reader.readMember(member)) {
        if (member == "isFmCodeDataValid") reader.read(isFmCodeDataValid);
        else if (member == "fmCodeData") reader.read(fmCodeData);
        else if (member == "fieldFlag") reader.read(fieldFlag);
        else if (member == "isVideoIdDataValid") reader.read(isVideoIdDataValid);
        else if (member == "videoIdData") reader.read(videoIdData);
        else if (member == "whiteFlag") reader.read(whiteFlag);
        else if (member == "ccData0") {
            // rev7 and earlier put ccData0/1 here rather than in cc
            reader.read(closedCaption.data0);
            closedCaption.inUse = true;
        } else if (member == "ccData1") {
            reader.read(closedCaption.data1);
            closedCaption.inUse = true;
        } else {
            reader.discard();
        }
    }

    reader.endObject();

    inUse = true;
}

// Read Vitc from JSON
void LdDecodeMetaData::Vitc::read(JsonReader &reader)
{
    reader.beginObject();

    std::string member;
    while (reader.readMember(member)) {
        if (member == "vitcData") {
            reader.beginArray();

            // There should be exactly 8 values, but handle more or less
            unsigned int i = 0;
            while (reader.readElement()) {
                int value;
                reader.read(value);

                if (i < vitcData.size()) vitcData[i++] = value;
            }
            while (i < vitcData.size()) vitcData[i++] = 0;

            reader.endArray();
        } else {
            reader.discard();
        }
    }

    reader.endObject();

    inUse = true;
}

// Read ClosedCaption from JSON
void LdDecodeMetaData::ClosedCaption::read(JsonReader &reader)
{
    reader.beginObject();

    std::string member;
    while (reader.readMember(member)) {
        if (member == "data0") reader.read(data0);
        else if (member == "data1") reader.read(data1);
        else reader.discard();
    }

    reader.endObject();

    inUse = true;
}

// Read PcmAudioParameters from JSON
void LdDecodeMetaData::PcmAudioParameters::read(JsonReader &reader)
{
    reader.beginObject();

    std::string member;
    while (reader.readMember(member)) {
        if (member == "bits") reader.read(bits);
        else if (member == "isLittleEndian") reader.read(isLittleEndian);
        else if (member == "isSigned") reader.read(isSigned);
        else if (member == "sampleRate") reader.read(sampleRate);
        else reader.discard();
    }

    reader.endObject();

    isValid = true;
}

// Read Field from JSON
void LdDecodeMetaData::Field::read(JsonReader &reader)
{
    reader.beginObject();

    std::string member;
    while (reader.readMember(member)) {
        if (member == "audioSamples") reader.read(audioSamples);
        else if (member == "cc") closedCaption.read(reader);
        else if (member == "decodeFaults") reader.read(decodeFaults);
        else if (member == "diskLoc") reader.read(diskLoc);
        else if (member == "dropOuts") dropOuts.read(reader);
        else if (member == "efmTValues") reader.read(efmTValues);
        else if (member == "fieldPhaseID") reader.read(fieldPhaseID);
        else if (member == "fileLoc") reader.read(fileLoc);
        else if (member == "isFirstField") reader.read(isFirstField);
        else if (member == "medianBurstIRE") reader.read(medianBurstIRE);
        else if (member == "ntsc") ntsc.read(reader, closedCaption);
        else if (member == "pad") reader.read(pad);
        else if (member == "seqNo") reader.read(seqNo);
        else if (member == "syncConf") reader.read(syncConf);
        else if (member == "vbi") vbi.read(reader);
        else if (member == "vitc") vitc.read(reader);
        else if (member == "vitsMetrics") vitsMetrics.read(reader);
        else reader.discard();
    }

    reader.endObject();
}

// Read all metadata from a JSON file. Walks the top-level
// { fields: [...], pcmAudioParameters: {...}, videoParameters: {...} }
// schema and dispatches each value to the per-struct reader.
bool LdDecodeMetaData::readJsonImpl(const std::string &fileName)
{
    std::ifstream jsonFile(fileName);
    if (jsonFile.fail()) {
        chd::log::error() << "Opening JSON input file failed:" << fileName;
        return false;
    }

    clear();

    JsonReader reader(jsonFile);

    try {
        reader.beginObject();

        std::string member;
        while (reader.readMember(member)) {
            if (member == "fields") {
                reader.beginArray();
                while (reader.readElement()) {
                    Field field;
                    field.read(reader);
                    fields.push_back(field);
                }
                reader.endArray();
            }
            else if (member == "pcmAudioParameters") pcmAudioParameters.read(reader);
            else if (member == "videoParameters") videoParameters.read(reader);
            else reader.discard();
        }

        reader.endObject();
    } catch (JsonReader::Error &error) {
        chd::log::error() << "Parsing JSON file failed:" << error.what();
        return false;
    }

    // Check we saw VideoParameters - if not, we can't do anything useful!
    if (!videoParameters.isValid) {
        chd::log::error() << "JSON file invalid: videoParameters object is not defined";
        return false;
    }

    // Check numberOfSequentialFields is consistent
    if (static_cast<size_t>(videoParameters.numberOfSequentialFields) != fields.size()) {
        chd::log::error() << "JSON file invalid: numberOfSequentialFields does not match fields array";
        return false;
    }

    // Now we know the video system, initialise the rest of VideoParameters
    initialiseVideoSystemParameters();

    // Generate the PCM audio map based on the field metadata
    generatePcmAudioMap();

    return true;
}

// Read DropOuts from JSON
void DropOuts::read(JsonReader &reader)
{
    std::vector<int32_t> startxArr, endxArr, fieldLineArr;
    auto readArray = [&reader](std::vector<int32_t> &array) {
        array.clear();
        reader.beginArray();
        while (reader.readElement()) {
            int32_t value;
            reader.read(value);
            array.push_back(value);
        }
        reader.endArray();
    };

    reader.beginObject();

    std::string member;
    while (reader.readMember(member)) {
        if (member == "endx") readArray(endxArr);
        else if (member == "fieldLine") readArray(fieldLineArr);
        else if (member == "startx") readArray(startxArr);
        else reader.discard();
    }

    if (endxArr.size() != fieldLineArr.size() || endxArr.size() != startxArr.size()) {
        reader.throwError("dropout array sizes do not match");
    }

    reader.endObject();

    clear();
    reserve(static_cast<int>(startxArr.size()));
    for (size_t i = 0; i < startxArr.size(); ++i) {
        append(startxArr[i], endxArr[i], fieldLineArr[i]);
    }
}

} // namespace chd::metadata
