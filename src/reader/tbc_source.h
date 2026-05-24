// SPDX-License-Identifier: GPL-3.0-or-later
/************************************************************************

    sourcevideo.h

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

#ifndef CHD_READER_TBC_SOURCE_H
#define CHD_READER_TBC_SOURCE_H

#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace chd::reader {

class SourceVideo
{
public:
    // A vector of timebase-corrected video samples.
    // This is usually a complete field, but it may be a partial field if
    // you've requested fewer lines from getVideoField (or if you've sliced it
    // yourself).
    using Data = std::vector<uint16_t>;

    SourceVideo();
    ~SourceVideo();

    // Prevent copying or assignment
    SourceVideo(const SourceVideo &) = delete;
    SourceVideo& operator=(const SourceVideo &) = delete;

    // File handling methods
    bool open(std::string filename, int32_t _fieldLength, int32_t _fieldLineLength = -1);
    void close(void);

    // Field handling methods
    Data getVideoField(int32_t fieldNumber, int32_t startFieldLine = -1, int32_t endFieldLine = -1);

    // Get and set methods
    bool isSourceValid();
    int32_t getNumberOfAvailableFields();
    int32_t getFieldLength();

private:
    // File handling globals
    std::ifstream inputFile;
    int64_t inputFilePos;
    bool isSourceVideoOpen;
    int32_t availableFields;
    int32_t fieldLength;
    int32_t fieldByteLength;
    int32_t fieldLineLength;

    Data outputFieldData;

    // Field caching
    std::unordered_map<int32_t, Data> fieldCache;
};

}  // namespace chd::reader

#endif // CHD_READER_TBC_SOURCE_H
