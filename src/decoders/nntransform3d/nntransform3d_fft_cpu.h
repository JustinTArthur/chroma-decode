// SPDX-License-Identifier: GPL-3.0-or-later
//
// Thread-local FFTW plan cache for the nnTransform3D CPU path. The plans
// operate on Nt=4, Ny=16, Nx=16 complex tiles, matching the chroma_net
// model's expected input shape. Plans are allocated lazily per worker
// thread so the pool created by DecoderPool::process can run inference in
// parallel without serialising on a global FFTW lock.
//
// This is the FFT primitive for the CPU path only. The CUDA and HIP paths
// carry their own device-resident cuFFT/hipFFT inside their pipelines.

#ifndef CHD_DECODERS_NNTRANSFORM3D_FFT_CPU_H
#define CHD_DECODERS_NNTRANSFORM3D_FFT_CPU_H

#include <fftw3.h>

namespace chd::decoders::nntransform3d {

// Block dimensions matching the chroma_net model trained by asdfqazsnbb.
// Changing these requires a different model.
inline constexpr int kNt = 4;   // temporal block
inline constexpr int kNy = 16;  // vertical block
inline constexpr int kNx = 16;  // horizontal block

// Overlap-add step follows asdfqazsnbb's harness: half the block size, so each output
// sample is touched by ~4 tiles on the (X,Y) plane.
inline constexpr int kStepX = 8;
inline constexpr int kStepY = 8;

// Thread-local plan accessor. The first call on a worker thread allocates
// both plans (forward + inverse) sized for Nt*Ny*Nx complex points; later
// calls are no-ops. Plans are intentionally never explicitly destroyed —
// they live until process exit.
struct CpuPlans {
    fftw_plan forward;
    fftw_plan inverse;
};
CpuPlans &getThreadLocalCpuPlans();

}  // namespace chd::decoders::nntransform3d

#endif  // CHD_DECODERS_NNTRANSFORM3D_FFT_CPU_H
