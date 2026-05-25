// SPDX-License-Identifier: GPL-3.0-or-later

#include "ldzeug_base.h"

#include <utility>

#include "../../common/log.h"
#include "../../nn/ort_session.h"

namespace chd::decoders::ldzeug {

bool LdzeugDecoderBase::configure(
    const chd::metadata::LdDecodeMetaData::VideoParameters &vp)
{
    if (vp.system != chd::metadata::NTSC) {
        chd::log::error() << "ldzeug decoders are for NTSC video sources only";
        return false;
    }
    videoParameters = vp;
    return true;
}

void LdzeugDecoderBase::setNnModel(std::shared_ptr<chd::nn::OrtSession> session)
{
    session_ = std::move(session);
}

}  // namespace chd::decoders::ldzeug
