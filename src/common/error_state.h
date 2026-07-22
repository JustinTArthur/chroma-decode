// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace chd::detail {

void set_last_error(std::string msg);
const std::string& get_last_error();
void clear_last_error();

// Message for a failure a lower layer has already described. Returns whatever
// that layer recorded (via chd::log::fail()), or `fallback` if it recorded
// nothing. Clear the detail before the guarded call so a stale message from an
// earlier failure on this thread cannot be mistaken for this one's.
std::string detail_or(const std::string &fallback);

}  // namespace chd::detail
