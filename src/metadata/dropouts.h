/******************************************************************************
 * dropouts.h
 * ld-decode-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2018-2025 Simon Inns
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef CHD_METADATA_DROPOUTS_H
#define CHD_METADATA_DROPOUTS_H

#include <cstdint>
#include <vector>

namespace chd::metadata {

class JsonReader;
class JsonWriter;
class SqliteReader;
class SqliteWriter;

class DropOuts
{
public:
    DropOuts() = default;
    DropOuts(int reserve);
    ~DropOuts() = default;
    DropOuts(const DropOuts &) = default;

    DropOuts(const std::vector<int32_t> &startx, const std::vector<int32_t> &endx, const std::vector<int32_t> &fieldLine);
    DropOuts &operator=(const DropOuts &);

    void append(const int32_t startx, const int32_t endx, const int32_t fieldLine);
    void reserve(int size);
    void resize(int32_t size);
    void clear();
    void concatenate(const bool verbose=true);

    // Return the number of dropouts
    int32_t size() const {
        return static_cast<int32_t>(m_startx.size());
    }

    // Return true if there are no dropouts
    bool empty() const {
        return m_startx.empty();
    }

    // Get position of a dropout
    int32_t startx(int32_t index) const {
        return m_startx[index];
    }
    int32_t endx(int32_t index) const {
        return m_endx[index];
    }
    int32_t fieldLine(int32_t index) const {
        return m_fieldLine[index];
    }

    void read(SqliteReader &reader, int captureId, int fieldId);
    void write(SqliteWriter &writer, int captureId, int fieldId) const;
    void read(JsonReader &reader);

private:
    std::vector<int32_t> m_startx;
    std::vector<int32_t> m_endx;
    std::vector<int32_t> m_fieldLine;
};

} // namespace chd::metadata

#endif // CHD_METADATA_DROPOUTS_H
