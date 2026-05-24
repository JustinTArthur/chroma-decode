// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <sstream>

namespace chd::log {

enum class Level { Debug, Info, Warn, Error };

class Stream {
public:
    explicit Stream(Level level);
    Stream(const Stream &) = delete;
    Stream &operator=(const Stream &) = delete;
    Stream(Stream &&other) noexcept;
    ~Stream();

    template <typename T>
    Stream &operator<<(const T &value) {
        if (!first_) buf_ << sep_;
        buf_ << value;
        first_ = false;
        return *this;
    }

    Stream &nospace() { sep_ = ""; return *this; }
    Stream &noquote() { return *this; }

private:
    std::ostringstream buf_;
    Level level_;
    const char *sep_ = " ";
    bool first_ = true;
    bool active_ = true;
};

Stream debug();
Stream info();
Stream warn();
Stream error();

bool isDebugEnabled();
void setDebugEnabled(bool enabled);

}  // namespace chd::log
