// SPDX-License-Identifier: GPL-3.0-or-later
/************************************************************************

    filters.h

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

#ifndef CHD_DROPOUT_LUMA_FILTERS_H
#define CHD_DROPOUT_LUMA_FILTERS_H

#include <cstdint>

namespace chd::dropout {

class Filters
{
public:
    void palLumaFirFilter(uint16_t *data, int32_t dataPoints);

    void ntscLumaFirFilter(uint16_t *data, int32_t dataPoints);

    void palMLumaFirFilter(uint16_t *data, int32_t dataPoints);
};

}  // namespace chd::dropout

#endif // CHD_DROPOUT_LUMA_FILTERS_H
