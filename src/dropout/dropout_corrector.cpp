// SPDX-License-Identifier: GPL-3.0-or-later
/************************************************************************

    dropoutcorrect.cpp

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

#include "dropout_corrector.h"

#include <cstdlib>

#include "luma_filters.h"

namespace chd::dropout {

DropoutCorrector::DropoutCorrector(const chd::metadata::LdDecodeMetaData::VideoParameters &videoParams)
    : videoParameters(videoParams)
{
}

// Single-source convenience: delegates to multi-source with no extras
void DropoutCorrector::correctFrame(chd::decoders::SourceField &firstField,
                                    chd::decoders::SourceField &secondField,
                                    bool overCorrect, bool intraField,
                                    DropoutCorrectionStats *stats)
{
    correctFrame(firstField, secondField, {}, overCorrect, intraField, stats);
}

// Multi-source correction
void DropoutCorrector::correctFrame(chd::decoders::SourceField &primaryFirst,
                                    chd::decoders::SourceField &primarySecond,
                                    const std::vector<ExtraSourceFrame> &extraSources,
                                    bool overCorrect, bool intraField,
                                    DropoutCorrectionStats *stats)
{
    // Determine broadcast field order from metadata
    chd::decoders::SourceField &broadcastFirst  = primaryFirst.field.isFirstField ? primaryFirst  : primarySecond;
    chd::decoders::SourceField &broadcastSecond = primaryFirst.field.isFirstField ? primarySecond : primaryFirst;

    // Build total source count: primary (0) + extras (1..N)
    const int32_t totalSources = 1 + static_cast<int32_t>(extraSources.size());

    // Collect all field data + per-field metadata + per-source VP, indexed by source.
    std::vector<chd::reader::Data> allFirstFieldData(totalSources);
    std::vector<chd::reader::Data> allSecondFieldData(totalSources);
    std::vector<chd::metadata::LdDecodeMetaData::Field> allFirstFieldMeta(totalSources);
    std::vector<chd::metadata::LdDecodeMetaData::Field> allSecondFieldMeta(totalSources);
    std::vector<chd::metadata::LdDecodeMetaData::VideoParameters> allVideoParams(totalSources);
    std::vector<double> sourceQuality(totalSources);

    // Source 0 = primary
    allFirstFieldData[0]  = broadcastFirst.data;
    allSecondFieldData[0] = broadcastSecond.data;
    allFirstFieldMeta[0]  = broadcastFirst.field;
    allSecondFieldMeta[0] = broadcastSecond.field;
    allVideoParams[0]     = videoParameters;
    // Primary quality from VITS bPSNR average across both fields.
    sourceQuality[0] = (broadcastFirst.field.vitsMetrics.bPSNR
                       + broadcastSecond.field.vitsMetrics.bPSNR) / 2.0;

    // Sources 1..N = extras
    for (int32_t i = 0; i < static_cast<int32_t>(extraSources.size()); i++) {
        allFirstFieldData[i + 1]  = extraSources[i].firstFieldData;
        allSecondFieldData[i + 1] = extraSources[i].secondFieldData;
        allFirstFieldMeta[i + 1]  = extraSources[i].firstFieldMeta;
        allSecondFieldMeta[i + 1] = extraSources[i].secondFieldMeta;
        allVideoParams[i + 1]     = extraSources[i].videoParams;
        sourceQuality[i + 1]      = extraSources[i].quality;
    }

    // Determine which sources are available (all of them, since the caller
    // only passes extras that have the required frame).
    std::vector<int32_t> availableSources;
    availableSources.reserve(totalSources);
    for (int32_t i = 0; i < totalSources; i++) availableSources.push_back(i);

    // Skip if no dropouts in primary fields
    if (allFirstFieldMeta[0].dropOuts.empty() && allSecondFieldMeta[0].dropOuts.empty()) {
        return;
    }

    // Build dropout location vectors for all sources
    std::vector<std::vector<DropOutLocation>> firstFieldDropouts(totalSources);
    std::vector<std::vector<DropOutLocation>> secondFieldDropouts(totalSources);
    for (int32_t i = 0; i < totalSources; i++) {
        if (!allFirstFieldMeta[i].dropOuts.empty())
            firstFieldDropouts[i] = setDropOutLocations(populateDropoutsVector(allFirstFieldMeta[i], allVideoParams[i], overCorrect));
        if (!allSecondFieldMeta[i].dropOuts.empty())
            secondFieldDropouts[i] = setDropOutLocations(populateDropoutsVector(allSecondFieldMeta[i], allVideoParams[i], overCorrect));
    }

    // Correct both fields
    correctField(firstFieldDropouts, secondFieldDropouts, allFirstFieldData, allSecondFieldData,
                 true, intraField, availableSources, sourceQuality, allVideoParams, stats);
    correctField(secondFieldDropouts, firstFieldDropouts, allSecondFieldData, allFirstFieldData,
                 false, intraField, availableSources, sourceQuality, allVideoParams, stats);

    // Write corrected primary data back
    broadcastFirst.data  = allFirstFieldData[0];
    broadcastSecond.data = allSecondFieldData[0];
}

// Correct dropouts within one field
void DropoutCorrector::correctField(const std::vector<std::vector<DropOutLocation>> &thisFieldDropouts,
                                    const std::vector<std::vector<DropOutLocation>> &otherFieldDropouts,
                                    std::vector<chd::reader::Data> &thisFieldData,
                                    const std::vector<chd::reader::Data> &otherFieldData,
                                    bool thisFieldIsFirst, bool intraField,
                                    const std::vector<int32_t> &availableSources,
                                    const std::vector<double> &sourceQuality,
                                    const std::vector<chd::metadata::LdDecodeMetaData::VideoParameters> &allVideoParams,
                                    DropoutCorrectionStats *stats)
{
    for (int32_t dropoutIndex = 0; dropoutIndex < static_cast<int32_t>(thisFieldDropouts[0].size()); dropoutIndex++) {
        Replacement replacement, chromaReplacement;

        // Is the current dropout in the colour burst?
        if (thisFieldDropouts[0][dropoutIndex].location == Location::colourBurst) {
            replacement = findReplacementLine(thisFieldDropouts, otherFieldDropouts,
                                              dropoutIndex, thisFieldIsFirst, true,
                                              true, intraField, availableSources,
                                              sourceQuality, allVideoParams);
        }

        // Is the current dropout in the visible video line?
        if (thisFieldDropouts[0][dropoutIndex].location == Location::visibleLine) {
            // Find separate replacements for luma and chroma
            replacement = findReplacementLine(thisFieldDropouts, otherFieldDropouts,
                                              dropoutIndex, thisFieldIsFirst, false,
                                              false, intraField, availableSources,
                                              sourceQuality, allVideoParams);
            chromaReplacement = findReplacementLine(thisFieldDropouts, otherFieldDropouts,
                                                    dropoutIndex, thisFieldIsFirst, true,
                                                    false, intraField, availableSources,
                                                    sourceQuality, allVideoParams);
        }

        // Record stats from the chosen replacement (matches vapoursynth-analog's
        // outer accounting rather than the upstream's same/multi-source split).
        if (stats) {
            if (replacement.fieldLine == -1) {
                stats->failed++;
            } else {
                stats->corrected++;
                stats->totalDistance += replacement.distance;
            }
        }

        // Correct the data
        correctDropOut(thisFieldDropouts[0][dropoutIndex], replacement, chromaReplacement,
                       thisFieldData, otherFieldData);
    }
}

// Populate the dropouts vector
std::vector<DropoutCorrector::DropOutLocation> DropoutCorrector::populateDropoutsVector(
    const chd::metadata::LdDecodeMetaData::Field &field,
    const chd::metadata::LdDecodeMetaData::VideoParameters &vp,
    bool overCorrect)
{
    std::vector<DropOutLocation> fieldDropOuts;

    for (int32_t dropOutIndex = 0; dropOutIndex < field.dropOuts.size(); dropOutIndex++) {
        DropOutLocation dropOutLocation;
        dropOutLocation.startx = field.dropOuts.startx(dropOutIndex);
        dropOutLocation.endx = field.dropOuts.endx(dropOutIndex);
        dropOutLocation.fieldLine = field.dropOuts.fieldLine(dropOutIndex);
        dropOutLocation.location = DropoutCorrector::Location::unknown;

        // Ignore dropouts outside the field's data
        if (dropOutLocation.fieldLine < 1 || dropOutLocation.fieldLine > vp.fieldHeight) {
            continue;
        }

        // Is over correct mode selected?
        if (overCorrect) {
            // Here we deliberately extend the length of dropouts to ensure that the
            // correction captures as much as possible.  This is useful on heavily
            // damaged discs where drop-outs can 'slope' in and out fooling ld-decode's
            // detection mechanisms

            int32_t overCorrectionDots = 24;
            if (dropOutLocation.startx > overCorrectionDots) dropOutLocation.startx -= overCorrectionDots;
            else dropOutLocation.startx = 0;
            if (dropOutLocation.endx < vp.fieldWidth - overCorrectionDots) dropOutLocation.endx += overCorrectionDots;
            else dropOutLocation.endx = vp.fieldWidth;
        }

        fieldDropOuts.push_back(dropOutLocation);
    }

    return fieldDropOuts;
}

// Figure out where drop-outs occur and split them if in more than one area
std::vector<DropoutCorrector::DropOutLocation> DropoutCorrector::setDropOutLocations(std::vector<DropoutCorrector::DropOutLocation> dropOuts)
{
    // Split count shows if a drop-out has been split (i.e. the original
    // drop-out covered more than one area).
    //
    // Since a drop-out can span multiple areas, we have to keep
    // splitting the drop-outs until there is nothing left to split
    int32_t splitCount = 0;

    do {
        int32_t noOfDropOuts = static_cast<int32_t>(dropOuts.size());
        splitCount = 0;

        for (int32_t index = 0; index < noOfDropOuts; index++) {
            // Does the drop-out start in the colour burst area?
            if (dropOuts[index].startx <= videoParameters.colourBurstEnd) {
                dropOuts[index].location = Location::colourBurst;

                // Does the drop-out end in the colour burst area?
                if (dropOuts[index].endx > videoParameters.colourBurstEnd) {
                    // Split the drop-out in two
                    DropOutLocation tempDropOut;
                    tempDropOut.startx = videoParameters.colourBurstEnd + 1;
                    tempDropOut.endx = dropOuts[index].endx;
                    tempDropOut.fieldLine = dropOuts[index].fieldLine;
                    tempDropOut.location = Location::colourBurst;
                    dropOuts.push_back(tempDropOut);

                    // Shorten the original drop out
                    dropOuts[index].endx = videoParameters.colourBurstEnd;

                    splitCount++;
                }
            }

            // Does the drop-out start in the active video area?
            // Note: Here we use the colour burst end as the active video start (to prevent a case where the
            // drop out begins between the colour burst level end and active video start and would go undetected)
            else if (dropOuts[index].startx > videoParameters.colourBurstEnd && dropOuts[index].startx <= videoParameters.activeVideoEnd) {
                dropOuts[index].location = Location::visibleLine;

                // Does the drop-out end in the active video area?
                if (dropOuts[index].endx > videoParameters.activeVideoEnd) {
                    // No need to split as we don't care about the sync area

                    // Shorten the original drop out
                    dropOuts[index].endx = videoParameters.activeVideoEnd;

                    splitCount++;
                }
            }
        }
    } while (splitCount != 0);

    return dropOuts;
}

// Find a replacement line to take replacement data from.  This method looks both up and down the field
// for the nearest replacement line that doesn't contain a drop-out itself (to prevent copying bad data
// over bad data).
DropoutCorrector::Replacement DropoutCorrector::findReplacementLine(const std::vector<std::vector<DropOutLocation>> &thisFieldDropouts,
                                                                    const std::vector<std::vector<DropOutLocation>> &otherFieldDropouts,
                                                                    int32_t dropOutIndex, bool thisFieldIsFirst, bool matchChromaPhase,
                                                                    bool isColourBurst, bool intraField,
                                                                    const std::vector<int32_t> &availableSources,
                                                                    const std::vector<double> &sourceQuality,
                                                                    const std::vector<chd::metadata::LdDecodeMetaData::VideoParameters> &allVideoParams)
{
    // Define the minimum step size to use when searching for replacement
    // lines, and the offset to the nearest replacement line in the other
    // field.
    int32_t stepAmount, otherFieldOffset;
    if (!matchChromaPhase) {
        // We're not trying to match the chroma phase, so any line will do.
        stepAmount = 1;
        otherFieldOffset = -1;
    } else if (videoParameters.system == chd::metadata::PAL || videoParameters.system == chd::metadata::PAL_M) {
        // For PAL: [Poynton ch44 p529]
        //
        // - Subcarrier has 283.7516 cycles per line, so there's a (nearly) 90
        //   degree phase shift between adjacent field lines.
        // - Colourburst is +135 degrees and -135 degrees from the subcarrier
        //   on alternate field lines.
        // - The V-switch causes the V component to be inverted on alternate
        //   field lines.
        //
        // So the nearest line we can use which has the same subcarrier phase,
        // colourburst phase and V-switch state is 4 field lines away.
        stepAmount = 4;

        // First field lines 1-313 are PAL line numbers 1-313.
        // Second field lines 1-312 are PAL line numbers 314-625.
        // Moving from first field line N to second field line N would give 313
        // lines = (nearly) 90 degrees phase shift; move by 310 lines to N-3 to
        // get (nearly) 0 degrees.
        if (thisFieldIsFirst) {
            otherFieldOffset = -3;
        } else {
            otherFieldOffset = -1;
        }
    } else {
        // For NTSC: [Poynton ch42 p511]
        //
        // - Subcarrier has 227.5 cycles per line, so there's a 180 degree
        //   phase shift between adjacent field lines.
        // - Colourburst is always 180 degrees from the subcarrier.
        //
        // So the nearest line we can use which has the same subcarrier phase
        // and colourburst phase is 2 field lines away.
        stepAmount = 2;

        // First field lines 1-263 are NTSC line numbers 1-263.
        // Second field lines 1-262 are NTSC line numbers 264-525.
        // Moving from first field line N to second field line N would give 263
        // lines = 180 degrees phase shift; move by 262 lines to N-1 to get 0
        // degrees.
        otherFieldOffset = -1;
    }

    // Look for potential replacement lines
    std::vector<DropoutCorrector::Replacement> candidates;

    for (int32_t i = 0; i < static_cast<int32_t>(availableSources.size()); i++) {
        int32_t currentSource = availableSources[i];

        // Examine this field:

        // Look up the field for a replacement
        findPotentialReplacementLine(thisFieldDropouts, dropOutIndex,
                                     thisFieldDropouts, true, 0, -stepAmount,
                                     currentSource, sourceQuality, allVideoParams,
                                     candidates);

        // Look down the field for a replacement
        findPotentialReplacementLine(thisFieldDropouts, dropOutIndex,
                                     thisFieldDropouts, true, stepAmount, stepAmount,
                                     currentSource, sourceQuality, allVideoParams,
                                     candidates);

        // Only check the other field for visible line replacements
        if (!isColourBurst && !intraField) {
            // Examine the other field:

            // Look up the field for a replacement
            findPotentialReplacementLine(thisFieldDropouts, dropOutIndex,
                                         otherFieldDropouts, false, otherFieldOffset, -stepAmount,
                                         currentSource, sourceQuality, allVideoParams,
                                         candidates);

            // Look down the field for a replacement
            findPotentialReplacementLine(thisFieldDropouts, dropOutIndex,
                                         otherFieldDropouts, false, otherFieldOffset + stepAmount, stepAmount,
                                         currentSource, sourceQuality, allVideoParams,
                                         candidates);
        }
    }

    // If no candidate is found, return no replacement
    Replacement replacement;

    if (!candidates.empty()) {
        // Find the candidate with the lowest spatial distance from the dropout
        replacement.distance = 1000000;

        // Find the candidate with the highest quality
        replacement.quality = -1;

        for (const Replacement &candidate: candidates) {
            // Work out the corresponding output frame line numbers.
            // The first field (in a .tbc, for both PAL and NTSC) contains the top frame line.
            const int32_t dropoutFrameLine = (2 * thisFieldDropouts[0][dropOutIndex].fieldLine) + (thisFieldIsFirst ? 0 : 1);
            const int32_t sourceFrameLine = (2 * candidate.fieldLine) + (candidate.isSameField ? (thisFieldIsFirst ? 0 : 1)
                                                                                               : (thisFieldIsFirst ? 1 : 0));

            const int32_t distance = std::abs(dropoutFrameLine - sourceFrameLine);

            // Choose the candidate if the distance is less
            if (distance < replacement.distance) {
                replacement = candidate;
                replacement.distance = distance;
            }

            // Choose the candidate if the distance is the same, but the quality is better
            else if (distance == replacement.distance && candidate.quality > replacement.quality) {
                replacement = candidate;
                replacement.distance = distance;
            }
        }
    }

    return replacement;
}

// Given a dropout, scan through a source field for the nearest replacement line that doesn't have overlapping dropouts.
// Adds a Replacement to candidates if one was found.
void DropoutCorrector::findPotentialReplacementLine(const std::vector<std::vector<DropOutLocation>> &targetDropouts, int32_t targetIndex,
                                                    const std::vector<std::vector<DropOutLocation>> &sourceDropouts, bool isSameField,
                                                    int32_t sourceOffset, int32_t stepAmount,
                                                    int32_t sourceNo, const std::vector<double> &sourceQuality,
                                                    const std::vector<chd::metadata::LdDecodeMetaData::VideoParameters> &allVideoParams,
                                                    std::vector<Replacement> &candidates)
{
    // Calculate the start source line, applying sourceOffset to find a line with the right chroma phase
    int32_t sourceLine = targetDropouts[0][targetIndex].fieldLine + sourceOffset;

    // Is the line within the active range?
    if ((sourceLine - 1) < allVideoParams[sourceNo].firstActiveFieldLine
        || (sourceLine - 1) >= allVideoParams[sourceNo].lastActiveFieldLine) {
        return;
    }

    // Hunt for a replacement, stopping at the last full line before the half-line between fields
    while ((sourceLine - 1) >= allVideoParams[sourceNo].firstActiveFieldLine
           && sourceLine < allVideoParams[sourceNo].lastActiveFieldLine) {
        // Is there a dropout that overlaps the one we're trying to replace?
        bool hasOverlap = false;
        for (int32_t sourceIndex = 0; sourceIndex < static_cast<int32_t>(sourceDropouts[sourceNo].size()); sourceIndex++) {
            if (sourceDropouts[sourceNo][sourceIndex].fieldLine == sourceLine &&
                (targetDropouts[0][targetIndex].endx - sourceDropouts[sourceNo][sourceIndex].startx) >= 0 &&
                (sourceDropouts[sourceNo][sourceIndex].endx - targetDropouts[0][targetIndex].startx) >= 0) {
                // Overlap -- can't use this line
                sourceLine += stepAmount;
                hasOverlap = true;
                break;
            }
        }
        if (!hasOverlap) {
            // No overlaps -- we can use this line
            Replacement replacement;
            replacement.isSameField = isSameField;
            replacement.fieldLine = sourceLine;

            // Set the source
            replacement.sourceNumber = sourceNo;

            // Set the quality of the replacement
            replacement.quality = sourceQuality[sourceNo];

            candidates.push_back(replacement);
            return;
        }
    }
}

// Correct a dropout by copying data from a replacement line.
void DropoutCorrector::correctDropOut(const DropOutLocation &dropOut,
                                      const Replacement &replacement, const Replacement &chromaReplacement,
                                      std::vector<chd::reader::Data> &thisFieldData,
                                      const std::vector<chd::reader::Data> &otherFieldData)
{
    if (replacement.fieldLine == -1) {
        // No correction needed
        return;
    }

    const uint16_t *sourceLine = (replacement.isSameField ? thisFieldData[replacement.sourceNumber].data()
                                                          : otherFieldData[replacement.sourceNumber].data())
                                + ((replacement.fieldLine - 1) * videoParameters.fieldWidth);
    uint16_t *targetLine = thisFieldData[0].data() + ((dropOut.fieldLine - 1) * videoParameters.fieldWidth);

    // Choose whole signal or just chroma replacement
    // Don't use chroma if the source of the replacement is > 0 and coming from the same line in another source
    if ((chromaReplacement.fieldLine == -1) || ((dropOut.fieldLine == replacement.fieldLine) && (dropOut.fieldLine == chromaReplacement.fieldLine))) {
        // No separate chroma replacement; just copy the whole signal
        for (int32_t pixel = dropOut.startx; pixel < dropOut.endx; pixel++) {
            targetLine[pixel] = sourceLine[pixel];
        }
    } else {
        // Combine low frequencies (mostly luma) from replacement with high
        // frequencies (mostly chroma) from chromaReplacement. As this is only
        // a 1D filter, it won't achieve very good separation, but it's good
        // enough for the purposes of replacing a dropout.
        Filters filters;
        std::vector<uint16_t> lineBuf(videoParameters.fieldWidth);
        auto filterLineBuf = [&] {
            if (videoParameters.system == chd::metadata::PAL) {
                filters.palLumaFirFilter(lineBuf.data(), lineBuf.size());
            } else if (videoParameters.system == chd::metadata::NTSC) {
                filters.ntscLumaFirFilter(lineBuf.data(), lineBuf.size());
            } else {
                filters.palMLumaFirFilter(lineBuf.data(), lineBuf.size());
            }
        };

        // Extract LF from replacement
        for (int32_t pixel = 0; pixel < videoParameters.fieldWidth; pixel++) {
            lineBuf[pixel] = sourceLine[pixel];
        }
        filterLineBuf();
        for (int32_t pixel = dropOut.startx; pixel < dropOut.endx; pixel++) {
            targetLine[pixel] = lineBuf[pixel];
        }

        // Extract HF from chromaReplacement (by extracting LF, then subtracting from the original)
        const uint16_t *chromaLine = (chromaReplacement.isSameField ? thisFieldData[chromaReplacement.sourceNumber].data()
                                                                    : otherFieldData[chromaReplacement.sourceNumber].data())
                                    + ((chromaReplacement.fieldLine - 1) * videoParameters.fieldWidth);
        for (int32_t pixel = 0; pixel < videoParameters.fieldWidth; pixel++) {
            lineBuf[pixel] = chromaLine[pixel];
        }
        filterLineBuf();
        for (int32_t pixel = dropOut.startx; pixel < dropOut.endx; pixel++) {
            targetLine[pixel] += chromaLine[pixel] - lineBuf[pixel];
        }
    }
}

}  // namespace chd::dropout
