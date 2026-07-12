// SPDX-License-Identifier: GPL-3.0-or-later

use std::marker::PhantomData;
use std::ptr::{self, NonNull};
use std::slice;

use chromadec_sys as sys;

use crate::error::{Error, Result, check};
use crate::types::{
    ChromaIdentMechanism, ChromaIdentReport, ChromaRowComponent, FrameInfo, PixelFormat, Plane,
    PlaneInfo,
};

/// A decoded frame (`chd_frame_t`). Owns its pixel data; plane accessors
/// borrow from it zero-copy.
#[derive(Debug)]
pub struct Frame {
    raw: NonNull<sys::chd_frame>,
}

// The frame's pixel data is immutable after decode.
unsafe impl Send for Frame {}
unsafe impl Sync for Frame {}

impl Frame {
    pub(crate) fn from_raw(raw: NonNull<sys::chd_frame>) -> Frame {
        Frame { raw }
    }

    pub fn info(&self) -> Result<FrameInfo> {
        let mut raw = sys::chd_frame_info::default();
        unsafe { check(sys::chd_frame_get_info(self.raw.as_ptr(), &mut raw))? };
        Ok(FrameInfo {
            format: PixelFormat::from_raw(raw.format)?,
            width: raw.width,
            height: raw.height,
            num_planes: raw.num_planes,
            frame_index: raw.frame_index,
        })
    }

    /// Per-plane geometry (`chd_frame_get_plane_info`). The 4:4:0 formats
    /// are the reason to call it: their chroma planes are shorter than the
    /// frame and differ from each other by up to one row.
    pub fn plane_info(&self, plane: Plane) -> Result<PlaneInfo> {
        let mut raw = sys::chd_plane_info::default();
        unsafe {
            check(sys::chd_frame_get_plane_info(
                self.raw.as_ptr(),
                plane.raw(),
                &mut raw,
            ))?;
        }
        Ok(PlaneInfo {
            width: raw.width,
            height: raw.height,
            first_frame_row: raw.first_frame_row,
        })
    }

    /// Zero-copy view of a 16-bit plane (integer pixel formats).
    pub fn plane_u16(&self, plane: Plane) -> Result<PlaneView<'_, u16>> {
        let info = self.plane_info(plane)?;
        let mut data: *const std::ffi::c_void = ptr::null();
        let mut stride: isize = 0;
        unsafe {
            check(sys::chd_frame_get_plane(
                self.raw.as_ptr(),
                plane.raw(),
                &mut data,
                &mut stride,
            ))?;
        }
        PlaneView::new(data as *const u16, stride, &info)
    }

    /// Zero-copy view of a float plane (float pixel formats): normalized
    /// E′Y/E′Cb/E′Cr or E′R/E′G/E′B per the committed output format.
    pub fn plane_f32(&self, plane: Plane) -> Result<PlaneView<'_, f32>> {
        let info = self.plane_info(plane)?;
        let mut data: *const f32 = ptr::null();
        let mut stride: isize = 0;
        unsafe {
            check(sys::chd_frame_get_plane_float(
                self.raw.as_ptr(),
                plane.raw(),
                &mut data,
                &mut stride,
            ))?;
        }
        PlaneView::new(data, stride, &info)
    }

    /// Colour-difference component decoded at output frame row `frame_row`
    /// (`chd_frame_chroma_row_component`). Line-sequential (4:4:0) frames
    /// only.
    pub fn chroma_row_component(&self, frame_row: i32) -> Result<ChromaRowComponent> {
        let mut raw = sys::chd_chroma_row_component::CHD_CHROMA_ROW_DB;
        unsafe {
            check(sys::chd_frame_chroma_row_component(
                self.raw.as_ptr(),
                frame_row,
                &mut raw,
            ))?;
        }
        ChromaRowComponent::from_raw(raw)
    }

    /// Per-frame Db/Dr ident summary (`chd_frame_get_chroma_ident`).
    /// Line-sequential (4:4:0) frames only.
    pub fn chroma_ident(&self) -> Result<ChromaIdentReport> {
        let mut raw = sys::chd_chroma_ident_report::default();
        unsafe {
            check(sys::chd_frame_get_chroma_ident(self.raw.as_ptr(), &mut raw))?;
        }
        let mechanism = match raw.mechanism {
            sys::chd_chroma_ident_mechanism::CHD_CHROMA_IDENT_PORCH => {
                ChromaIdentMechanism::Porch
            }
            sys::chd_chroma_ident_mechanism::CHD_CHROMA_IDENT_BOTTLES => {
                ChromaIdentMechanism::Bottles
            }
            sys::chd_chroma_ident_mechanism::CHD_CHROMA_IDENT_CONTENT => {
                ChromaIdentMechanism::Content
            }
            sys::chd_chroma_ident_mechanism::CHD_CHROMA_IDENT_MANUAL => {
                ChromaIdentMechanism::Manual
            }
            other => {
                return Err(Error::internal(&format!(
                    "unrecognized chd_chroma_ident_mechanism value {}",
                    other.0
                )));
            }
        };
        Ok(ChromaIdentReport {
            mechanism,
            confidence: raw.confidence,
            field_confidence: raw.field_confidence,
            first_row_component: ChromaRowComponent::from_raw(raw.first_row_component)?,
        })
    }
}

impl Drop for Frame {
    fn drop(&mut self) {
        unsafe { sys::chd_frame_free(self.raw.as_ptr()) };
    }
}

/// Borrowed view of one plane of a [`Frame`]. Rows are `width` samples;
/// consecutive rows are `stride_bytes` apart in memory.
pub struct PlaneView<'f, T> {
    data: *const T,
    stride_bytes: isize,
    width: usize,
    height: usize,
    _frame: PhantomData<&'f Frame>,
}

unsafe impl<T: Sync> Send for PlaneView<'_, T> {}
unsafe impl<T: Sync> Sync for PlaneView<'_, T> {}

impl<'f, T: 'f> PlaneView<'f, T> {
    fn new(data: *const T, stride_bytes: isize, info: &PlaneInfo) -> Result<PlaneView<'f, T>> {
        if data.is_null() {
            return Err(Error::internal(
                "library returned CHD_OK with a null plane pointer",
            ));
        }
        if stride_bytes < 0 || !data.is_aligned() {
            return Err(Error::internal("plane data is not viewable as typed rows"));
        }
        Ok(PlaneView {
            data,
            stride_bytes,
            width: info.width as usize,
            height: info.height as usize,
            _frame: PhantomData,
        })
    }

    pub fn width(&self) -> usize {
        self.width
    }

    pub fn height(&self) -> usize {
        self.height
    }

    pub fn stride_bytes(&self) -> isize {
        self.stride_bytes
    }

    /// One row of samples. Panics if `y >= height()`.
    pub fn row(&self, y: usize) -> &'f [T] {
        assert!(
            y < self.height,
            "row {y} out of range (height {})",
            self.height
        );
        unsafe {
            let row = self.data.byte_offset(self.stride_bytes * y as isize);
            slice::from_raw_parts(row, self.width)
        }
    }

    /// Iterates over rows, top to bottom.
    pub fn rows(&self) -> impl Iterator<Item = &'f [T]> + '_ {
        (0..self.height).map(move |y| self.row(y))
    }
}
