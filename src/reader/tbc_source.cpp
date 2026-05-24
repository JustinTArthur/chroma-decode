// SPDX-License-Identifier: GPL-3.0-or-later
/************************************************************************

    sourcevideo.cpp

    ld-decode-tools TBC library
    Copyright (C) 2018-2019 Simon Inns

    This file is part of ld-decode-tools.

    ld-decode-tools is free software: you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

************************************************************************/

#include "tbc_source.h"

#include <algorithm>
#include <stdexcept>

#include "../common/log.h"

namespace chd::reader {

// Class constructor
SourceVideo::SourceVideo()
{
    // Default object settings
    isSourceVideoOpen = false;
    inputFilePos = -1;
    availableFields = -1;
    fieldLength = -1;
    fieldByteLength = -1;
    fieldLineLength = -1;
}

SourceVideo::~SourceVideo()
{
    if (isSourceVideoOpen) inputFile.close();
}

// Source Video file manipulation methods -----------------------------------------------------------------------------

// Open an input video data file. If filename is "-", read from stdin.
// Returns true on success.
bool SourceVideo::open(std::string filename, int32_t _fieldLength, int32_t _fieldLineLength)
{
    fieldLength = _fieldLength;
    fieldByteLength = _fieldLength * 2;
    if (_fieldLineLength != -1) {
        fieldLineLength = _fieldLineLength * 2;
    } else fieldLineLength = -1;
    chd::log::debug() << "SourceVideo::open(): Called with field byte length =" << fieldByteLength;

    if (isSourceVideoOpen) {
        // Video file is already open, close it
        chd::log::info() << "A source video input file is already open, cannot open a new one";
        return false;
    }

    // Open the source video file
    if (filename == "-") {
        // Reading from stdin is not supported through std::ifstream; the
        // caller must use a pipe or a regular file path.
        chd::log::warn() << "Could not open stdin as source video input file";
        return false;
    } else {
        inputFile.open(filename, std::ios::binary);
        if (!inputFile.is_open()) {
            // Failed to open named input file
            chd::log::warn() << "Could not open" << filename << "as source video input file";
            return false;
        }

        // File open successful - configure source video parameters
        inputFile.seekg(0, std::ios::end);
        const int64_t fileSize = inputFile.tellg();
        inputFile.seekg(0, std::ios::beg);
        int64_t tAvailableFields = (fileSize / fieldByteLength);
        availableFields = static_cast<int32_t>(tAvailableFields);
        chd::log::debug() << "SourceVideo::open(): Successful -" << availableFields << "fields available";
    }

    // Initialise cache
    fieldCache.clear();

    isSourceVideoOpen = true;
    inputFilePos = 0;

    return true;
}

// Close an input video data file
void SourceVideo::close()
{
    if (!isSourceVideoOpen) {
        chd::log::debug() << "SourceVideo::close(): Called but no source video input file is open";
        return;
    }

    chd::log::debug() << "SourceVideo::close(): Called, closing the source video file and emptying the frame cache";
    inputFile.close();
    isSourceVideoOpen = false;
    inputFilePos = -1;

    chd::log::debug() << "SourceVideo::close(): Source video input file closed";
}

// Get the validity of the source video file
bool SourceVideo::isSourceValid()
{
    return isSourceVideoOpen;
}

// Get the number of fields available from the source video file.
// Returns -1 if the length is unknown (e.g. we're reading from stdin).
int32_t SourceVideo::getNumberOfAvailableFields()
{
    return availableFields;
}

// Get the number of samples in a field
int32_t SourceVideo::getFieldLength()
{
    return fieldLength;
}

// Frame data retrieval methods ---------------------------------------------------------------------------------------

// Method to retrieve a range of field lines from a single video field.
// If startFieldLine and endFieldLine are both -1, read the whole field.
SourceVideo::Data SourceVideo::getVideoField(int32_t fieldNumber, int32_t startFieldLine, int32_t endFieldLine)
{
    // Adjust the field number to index from zero
    fieldNumber--;

    // Ensure source video is open
    if (!isSourceVideoOpen) throw std::runtime_error("Application requested TBC field before opening TBC file - Fatal error");

    // Calculate the position of the require field line data
    int64_t requiredStartPosition = static_cast<int64_t>(fieldByteLength) * static_cast<int64_t>(fieldNumber);
    int64_t requiredReadLength;

    if (startFieldLine == -1 && endFieldLine == -1) {
        // Read the whole field

        // Check the cache (we only cache whole fields)
        auto it = fieldCache.find(fieldNumber);
        if (it != fieldCache.end()) {
            return it->second;
        }

        requiredReadLength = static_cast<int64_t>(fieldByteLength);
    } else {
        // Read a range of lines

        // Adjust the field line range to index from zero
        startFieldLine--;
        endFieldLine--;

        // Verify the required range
        if (fieldLineLength == -1) throw std::runtime_error("Application did not set field line length when opening TBC file");
        if (startFieldLine < 0) throw std::runtime_error("Application requested out-of-bounds field line");

        requiredStartPosition += static_cast<int64_t>(fieldLineLength) * static_cast<int64_t>(startFieldLine);
        requiredReadLength = static_cast<int64_t>(endFieldLine - startFieldLine + 1) * static_cast<int64_t>(fieldLineLength);
    }

    // Check the requested field and lines are valid
    if (availableFields != -1
        && (requiredStartPosition < 0
            || requiredStartPosition + requiredReadLength > (static_cast<int64_t>(fieldByteLength) * availableFields))) {
        throw std::runtime_error("Application requested field line range that exceeds the boundaries of the input TBC file");
    }

    // Resize the output buffer
    outputFieldData.resize(static_cast<int32_t>(requiredReadLength) / 2);

    // Seek to the correct file position (if not already there)
    if (inputFilePos != requiredStartPosition) {
        inputFile.clear();
        inputFile.seekg(requiredStartPosition);
        if (!inputFile.good()) {
            // Seek failed

            if (inputFilePos > requiredStartPosition) {
                throw std::runtime_error("Could not seek backwards to required field position in input TBC file");
            } else {
                // Seeking forwards -- try reading and discarding data instead
                inputFile.clear();
                int64_t discardBytes = requiredStartPosition - inputFilePos;
                while (discardBytes > 0) {
                    int64_t chunk = std::min(discardBytes, static_cast<int64_t>(outputFieldData.size() * 2));
                    inputFile.read(reinterpret_cast<char *>(outputFieldData.data()), chunk);
                    int64_t readBytes = inputFile.gcount();
                    if (readBytes <= 0) {
                        throw std::runtime_error("Could not seek or read forwards to required field position in input TBC file");
                    }
                    discardBytes -= readBytes;
                }
            }
        }
        inputFilePos = requiredStartPosition;
    }

    // Read the field lines from the input
    int64_t totalReceivedBytes = 0;
    int64_t receivedBytes = 0;
    do {
        inputFile.read(reinterpret_cast<char *>(outputFieldData.data()) + totalReceivedBytes,
                       requiredReadLength - totalReceivedBytes);
        receivedBytes = inputFile.gcount();
        if (receivedBytes > 0) {
            totalReceivedBytes += receivedBytes;
            inputFilePos += receivedBytes;
        }
    } while (receivedBytes > 0 && totalReceivedBytes < requiredReadLength);

    // Verify read was ok
    if (totalReceivedBytes != requiredReadLength) throw std::runtime_error("Could not read field data from input TBC file");

    if (startFieldLine == -1 && endFieldLine == -1) {
        // Insert the field data into the cache
        fieldCache.emplace(fieldNumber, outputFieldData);
    }

    // Return the data
    return outputFieldData;
}

}  // namespace chd::reader













