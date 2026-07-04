// SPDX-License-Identifier: GPL-3.0-or-later

use std::path::Path;
use std::ptr::{self, NonNull};

use chromadec_sys as sys;

use crate::ensure_init;
use crate::error::{Result, check};
use crate::types::{VideoInfo, VideoParams};
use crate::util::{non_null, path_to_cstring};

/// An open composite video source (`chd_video_t`).
#[derive(Debug)]
pub struct Video {
    raw: NonNull<sys::chd_video>,
}

// The handle owns its source exclusively; moving it between threads is fine.
unsafe impl Send for Video {}

impl Video {
    /// Opens a single-file composite capture (`chd_video_open_composite`): an
    /// ld-decode `.tbc` or a CVBS `.composite`. With no sidecar path the
    /// library auto-locates an ld-decode `<path>.db`/`<path>.json` or a CVBS
    /// `<basename>.meta`, detecting the flavour automatically. See
    /// [`VideoParams`] for how `params` interacts with sidecar metadata.
    pub fn open_composite(
        path: impl AsRef<Path>,
        sidecar_path: Option<&Path>,
        params: Option<&VideoParams>,
    ) -> Result<Video> {
        ensure_init()?;
        let path = path_to_cstring(path.as_ref())?;
        let sidecar = sidecar_path.map(path_to_cstring).transpose()?;
        let raw_params = params.map(VideoParams::raw);
        let mut raw = ptr::null_mut();
        unsafe {
            check(sys::chd_video_open_composite(
                path.as_ptr(),
                sidecar.as_ref().map_or(ptr::null(), |s| s.as_ptr()),
                raw_params.as_ref().map_or(ptr::null(), |p| p),
                &mut raw,
            ))?;
        }
        Ok(Video {
            raw: non_null(raw)?,
        })
    }

    /// Opens a dual-file Y/C capture (`chd_video_open_yc`): a CVBS `.y`/`.c`
    /// pair or an ld-decode luma `.tbc` + chroma `.tbc` pair. Sidecar
    /// resolution follows [`Video::open_composite`].
    pub fn open_yc(
        luma_path: impl AsRef<Path>,
        chroma_path: impl AsRef<Path>,
        sidecar_path: Option<&Path>,
        params: Option<&VideoParams>,
    ) -> Result<Video> {
        ensure_init()?;
        let luma = path_to_cstring(luma_path.as_ref())?;
        let chroma = path_to_cstring(chroma_path.as_ref())?;
        let sidecar = sidecar_path.map(path_to_cstring).transpose()?;
        let raw_params = params.map(VideoParams::raw);
        let mut raw = ptr::null_mut();
        unsafe {
            check(sys::chd_video_open_yc(
                luma.as_ptr(),
                chroma.as_ptr(),
                sidecar.as_ref().map_or(ptr::null(), |s| s.as_ptr()),
                raw_params.as_ref().map_or(ptr::null(), |p| p),
                &mut raw,
            ))?;
        }
        Ok(Video {
            raw: non_null(raw)?,
        })
    }

    pub fn info(&self) -> Result<VideoInfo> {
        let mut raw = sys::chd_video_info::default();
        unsafe { check(sys::chd_video_get_info(self.raw.as_ptr(), &mut raw))? };
        Ok(VideoInfo::from_raw(&raw))
    }

    /// Adds an extra composite source for multi-source dropout correction.
    pub fn add_extra_source_composite(
        &mut self,
        path: impl AsRef<Path>,
        sidecar_path: Option<&Path>,
    ) -> Result<()> {
        let path = path_to_cstring(path.as_ref())?;
        let sidecar = sidecar_path.map(path_to_cstring).transpose()?;
        unsafe {
            check(sys::chd_video_add_extra_source_composite(
                self.raw.as_ptr(),
                path.as_ptr(),
                sidecar.as_ref().map_or(ptr::null(), |s| s.as_ptr()),
            ))
        }
    }

    /// Adds an extra Y/C-pair source for multi-source dropout correction.
    pub fn add_extra_source_yc(
        &mut self,
        luma_path: impl AsRef<Path>,
        chroma_path: impl AsRef<Path>,
        sidecar_path: Option<&Path>,
    ) -> Result<()> {
        let luma = path_to_cstring(luma_path.as_ref())?;
        let chroma = path_to_cstring(chroma_path.as_ref())?;
        let sidecar = sidecar_path.map(path_to_cstring).transpose()?;
        unsafe {
            check(sys::chd_video_add_extra_source_yc(
                self.raw.as_ptr(),
                luma.as_ptr(),
                chroma.as_ptr(),
                sidecar.as_ref().map_or(ptr::null(), |s| s.as_ptr()),
            ))
        }
    }

    pub(crate) fn as_ptr(&self) -> *mut sys::chd_video {
        self.raw.as_ptr()
    }
}

impl Drop for Video {
    fn drop(&mut self) {
        unsafe { sys::chd_video_free(self.raw.as_ptr()) };
    }
}
