// SPDX-License-Identifier: GPL-3.0-or-later

#include "nntransform3d_fft_cpu.h"

namespace chd::decoders::nntransform3d {

CpuPlans &getThreadLocalCpuPlans()
{
    static thread_local CpuPlans plans { nullptr, nullptr };
    if (plans.forward == nullptr || plans.inverse == nullptr) {
        // Allocate scratch buffers just to construct the plans, then free
        // them — FFTW retains the plan independent of the buffer it was
        // measured against.
        auto *planIn  = reinterpret_cast<fftw_complex *>(
            fftw_malloc(sizeof(fftw_complex) * kNt * kNy * kNx));
        auto *planOut = reinterpret_cast<fftw_complex *>(
            fftw_malloc(sizeof(fftw_complex) * kNt * kNy * kNx));
        plans.forward = fftw_plan_dft_3d(kNt, kNy, kNx, planIn, planOut,
                                         FFTW_FORWARD, FFTW_ESTIMATE);
        plans.inverse = fftw_plan_dft_3d(kNt, kNy, kNx, planOut, planIn,
                                         FFTW_BACKWARD, FFTW_ESTIMATE);
        fftw_free(planIn);
        fftw_free(planOut);
    }
    return plans;
}

}  // namespace chd::decoders::nntransform3d
