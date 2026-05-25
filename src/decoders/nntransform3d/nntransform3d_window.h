// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sine window tables for the nnTransform3D overlap-add
// reconstruction. The three 1D windows winX[i], winY[i], winT[i] match
// asdfqazsnbb's harness: each is
//
//     sin(pi * (i + 0.5) / N)
//
// where N is the block size on that axis. The full 3D weight applied to
// each tile sample is winX[x] * winY[y] * winT[t]. With the kStepX/kStepY
// stride of half the block size, four overlapping tiles touch each output
// pixel and the per-pixel weight sum is collected into a weight buffer for
// normalisation at the end (finalizeNnTransform3D).

#ifndef CHD_DECODERS_NNTRANSFORM3D_WINDOW_H
#define CHD_DECODERS_NNTRANSFORM3D_WINDOW_H

#include "nntransform3d_fft_cpu.h"

namespace chd::decoders::nntransform3d {

struct SineWindows {
    double x[kNx];
    double y[kNy];
    double t[kNt];
};

// Process-wide one-shot table, initialised via std::call_once on first
// access. Cheap to compute; only kNx+kNy+kNt = 36 sin() calls.
const SineWindows &getSineWindows();

}  // namespace chd::decoders::nntransform3d

#endif  // CHD_DECODERS_NNTRANSFORM3D_WINDOW_H
