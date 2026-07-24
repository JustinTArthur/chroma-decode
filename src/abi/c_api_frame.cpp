// SPDX-License-Identifier: GPL-3.0-or-later
#include <chromadec/frame.h>

#include <cstring>

#include "../common/error_state.h"
#include "handles.h"

extern "C" {

namespace {

chd_status_t arg_error(const char *what) {
    chd::detail::set_last_error(std::string("chromadec: ") + what + ": null argument");
    return CHD_E_INVALID_ARG;
}

bool isFrame440(const chd_frame_t *f) {
    return f->format == CHD_PIXEL_YUV440P16 || f->format == CHD_PIXEL_YUV440PS;
}

}  // namespace

chd_status_t chd_frame_get_info(const chd_frame_t *f, chd_frame_info_t *out) {
    if (f == nullptr || out == nullptr) return arg_error("chd_frame_get_info");
    *out = f->info;
    return CHD_OK;
}

chd_status_t chd_frame_get_plane_info(const chd_frame_t *f, chd_plane_t plane,
                                      chd_plane_info_t *out) {
    if (f == nullptr || out == nullptr) return arg_error("chd_frame_get_plane_info");

    // Which planes exist for the frame's format.
    bool valid = false;
    switch (f->format) {
        case CHD_PIXEL_YUV444P16:
        case CHD_PIXEL_YUV444PS:
        case CHD_PIXEL_YUV440P16:
        case CHD_PIXEL_YUV440PS:
            valid = plane == CHD_PLANE_Y || plane == CHD_PLANE_CB || plane == CHD_PLANE_CR;
            break;
        case CHD_PIXEL_GRAY16:
        case CHD_PIXEL_GRAYS:
            valid = plane == CHD_PLANE_Y;
            break;
        case CHD_PIXEL_RGB48:
        case CHD_PIXEL_RGBS:
            valid = plane == CHD_PLANE_R || plane == CHD_PLANE_G || plane == CHD_PLANE_B;
            break;
    }
    if (!valid) {
        chd::detail::set_last_error(
            "chd_frame_get_plane_info: plane not valid for the frame's pixel format");
        return CHD_E_INVALID_ARG;
    }

    out->width = f->outputWidth;
    out->height = f->outputHeight;
    out->first_frame_row = 0;
    if (isFrame440(f) && plane == CHD_PLANE_CB) {
        out->height = f->chroma440.cbHeight;
        out->first_frame_row = f->chroma440.cbFirstRow;
    } else if (isFrame440(f) && plane == CHD_PLANE_CR) {
        out->height = f->chroma440.crHeight;
        out->first_frame_row = f->chroma440.crFirstRow;
    }
    return CHD_OK;
}

chd_status_t chd_frame_chroma_row_component(const chd_frame_t *f, int32_t frame_row,
                                            chd_chroma_row_component_t *out) {
    if (f == nullptr || out == nullptr) return arg_error("chd_frame_chroma_row_component");
    if (f->rowComponent.empty()) {
        chd::detail::set_last_error(
            "chd_frame_chroma_row_component: frame does not carry line-sequential chroma");
        return CHD_E_UNSUPPORTED;
    }
    if (frame_row < 0 || frame_row >= static_cast<int32_t>(f->rowComponent.size())) {
        chd::detail::set_last_error("chd_frame_chroma_row_component: frame_row out of range");
        return CHD_E_OUT_OF_RANGE;
    }
    const int8_t c = f->rowComponent[static_cast<size_t>(frame_row)];
    if (c != 0 && c != 1) {
        chd::detail::set_last_error(
            "chd_frame_chroma_row_component: no chroma was decoded for this row");
        return CHD_E_OUT_OF_RANGE;
    }
    *out = (c == 0) ? CHD_CHROMA_ROW_DB : CHD_CHROMA_ROW_DR;
    return CHD_OK;
}

chd_status_t chd_frame_get_chroma_ident(const chd_frame_t *f,
                                        chd_chroma_ident_report_t *out) {
    if (f == nullptr || out == nullptr) return arg_error("chd_frame_get_chroma_ident");
    if (!f->hasIdentReport) {
        chd::detail::set_last_error(
            "chd_frame_get_chroma_ident: frame does not carry line-sequential chroma");
        return CHD_E_UNSUPPORTED;
    }
    *out = f->identReport;
    return CHD_OK;
}

chd_status_t chd_frame_get_plane(const chd_frame_t *f, chd_plane_t plane,
                                  const void **out_data,
                                  ptrdiff_t *out_stride_bytes) {
    if (f == nullptr || out_data == nullptr || out_stride_bytes == nullptr) {
        return arg_error("chd_frame_get_plane");
    }
    *out_data = nullptr;
    *out_stride_bytes = 0;

    const int32_t w = f->outputWidth;
    const int32_t h = f->outputHeight;

    switch (f->format) {
        case CHD_PIXEL_YUV444P16: {
            // Layout written by OutputWriter::convert: [Y plane | Cb plane | Cr plane],
            // each of size w*h u16 samples.
            const uint16_t *base = f->u16Plane.data();
            const ptrdiff_t planeStride = static_cast<ptrdiff_t>(w) * sizeof(uint16_t);
            switch (plane) {
                case CHD_PLANE_Y:  *out_data = base;                       break;
                case CHD_PLANE_CB: *out_data = base + static_cast<size_t>(w) * h;     break;
                case CHD_PLANE_CR: *out_data = base + 2 * static_cast<size_t>(w) * h; break;
                default:
                    chd::detail::set_last_error("chd_frame_get_plane: plane not valid for YUV444P16");
                    return CHD_E_INVALID_ARG;
            }
            *out_stride_bytes = planeStride;
            return CHD_OK;
        }
        case CHD_PIXEL_GRAY16: {
            if (plane != CHD_PLANE_Y) {
                chd::detail::set_last_error("chd_frame_get_plane: GRAY16 only has CHD_PLANE_Y");
                return CHD_E_INVALID_ARG;
            }
            *out_data = f->u16Plane.data();
            *out_stride_bytes = static_cast<ptrdiff_t>(w) * sizeof(uint16_t);
            return CHD_OK;
        }
        case CHD_PIXEL_YUV440P16: {
            // Layout written by OutputWriter::convert440:
            // [Y w*h | Cb w*cbHeight | Cr w*crHeight].
            const uint16_t *base = f->u16Plane.data();
            switch (plane) {
                case CHD_PLANE_Y:  *out_data = base; break;
                case CHD_PLANE_CB: *out_data = base + static_cast<size_t>(w) * h; break;
                case CHD_PLANE_CR:
                    *out_data = base + static_cast<size_t>(w) * (h + f->chroma440.cbHeight);
                    break;
                default:
                    chd::detail::set_last_error(
                        "chd_frame_get_plane: plane not valid for YUV440P16");
                    return CHD_E_INVALID_ARG;
            }
            *out_stride_bytes = static_cast<ptrdiff_t>(w) * sizeof(uint16_t);
            return CHD_OK;
        }
        case CHD_PIXEL_RGB48: {
            // Packed interleaved RGB48: each row is w * 3 u16 samples. The
            // R/G/B accessors return the same buffer with a per-channel
            // byte offset of 0/2/4; stride is the row pitch in bytes,
            // identical for all three planes. The caller advances by
            // 3 u16s (= 6 bytes) per pixel within a row.
            const uint16_t *base = f->u16Plane.data();
            const ptrdiff_t rowStride = static_cast<ptrdiff_t>(w) * 3 * sizeof(uint16_t);
            switch (plane) {
                case CHD_PLANE_R: *out_data = base;     break;
                case CHD_PLANE_G: *out_data = base + 1; break;
                case CHD_PLANE_B: *out_data = base + 2; break;
                default:
                    chd::detail::set_last_error("chd_frame_get_plane: plane not valid for RGB48");
                    return CHD_E_INVALID_ARG;
            }
            *out_stride_bytes = rowStride;
            return CHD_OK;
        }
        case CHD_PIXEL_YUV444PS:
        case CHD_PIXEL_RGBS:
        case CHD_PIXEL_GRAYS:
        case CHD_PIXEL_YUV440PS:
            chd::detail::set_last_error(
                "chd_frame_get_plane: use chd_frame_get_plane_float for float pixel formats");
            return CHD_E_INVALID_ARG;
    }
    chd::detail::set_last_error("chd_frame_get_plane: unknown pixel format");
    return CHD_E_INTERNAL;
}

chd_status_t chd_frame_get_plane_float(const chd_frame_t *f, chd_plane_t plane,
                                        const float **out_data,
                                        ptrdiff_t *out_stride_bytes) {
    if (f == nullptr || out_data == nullptr || out_stride_bytes == nullptr) {
        return arg_error("chd_frame_get_plane_float");
    }
    if (f->format != CHD_PIXEL_YUV444PS && f->format != CHD_PIXEL_GRAYS
        && f->format != CHD_PIXEL_RGBS && f->format != CHD_PIXEL_YUV440PS) {
        chd::detail::set_last_error(
            "chd_frame_get_plane_float: frame is not a float pixel format");
        return CHD_E_INVALID_ARG;
    }
    if (f->format == CHD_PIXEL_GRAYS && plane != CHD_PLANE_Y) {
        chd::detail::set_last_error(
            "chd_frame_get_plane_float: CHD_PIXEL_GRAYS only has CHD_PLANE_Y");
        return CHD_E_INVALID_ARG;
    }
    int32_t idx = -1;
    if (f->format == CHD_PIXEL_RGBS) {
        switch (plane) {
            case CHD_PLANE_R: idx = 0; break;
            case CHD_PLANE_G: idx = 1; break;
            case CHD_PLANE_B: idx = 2; break;
            default:
                chd::detail::set_last_error(
                    "chd_frame_get_plane_float: CHD_PIXEL_RGBS plane must be R, G, or B");
                return CHD_E_INVALID_ARG;
        }
    } else {
        switch (plane) {
            case CHD_PLANE_Y:  idx = 0; break;
            case CHD_PLANE_CB: idx = 1; break;
            case CHD_PLANE_CR: idx = 2; break;
            default:
                chd::detail::set_last_error(
                    "chd_frame_get_plane_float: Y'CbCr float frame plane must be Y, Cb, or Cr");
                return CHD_E_INVALID_ARG;
        }
    }
    *out_data = f->floatPlane[idx].data();
    *out_stride_bytes = static_cast<ptrdiff_t>(f->outputWidth) * sizeof(float);
    return CHD_OK;
}

void chd_frame_free(chd_frame_t *f) {
    delete f;
}

}  // extern "C"
