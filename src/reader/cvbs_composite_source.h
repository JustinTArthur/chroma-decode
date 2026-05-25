// SPDX-License-Identifier: GPL-3.0-or-later
//
// CvbsCompositeSource — ISource implementation for the CVBS file format
// specification's `<basename>.composite` layout (single file containing
// luma+chroma combined into one signal).
//
// On open the source pairs the on-disk binary with a Video Standard Preset,
// Sample Encoding Preset, and Signal State Preset. Per-encoding amplitude
// conversion happens at load time so the decoder pipeline always sees
// canonical uint16_t samples in the canonical TBC convention (10-bit value × 64,
// blanking at 16384).
//
// For NTSC (910 × 525 orthogonal) and PAL_M (909 × 525 orthogonal) line-major
// slicing is trivial. For PAL (709,379 samples/frame, non-orthogonal) the
// reader treats the frame as if orthogonal at 1135 samples/line,
// matching the existing PALcolour assumption.

#ifndef CHD_READER_CVBS_COMPOSITE_SOURCE_H
#define CHD_READER_CVBS_COMPOSITE_SOURCE_H

#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../format/sample_encoding.h"
#include "../format/signal_state.h"
#include "../format/video_standards.h"
#include "../metadata/core.h"
#include "source.h"

namespace chd::reader {

class CvbsCompositeSource : public ISource
{
public:
    using Data = chd::reader::Data;

    CvbsCompositeSource();
    ~CvbsCompositeSource() override;

    // Open a composite file with the given preset triple. The Video Standard
    // Preset fixes the field geometry; the Sample Encoding Preset selects
    // amplitude conversion; the Signal State Preset controls whether
    // normative sample-count constraints apply and is reported back via
    // signalState().
    //
    // Returns true on success. On failure, the source is left invalid.
    bool open(const std::string &compositePath,
              const chd::format::VideoStandardPreset &videoStandard,
              chd::format::SampleEncoding sampleEncoding,
              chd::format::SignalState signalState);

    void close();

    // ISource implementation -------------------------------------------------
    const chd::metadata::LdDecodeMetaData::VideoParameters &parameters() const override;
    chd::format::SignalState     signalState()    const override;
    chd::format::SampleEncoding  sampleEncoding() const override;

    bool isSourceValid() const override;
    int32_t getNumberOfAvailableFields() const override;
    int32_t getFieldLength() const override;

    Data getVideoField(int32_t fieldNumber,
                       int32_t startFieldLine = -1,
                       int32_t endFieldLine = -1) override;

private:
    // Read the requested raw byte range from the file under ioMutex, then
    // return a freshly-allocated buffer of canonical-domain uint16_t samples
    // produced by per-encoding amplitude conversion.
    Data readAndConvert(int64_t startByte, int64_t numBytes);

    std::ifstream inputFile;
    bool isOpen = false;
    int64_t fileSize = 0;

    // Field geometry derived from the Video Standard Preset.
    int32_t fieldWidth = 0;          // samples per "line" (orthogonal-line simplification for PAL)
    int32_t fieldHeight = 0;         // lines per field (262/263 for NTSC/PAL_M; 312/313 for PAL)
    int32_t fieldSamples = 0;        // fieldWidth * fieldHeight
    int32_t fieldByteSize = 0;       // fieldSamples * 2

    int32_t numFields = 0;

    chd::format::VideoStandard          standardEnum;
    chd::format::SampleEncoding         encoding;
    chd::format::SignalState            state;
    chd::metadata::LdDecodeMetaData::VideoParameters videoParameters;

    // Whole-field cache (matches TbcSource's behaviour). Serialised by
    // ioMutex.
    std::unordered_map<int32_t, Data> fieldCache;
    mutable std::mutex ioMutex;
};

}  // namespace chd::reader

#endif  // CHD_READER_CVBS_COMPOSITE_SOURCE_H
