/******************************************************************************
 * dropouts.cpp
 * ld-decode-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2018-2025 Simon Inns
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "dropouts.h"

#include "ld_metadata_sqlite.h"

#include <cassert>

#include "../common/log.h"

namespace chd::metadata {

DropOuts::DropOuts(const std::vector<int32_t> &startx, const std::vector<int32_t> &endx, const std::vector<int32_t> &fieldLine)
    : m_startx(startx), m_endx(endx), m_fieldLine(fieldLine)
{
}

DropOuts::DropOuts(int reserve_size)
    : m_startx(), m_endx(), m_fieldLine()
{
    reserve(reserve_size);
}

// Assignment '=' operator
DropOuts& DropOuts::operator=(const DropOuts &inDropouts)
{
    // Do not allow assignment if both objects are the same
    if (this != &inDropouts) {
        // Copy the object's data
        m_startx = inDropouts.m_startx;
        m_endx = inDropouts.m_endx;
        m_fieldLine = inDropouts.m_fieldLine;;
    }

    return *this;
}

// Append a dropout
void DropOuts::append(const int32_t startx, const int32_t endx, const int32_t fieldLine)
{
    m_startx.push_back(startx);
    m_endx.push_back(endx);
    m_fieldLine.push_back(fieldLine);
}

void DropOuts::reserve(int size)
{
    m_startx.reserve(size);
    m_endx.reserve(size);
    m_fieldLine.reserve(size);
}

// Resize the size of the DropOuts record
void DropOuts::resize(int32_t size)
{
    m_startx.resize(size);
    m_endx.resize(size);
    m_fieldLine.resize(size);
}

// Clear the DropOuts record
void DropOuts::clear()
{
    m_startx.clear();
    m_endx.clear();
    m_fieldLine.clear();
}

// Method to concatenate dropouts on the same line that are close together
// (to cut down on the amount of generated dropout data when processing noisy/bad sources)

// TODO: Sort the DOs by fieldLine otherwise this won't work correctly unless the
// caller is aware of the restriction (used in ld-diffdod only at the moment)
void DropOuts::concatenate(const bool verbose)
{
    int32_t sizeAtStart = m_startx.size();

    // This variable controls the minimum allowed gap between dropouts
    // if the gap between the end of the last dropout and the start of
    // the next is less than minimumGap, the two dropouts will be
    // concatenated together
    int32_t minimumGap = 50;

    // Start from dropout 1 as 0 has no previous dropout
    int32_t i = 1;

    while (i < m_startx.size()) {
        // Is the current dropout on the same frame line as the last?
        if (m_fieldLine[i - 1] == m_fieldLine[i]) {
            if ((m_endx[i - 1] + minimumGap) > (m_startx[i])) {
                // Concatenate
                m_endx[i - 1] = m_endx[i];

                // Remove the current dropout
                m_startx.erase(m_startx.begin() + i);
                m_endx.erase(m_endx.begin() + i);
                m_fieldLine.erase(m_fieldLine.begin() + i);
            }
        }

        // Next dropout
        i++;
    }

    if(verbose){chd::log::debug() << "Concatenated dropouts: was" << sizeAtStart << "now" << m_startx.size() << "dropouts";}
}

// Read DropOuts from SQLite
void DropOuts::read(SqliteReader &reader, int captureId, int fieldId)
{
    clear();

    SqliteQuery dropoutsQuery;
    if (reader.readFieldDropouts(captureId, fieldId, dropoutsQuery)) {
        while (dropoutsQuery.next()) {
            int startx = dropoutsQuery.value("startx").toInt();
            int endx = dropoutsQuery.value("endx").toInt();
            int fieldLine = dropoutsQuery.value("field_line").toInt();
            append(startx, endx, fieldLine);
        }
    }
}

// Write DropOuts to SQLite
void DropOuts::write(SqliteWriter &writer, int captureId, int fieldId) const
{
    for (int i = 0; i < size(); i++) {
        writer.writeFieldDropouts(captureId, fieldId, startx(i), endx(i), fieldLine(i));
    }
}

}  // namespace chd::metadata


