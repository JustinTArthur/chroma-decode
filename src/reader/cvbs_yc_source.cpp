// SPDX-License-Identifier: GPL-3.0-or-later

#include "cvbs_yc_source.h"

#include <stdexcept>

#include "../common/log.h"

namespace chd::reader {

CvbsYcSource::CvbsYcSource() = default;
CvbsYcSource::~CvbsYcSource()
{
    if (isOpen) {
        yFile.close();
        cFile.close();
    }
}

bool CvbsYcSource::open(const std::string &yPath,
                        const std::string &cPath,
                        const chd::format::VideoStandardPreset &videoStandard,
                        chd::format::SampleEncoding sampleEncoding,
                        chd::format::SignalState signalState)
{
    if (isOpen) {
        chd::log::warn() << "CvbsYcSource::open(): source already open";
        return false;
    }

    standardEnum = videoStandard.standard;
    encoding     = sampleEncoding;
    state        = signalState;
    videoParameters = chd::format::makeVideoParameters(
        videoStandard,
        /*isSubcarrierLocked*/ chd::format::getSignalState(state).burstLocked);
    fieldWidth   = videoParameters.fieldWidth;
    fieldHeight  = videoParameters.fieldHeight;
    fieldSamples = fieldWidth * fieldHeight;
    fieldByteSize = fieldSamples * chd::format::getSampleEncoding(encoding).bytesPerSample;

    yFile.open(yPath, std::ios::binary);
    cFile.open(cPath, std::ios::binary);
    if (!yFile.is_open() || !cFile.is_open()) {
        chd::log::warn() << "CvbsYcSource::open(): could not open" << yPath << "/" << cPath;
        yFile.close();
        cFile.close();
        return false;
    }

    yFile.seekg(0, std::ios::end);
    cFile.seekg(0, std::ios::end);
    const int64_t ySize = yFile.tellg();
    const int64_t cSize = cFile.tellg();
    yFile.seekg(0, std::ios::beg);
    cFile.seekg(0, std::ios::beg);
    if (ySize != cSize || ySize <= 0 || fieldByteSize <= 0) {
        chd::log::warn().nospace()
            << "CvbsYcSource::open(): file size mismatch (y=" << ySize << ", c=" << cSize << ")";
        yFile.close();
        cFile.close();
        return false;
    }
    fileSize = ySize;
    numFields = static_cast<int32_t>(fileSize / fieldByteSize);
    isOpen = true;
    chd::log::debug().nospace()
        << "CvbsYcSource::open(): " << yPath << " + " << cPath << " have " << numFields
        << " fields (" << fieldWidth << "x" << fieldHeight << " samples/field)";
    return true;
}

void CvbsYcSource::close()
{
    if (!isOpen) return;
    yFile.close();
    cFile.close();
    isOpen = false;
    fieldCache.clear();
}

const chd::metadata::LdDecodeMetaData::VideoParameters &CvbsYcSource::parameters() const
{
    return videoParameters;
}

chd::format::SignalState CvbsYcSource::signalState() const
{
    return state;
}

chd::format::SampleEncoding CvbsYcSource::sampleEncoding() const
{
    return encoding;
}

bool CvbsYcSource::isSourceValid() const
{
    return isOpen;
}

int32_t CvbsYcSource::getNumberOfAvailableFields() const
{
    return numFields;
}

int32_t CvbsYcSource::getFieldLength() const
{
    return fieldSamples;
}

CvbsYcSource::Data CvbsYcSource::getVideoField(int32_t fieldNumber,
                                               int32_t startFieldLine,
                                               int32_t endFieldLine)
{
    if (!isOpen) throw std::runtime_error("CvbsYcSource::getVideoField(): source not open");
    const int32_t fieldIndex = fieldNumber - 1;
    if (fieldIndex < 0 || fieldIndex >= numFields) {
        throw std::runtime_error("CvbsYcSource::getVideoField(): field out of range");
    }

    std::lock_guard<std::mutex> lock(ioMutex);

    int64_t startByte;
    int64_t numBytes;
    const bool wholeField = (startFieldLine == -1 && endFieldLine == -1);
    if (wholeField) {
        auto it = fieldCache.find(fieldIndex);
        if (it != fieldCache.end()) return it->second;
        startByte = static_cast<int64_t>(fieldByteSize) * fieldIndex;
        numBytes  = fieldByteSize;
    } else {
        const int32_t first0 = startFieldLine - 1;
        const int32_t last0  = endFieldLine - 1;
        if (first0 < 0 || last0 < first0 || last0 >= fieldHeight) {
            throw std::runtime_error("CvbsYcSource::getVideoField(): line range out of bounds");
        }
        const int64_t lineByteSize = static_cast<int64_t>(fieldWidth) * 2;
        startByte = static_cast<int64_t>(fieldByteSize) * fieldIndex + lineByteSize * first0;
        numBytes  = lineByteSize * (last0 - first0 + 1);
    }

    Data data = readAndSynthesise(startByte, numBytes);
    if (wholeField) {
        fieldCache.emplace(fieldIndex, data);
    }
    return data;
}

CvbsYcSource::Data CvbsYcSource::readAndSynthesise(int64_t startByte, int64_t numBytes)
{
    yFile.clear(); cFile.clear();
    yFile.seekg(startByte);
    cFile.seekg(startByte);
    if (!yFile.good() || !cFile.good()) {
        throw std::runtime_error("CvbsYcSource::readAndSynthesise(): seek failed");
    }

    const int64_t numSamples = numBytes / 2;
    std::vector<int16_t> yRaw(static_cast<size_t>(numSamples));
    std::vector<int16_t> cRaw(static_cast<size_t>(numSamples));
    yFile.read(reinterpret_cast<char *>(yRaw.data()), numBytes);
    cFile.read(reinterpret_cast<char *>(cRaw.data()), numBytes);
    if (yFile.gcount() != numBytes || cFile.gcount() != numBytes) {
        throw std::runtime_error("CvbsYcSource::readAndSynthesise(): short read");
    }

    // Synthesise a composite-shaped sample: luma in the canonical TBC
    // domain plus the centred chroma excursion (× 64). Clamp to uint16_t.
    Data out(static_cast<size_t>(numSamples));
    for (size_t i = 0; i < yRaw.size(); ++i) {
        const uint16_t luma = chd::format::convertCompositeSampleToCanonical(encoding, yRaw[i]);
        const int16_t  chroma = chd::format::convertChromaSampleToCenteredCanonical(encoding, cRaw[i]);
        const int32_t  combined = static_cast<int32_t>(luma) + static_cast<int32_t>(chroma);
        if (combined < 0) out[i] = 0;
        else if (combined > 65535) out[i] = 65535;
        else out[i] = static_cast<uint16_t>(combined);
    }
    return out;
}

}  // namespace chd::reader
