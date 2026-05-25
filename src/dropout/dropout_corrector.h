// SPDX-License-Identifier: GPL-3.0-or-later
/************************************************************************

    dropoutcorrect.h

    ld-dropout-correct - Dropout correction for ld-decode
    Copyright (C) 2018-2020 Simon Inns
    Copyright (C) 2019-2020 Adam Sampson

    This file is part of ld-decode-tools.

    ld-dropout-correct is free software: you can redistribute it and/or
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

#ifndef CHD_DROPOUT_DROPOUT_CORRECTOR_H
#define CHD_DROPOUT_DROPOUT_CORRECTOR_H

#include <cstdint>
#include <vector>

#include "../decoders/source_field.h"
#include "../metadata/core.h"
#include "../reader/source.h"

namespace chd::dropout {

struct DropoutCorrectionStats {
    int corrected = 0;       // Dropout regions successfully replaced
    int failed = 0;          // Dropout regions where no replacement was found
    int64_t totalDistance = 0;  // Sum of spatial distances of all replacements
};

// Per-source frame data for multi-source correction.
// Each extra source provides its field data and metadata for one frame.
struct ExtraSourceFrame {
    chd::reader::Data firstFieldData;
    chd::reader::Data secondFieldData;
    chd::metadata::LdDecodeMetaData::Field firstFieldMeta;
    chd::metadata::LdDecodeMetaData::Field secondFieldMeta;
    chd::metadata::LdDecodeMetaData::VideoParameters videoParams;
    double quality = -1.0;  // Frame quality (average bPSNR of both fields)
};

class DropoutCorrector
{
public:
    explicit DropoutCorrector(const chd::metadata::LdDecodeMetaData::VideoParameters &videoParams);

    // Single-source correction.
    // Modifies SourceField::data in place before chroma decoding.
    void correctFrame(chd::decoders::SourceField &firstField,
                      chd::decoders::SourceField &secondField,
                      bool overCorrect, bool intraField,
                      DropoutCorrectionStats *stats = nullptr);

    // Multi-source correction.
    // Primary fields are modified in place. Extra sources provide replacement
    // data from additional captures aligned via caller-supplied frame mapping.
    void correctFrame(chd::decoders::SourceField &primaryFirst,
                      chd::decoders::SourceField &primarySecond,
                      const std::vector<ExtraSourceFrame> &extraSources,
                      bool overCorrect, bool intraField,
                      DropoutCorrectionStats *stats = nullptr);

private:
    enum Location {
        visibleLine,
        colourBurst,
        unknown
    };

    struct DropOutLocation {
        int32_t fieldLine;
        int32_t startx;
        int32_t endx;
        Location location;
    };

    struct Replacement {
        // The default value is no replacement
        Replacement() : isSameField(true), fieldLine(-1), sourceNumber(0), quality(-1.0), distance(0) {}

        bool isSameField;
        int32_t fieldLine;

        int32_t sourceNumber;
        double quality;

        int32_t distance;
    };

    chd::metadata::LdDecodeMetaData::VideoParameters videoParameters;

    void correctField(const std::vector<std::vector<DropOutLocation> > &thisFieldDropouts,
                      const std::vector<std::vector<DropOutLocation> > &otherFieldDropouts,
                      std::vector<chd::reader::Data> &thisFieldData, const std::vector<chd::reader::Data> &otherFieldData,
                      bool thisFieldIsFirst, bool intraField, const std::vector<int32_t> &availableSources,
                      const std::vector<double> &sourceQuality,
                      const std::vector<chd::metadata::LdDecodeMetaData::VideoParameters> &allVideoParams,
                      DropoutCorrectionStats *stats);
    std::vector<DropOutLocation> populateDropoutsVector(const chd::metadata::LdDecodeMetaData::Field &field,
                                                        const chd::metadata::LdDecodeMetaData::VideoParameters &vp,
                                                        bool overCorrect);
    std::vector<DropOutLocation> setDropOutLocations(std::vector<DropOutLocation> dropOuts);
    Replacement findReplacementLine(const std::vector<std::vector<DropOutLocation>> &thisFieldDropouts,
                                    const std::vector<std::vector<DropOutLocation>> &otherFieldDropouts,
                                    int32_t dropOutIndex, bool thisFieldIsFirst, bool matchChromaPhase,
                                    bool isColourBurst, bool intraField, const std::vector<int32_t> &availableSources,
                                    const std::vector<double> &sourceQuality,
                                    const std::vector<chd::metadata::LdDecodeMetaData::VideoParameters> &allVideoParams);
    void findPotentialReplacementLine(const std::vector<std::vector<DropOutLocation>> &targetDropouts, int32_t targetIndex,
                                      const std::vector<std::vector<DropOutLocation>> &sourceDropouts, bool isSameField,
                                      int32_t sourceOffset, int32_t stepAmount,
                                      int32_t sourceNo, const std::vector<double> &sourceQuality,
                                      const std::vector<chd::metadata::LdDecodeMetaData::VideoParameters> &allVideoParams,
                                      std::vector<Replacement> &candidates);
    void correctDropOut(const DropOutLocation &dropOut,
                        const Replacement &replacement, const Replacement &chromaReplacement,
                        std::vector<chd::reader::Data> &thisFieldData, const std::vector<chd::reader::Data> &otherFieldData);
};

}  // namespace chd::dropout

#endif  // CHD_DROPOUT_DROPOUT_CORRECTOR_H
