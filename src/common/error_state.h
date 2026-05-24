// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace chd::detail {

void set_last_error(std::string msg);
const std::string& get_last_error();
void clear_last_error();

}  // namespace chd::detail
