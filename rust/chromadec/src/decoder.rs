// SPDX-License-Identifier: GPL-3.0-or-later

use std::ffi::{CString, c_void};
use std::marker::PhantomData;
use std::panic::{self, AssertUnwindSafe};
use std::ptr::{self, NonNull};
use std::sync::Mutex;

use chromadec_sys as sys;

use crate::cancel::Cancel;
use crate::error::{Error, Result, check};
use crate::frame::Frame;
use crate::nn::NnModel;
use crate::types::{DecoderKind, OutputInfo, PixelFormat};
use crate::util::non_null;
use crate::video::Video;

/// Dropout correction options (`chd_dropout_opts_t`).
#[derive(Clone, Copy, Debug, Default)]
pub struct DropoutOpts {
    pub enabled: bool,
    /// Extend dropout boundaries by ±24 samples.
    pub overcorrect: bool,
    /// Skip cross-field replacement candidates.
    pub intra_field_only: bool,
}

/// Dropout correction stats for one decoded frame (`chd_dropout_stats_t`).
#[derive(Clone, Copy, Debug, Default)]
#[non_exhaustive]
pub struct DropoutStats {
    pub corrected: i32,
    pub failed: i32,
    pub total_distance: i64,
}

/// A run of dropped samples on one output row (`chd_dropout_span_t`):
/// `x_start..x_end` (half-open) on row `y`, in the committed output framing.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct DropoutSpan {
    pub y: i32,
    pub x_start: i32,
    pub x_end: i32,
}

/// Which dropout regions a detection query reports
/// (`chd_dropout_detect_mode_t`).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum DropoutDetectMode {
    /// Raw flagged regions.
    Detected,
    /// Detected regions widened by the overcorrect margin.
    Overcorrect,
}

impl DropoutDetectMode {
    fn raw(self) -> sys::chd_dropout_detect_mode {
        match self {
            DropoutDetectMode::Detected => sys::chd_dropout_detect_mode::CHD_DROPOUT_DETECTED,
            DropoutDetectMode::Overcorrect => sys::chd_dropout_detect_mode::CHD_DROPOUT_OVERCORRECT,
        }
    }
}

/// A decoder bound to a [`Video`] (`chd_decoder_t`). The video is mutably
/// borrowed for the decoder's lifetime — the C handle keeps a non-owning
/// pointer to it.
///
/// Configure with the option setters, then [`commit`](Decoder::commit), then
/// decode. Options are rejected after commit.
#[derive(Debug)]
pub struct Decoder<'v> {
    raw: NonNull<sys::chd_decoder>,
    _video: PhantomData<&'v mut Video>,
}

unsafe impl Send for Decoder<'_> {}

impl<'v> Decoder<'v> {
    pub fn new(video: &'v mut Video, kind: DecoderKind) -> Result<Decoder<'v>> {
        let mut raw = ptr::null_mut();
        unsafe {
            check(sys::chd_decoder_create(
                video.as_ptr(),
                kind.raw(),
                &mut raw,
            ))?
        };
        Ok(Decoder {
            raw: non_null(raw)?,
            _video: PhantomData,
        })
    }

    pub fn set_option_f64(&mut self, name: &str, value: f64) -> Result<()> {
        let name = option_name(name)?;
        unsafe {
            check(sys::chd_decoder_set_option_f64(
                self.raw.as_ptr(),
                name.as_ptr(),
                value,
            ))
        }
    }

    pub fn set_option_i32(&mut self, name: &str, value: i32) -> Result<()> {
        let name = option_name(name)?;
        unsafe {
            check(sys::chd_decoder_set_option_i32(
                self.raw.as_ptr(),
                name.as_ptr(),
                value,
            ))
        }
    }

    pub fn set_option_bool(&mut self, name: &str, value: bool) -> Result<()> {
        let name = option_name(name)?;
        unsafe {
            check(sys::chd_decoder_set_option_bool(
                self.raw.as_ptr(),
                name.as_ptr(),
                value as _,
            ))
        }
    }

    pub fn set_option_str(&mut self, name: &str, value: &str) -> Result<()> {
        let name = option_name(name)?;
        let value = CString::new(value)
            .map_err(|_| Error::internal("option value contains an interior NUL byte"))?;
        unsafe {
            check(sys::chd_decoder_set_option_str(
                self.raw.as_ptr(),
                name.as_ptr(),
                value.as_ptr(),
            ))
        }
    }

    /// Whether an option name is meaningful for this decoder's kind.
    pub fn has_option(&self, name: &str) -> bool {
        let Ok(name) = option_name(name) else {
            return false;
        };
        let rc = unsafe { sys::chd_decoder_has_option(self.raw.as_ptr(), name.as_ptr()) };
        rc == sys::chd_status::CHD_OK
    }

    /// Binds an NN model (`chd_decoder_set_nn_model`). The decoder takes its
    /// own reference; the model handle may be dropped afterwards.
    pub fn set_nn_model(&mut self, model: &NnModel) -> Result<()> {
        unsafe {
            check(sys::chd_decoder_set_nn_model(
                self.raw.as_ptr(),
                model.as_ptr(),
            ))
        }
    }

    pub fn set_dropout(&mut self, opts: &DropoutOpts) -> Result<()> {
        // `..default()` zero-initialises the reserved ABI-extension field.
        let raw = sys::chd_dropout_opts {
            enabled: opts.enabled as _,
            overcorrect: opts.overcorrect as _,
            intra_field_only: opts.intra_field_only as _,
            ..Default::default()
        };
        unsafe { check(sys::chd_decoder_set_dropout(self.raw.as_ptr(), &raw)) }
    }

    /// Applies pending options and builds the decode engines
    /// (`chd_decoder_commit`). Required before decoding.
    pub fn commit(&mut self) -> Result<()> {
        unsafe { check(sys::chd_decoder_commit(self.raw.as_ptr())) }
    }

    /// Committed output framing (`chd_decoder_get_output_info`).
    pub fn output_info(&self) -> Result<OutputInfo> {
        let mut raw = sys::chd_output_info::default();
        unsafe {
            check(sys::chd_decoder_get_output_info(
                self.raw.as_ptr(),
                &mut raw,
            ))?
        };
        Ok(OutputInfo {
            format: PixelFormat::from_raw(raw.format)?,
            width: raw.width,
            height: raw.height,
            num_planes: raw.num_planes,
            num_frames: raw.num_frames,
        })
    }

    pub fn decode_frame(&mut self, frame_index: i64) -> Result<Frame> {
        let mut raw = ptr::null_mut();
        unsafe {
            check(sys::chd_decode_frame(
                self.raw.as_ptr(),
                frame_index,
                &mut raw,
            ))?
        };
        Ok(Frame::from_raw(non_null(raw)?))
    }

    /// Decodes a batch of frames on the decoder's worker pool
    /// (`chd_decode_frames_async`). Blocks until every index has been
    /// reported to `on_frame`, which is invoked concurrently from worker
    /// threads and in completion order, not index order. A cancel handle
    /// makes still-queued frames report [`Status::Cancelled`].
    ///
    /// [`Status::Cancelled`]: crate::Status::Cancelled
    pub fn decode_frames<F>(
        &mut self,
        frame_indices: &[i64],
        cancel: Option<&Cancel>,
        on_frame: F,
    ) -> Result<()>
    where
        F: Fn(i64, Result<Frame>) + Sync,
    {
        struct CbCtx<F> {
            on_frame: F,
            panic: Mutex<Option<Box<dyn std::any::Any + Send>>>,
        }

        unsafe extern "C" fn trampoline<F: Fn(i64, Result<Frame>) + Sync>(
            user: *mut c_void,
            status: sys::chd_status,
            frame_index: i64,
            frame: *mut sys::chd_frame,
        ) {
            let ctx = unsafe { &*(user as *const CbCtx<F>) };
            // Take ownership of the frame before anything can unwind, so it
            // is freed on every path.
            let frame = NonNull::new(frame).map(Frame::from_raw);
            let result = match (status == sys::chd_status::CHD_OK, frame) {
                (true, Some(frame)) => Ok(frame),
                (true, None) => Err(Error::internal("decode reported CHD_OK with a null frame")),
                (false, _) => Err(Error::from_raw(status)),
            };
            // The callback must not unwind into C; stash the panic and
            // re-raise it after the C call returns.
            if let Err(payload) =
                panic::catch_unwind(AssertUnwindSafe(|| (ctx.on_frame)(frame_index, result)))
            {
                *ctx.panic.lock().unwrap() = Some(payload);
            }
        }

        let ctx = CbCtx {
            on_frame,
            panic: Mutex::new(None),
        };
        let rc = unsafe {
            sys::chd_decode_frames_async(
                self.raw.as_ptr(),
                frame_indices.as_ptr(),
                frame_indices.len(),
                Some(trampoline::<F>),
                &ctx as *const CbCtx<F> as *mut c_void,
                cancel.map_or(ptr::null_mut(), Cancel::as_ptr),
            )
        };
        if let Some(payload) = ctx.panic.into_inner().unwrap() {
            panic::resume_unwind(payload);
        }
        check(rc)
    }

    /// Dropout stats from the most recent `decode_frame` on this decoder.
    pub fn last_dropout_stats(&self) -> Result<DropoutStats> {
        let mut raw = sys::chd_dropout_stats::default();
        unsafe {
            check(sys::chd_decoder_get_last_dropout_stats(
                self.raw.as_ptr(),
                &mut raw,
            ))?
        };
        Ok(DropoutStats {
            corrected: raw.corrected,
            failed: raw.failed,
            total_distance: raw.total_distance,
        })
    }

    /// Dropout spans for one frame without running the chroma decoder
    /// (`chd_decoder_get_dropout_spans`). Requires a committed decoder.
    pub fn dropout_spans(
        &mut self,
        frame_index: i64,
        mode: DropoutDetectMode,
    ) -> Result<Vec<DropoutSpan>> {
        let mut spans: *mut sys::chd_dropout_span = ptr::null_mut();
        let mut count: usize = 0;
        unsafe {
            check(sys::chd_decoder_get_dropout_spans(
                self.raw.as_ptr(),
                frame_index,
                mode.raw(),
                &mut spans,
                &mut count,
            ))?;
        }
        let mut out = Vec::with_capacity(count);
        if !spans.is_null() {
            for raw in unsafe { std::slice::from_raw_parts(spans, count) } {
                out.push(DropoutSpan {
                    y: raw.y,
                    x_start: raw.x_start,
                    x_end: raw.x_end,
                });
            }
            unsafe { sys::chd_dropout_spans_free(spans) };
        }
        Ok(out)
    }

    /// The same dropout regions rasterised into a single-plane mask frame
    /// (`chd_decode_dropout_mask`). Does not run the chroma decoder.
    pub fn dropout_mask(&mut self, frame_index: i64, mode: DropoutDetectMode) -> Result<Frame> {
        let mut raw = ptr::null_mut();
        unsafe {
            check(sys::chd_decode_dropout_mask(
                self.raw.as_ptr(),
                frame_index,
                mode.raw(),
                &mut raw,
            ))?;
        }
        Ok(Frame::from_raw(non_null(raw)?))
    }
}

impl Drop for Decoder<'_> {
    fn drop(&mut self) {
        unsafe { sys::chd_decoder_free(self.raw.as_ptr()) };
    }
}

fn option_name(name: &str) -> Result<CString> {
    CString::new(name).map_err(|_| Error::internal("option name contains an interior NUL byte"))
}
