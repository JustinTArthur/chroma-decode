/******************************************************************************
 * jsonio.h
 * ld-decode-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2022 Adam Sampson
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef CHD_METADATA_JSON_IO_H
#define CHD_METADATA_JSON_IO_H

#include <cassert>
#include <cstdint>
#include <istream>
#include <ostream>
#include <stdexcept>
#include <string>
#include <stack>
#include <cmath>

namespace chd::metadata {

class JsonReader
{
public:
    JsonReader(std::istream &_input);

    // Exception class to be thrown when parsing fails
    class Error : public std::runtime_error
    {
    public:
        Error(std::string message) : std::runtime_error(message) {}
    };

    // Throw an Error exception with the given message
    [[noreturn]] void throwError(std::string message) {
        throw Error(message + " at byte " + std::to_string(position));
    }

    // Numbers
    void read(int64_t &value);
    void read(int &value);
    void read(double &value);

    // Booleans
    void read(bool &value);

    // Strings
    void read(std::string &value);

    // Objects (each member followed by a value)
    void beginObject();
    bool readMember(std::string &member);
    void endObject();

    // Arrays (each element followed by a value)
    void beginArray();
    bool readElement();
    void endArray();

    // Read and discard the next value, whatever type it is
    void discard();

private:
    char get();
    char spaceGet();
    void unget();

    void readString(std::string &value);
    void readNumber(double &value);
    template <typename T> void readSignedInteger(T& value) {
        // Round to the nearest integer
        double d;
        readNumber(d);
        value = static_cast<T>(std::llround(d));
    }

    // The input stream
    std::istream &input;
    unsigned long position;

    // True if we're at the start of a { or [ construct
    bool atStart;
    std::stack<bool> atStarts;

    // Buffer for reading tokens, to minimise reallocations
    std::string buf;
};

class JsonWriter
{
public:
    JsonWriter(std::ostream &_output);

    // Numbers
    void write(int value);
    void write(int64_t value);
    void write(double value);

    // Booleans
    void write(bool value);

    // Strings
    void write(const char *value);

    // Objects (each member followed by a value)
    void beginObject();
    void writeMember(const char *name);
    template <typename T>
    void writeMember(const char *name, T value) {
        writeMember(name);
        write(value);
    }
    void endObject();

    // Arrays (each element followed by a value)
    void beginArray();
    void writeElement();
    void endArray();

private:
    void writeString(const char *str);

    // The output stream
    std::ostream &output;

    // True if we're at the start of a [ or { construct
    bool atStart;
    std::stack<bool> atStarts;
};

} // namespace chd::metadata

#endif // CHD_METADATA_JSON_IO_H
