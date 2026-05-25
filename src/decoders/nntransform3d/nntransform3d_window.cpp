// SPDX-License-Identifier: GPL-3.0-or-later

#include "nntransform3d_window.h"

#include <cmath>
#include <mutex>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace chd::decoders::nntransform3d {

namespace {
SineWindows  g_windows;
std::once_flag g_initOnce;
}  // namespace

const SineWindows &getSineWindows()
{
    std::call_once(g_initOnce, []() {
        for (int i = 0; i < kNx; i++) {
            g_windows.x[i] = std::sin(M_PI * (static_cast<double>(i) + 0.5) /
                                      static_cast<double>(kNx));
        }
        for (int i = 0; i < kNy; i++) {
            g_windows.y[i] = std::sin(M_PI * (static_cast<double>(i) + 0.5) /
                                      static_cast<double>(kNy));
        }
        for (int i = 0; i < kNt; i++) {
            g_windows.t[i] = std::sin(M_PI * (static_cast<double>(i) + 0.5) /
                                      static_cast<double>(kNt));
        }
    });
    return g_windows;
}

}  // namespace chd::decoders::nntransform3d
