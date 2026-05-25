// SPDX-License-Identifier: GPL-3.0-or-later

#include "cvbs_composite_source.h"

#include <stdexcept>

#include "../common/log.h"

namespace chd::reader {

CvbsCompositeSource::CvbsCompositeSource() = default;
CvbsCompositeSource::~CvbsCompositeSource()
{
    if (isOpen) inputFile.close();
}

bool CvbsCompositeSource::open(const std::string &compositePath,
                               const chd::format::VideoStandardPreset &videoStandard,
                               chd::format::SampleEncoding sampleEncoding,
                               chd::format::SignalState signalState)
{
    if (isOpen) {
        chd::log::warn() << "CvbsCompositeSource::open(): source already open";
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

    inputFile.open(compositePath, std::ios::binary);
    if (!inputFile.is_open()) {
        chd::log::warn() << "CvbsCompositeSource::open(): could not open" << compositePath;
        return false;
    }
    inputFile.seekg(0, std::ios::end);
    fileSize = inputFile.tellg();
    inputFile.seekg(0, std::ios::beg);
    if (fileSize <= 0 || fieldByteSize <= 0) {
        chd::log::warn() << "CvbsCompositeSource::open(): invalid file or field size";
        inputFile.close();
        return false;
    }
    numFields = static_cast<int32_t>(fileSize / fieldByteSize);
    isOpen = true;
    chd::log::debug().nospace()
        << "CvbsCompositeSource::open(): " << compositePath << " has " << numFields
        << " fields (" << fieldWidth << "x" << fieldHeight << " samples/field)";
    return true;
}

void CvbsCompositeSource::close()
{
    if (!isOpen) return;
    inputFile.close();
    isOpen = false;
    fieldCache.clear();
}

const chd::metadata::LdDecodeMetaData::VideoParameters &CvbsCompositeSource::parameters() const
{
    return videoParameters;
}

chd::format::SignalState CvbsCompositeSource::signalState() const
{
    return state;
}

chd::format::SampleEncoding CvbsCompositeSource::sampleEncoding() const
{
    return encoding;
}

bool CvbsCompositeSource::isSourceValid() const
{
    return isOpen;
}

int32_t CvbsCompositeSource::getNumberOfAvailableFields() const
{
    return numFields;
}

int32_t CvbsCompositeSource::getFieldLength() const
{
    return fieldSamples;
}

CvbsCompositeSource::Data CvbsCompositeSource::getVideoField(int32_t fieldNumber,
                                                             int32_t startFieldLine,
                                                             int32_t endFieldLine)
{
    if (!isOpen) {
        throw std::runtime_error("CvbsCompositeSource::getVideoField(): source not open");
    }
    // 1-based field number, matching the canonical TBC convention.
    const int32_t fieldIndex = fieldNumber - 1;
    if (fieldIndex < 0 || fieldIndex >= numFields) {
        throw std::runtime_error("CvbsCompositeSource::getVideoField(): field out of range");
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
        // Lines are 1-based. Convert to 0-based and validate.
        const int32_t first0 = startFieldLine - 1;
        const int32_t last0  = endFieldLine - 1;
        if (first0 < 0 || last0 < first0 || last0 >= fieldHeight) {
            throw std::runtime_error("CvbsCompositeSource::getVideoField(): line range out of bounds");
        }
        const int64_t lineByteSize = static_cast<int64_t>(fieldWidth) * 2;
        startByte = static_cast<int64_t>(fieldByteSize) * fieldIndex + lineByteSize * first0;
        numBytes  = lineByteSize * (last0 - first0 + 1);
    }

    Data data = readAndConvert(startByte, numBytes);

    if (wholeField) {
        fieldCache.emplace(fieldIndex, data);
    }
    return data;
}

CvbsCompositeSource::Data CvbsCompositeSource::readAndConvert(int64_t startByte, int64_t numBytes)
{
    inputFile.clear();
    inputFile.seekg(startByte);
    if (!inputFile.good()) {
        throw std::runtime_error("CvbsCompositeSource::readAndConvert(): seek failed");
    }

    // Read the raw on-disk samples as int16_t (signedness handled by the
    // per-encoding converter; we use a signed buffer here so two's-complement
    // sign extension matches the spec for signed encodings).
    const int64_t numSamples = numBytes / 2;
    std::vector<int16_t> raw(static_cast<size_t>(numSamples));
    inputFile.read(reinterpret_cast<char *>(raw.data()), numBytes);
    if (inputFile.gcount() != numBytes) {
        throw std::runtime_error("CvbsCompositeSource::readAndConvert(): short read");
    }

    Data out(static_cast<size_t>(numSamples));
    for (size_t i = 0; i < raw.size(); ++i) {
        out[i] = chd::format::convertCompositeSampleToCanonical(encoding, raw[i]);
    }
    return out;
}

}  // namespace chd::reader
