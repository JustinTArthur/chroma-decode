// SPDX-License-Identifier: GPL-3.0-or-later

use chromadec_sys as sys;

use crate::error::{Error, Result};

/// Decoder kind (`chd_decoder_kind_t`).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[non_exhaustive]
pub enum DecoderKind {
    Auto,
    Mono,
    Ntsc1d,
    Ntsc2d,
    Ntsc3d,
    Ntsc3dNoAdapt,
    Pal2d,
    Transform2d,
    Transform3d,
    NnTransform3d,
    LdzeugColorCnn,
    LdzeugLumaSep,
    LdzeugLumaSepFrame,
    /// Geometry/metadata only: no chroma decode engines are built and frame
    /// decoding is rejected, but output-info and dropout span/mask queries
    /// work.
    None,
}

impl DecoderKind {
    pub(crate) fn raw(self) -> sys::chd_decoder_kind {
        match self {
            DecoderKind::Auto => sys::chd_decoder_kind::CHD_DEC_AUTO,
            DecoderKind::Mono => sys::chd_decoder_kind::CHD_DEC_MONO,
            DecoderKind::Ntsc1d => sys::chd_decoder_kind::CHD_DEC_NTSC_1D,
            DecoderKind::Ntsc2d => sys::chd_decoder_kind::CHD_DEC_NTSC_2D,
            DecoderKind::Ntsc3d => sys::chd_decoder_kind::CHD_DEC_NTSC_3D,
            DecoderKind::Ntsc3dNoAdapt => sys::chd_decoder_kind::CHD_DEC_NTSC_3D_NO_ADAPT,
            DecoderKind::Pal2d => sys::chd_decoder_kind::CHD_DEC_PAL_2D,
            DecoderKind::Transform2d => sys::chd_decoder_kind::CHD_DEC_TRANSFORM_2D,
            DecoderKind::Transform3d => sys::chd_decoder_kind::CHD_DEC_TRANSFORM_3D,
            DecoderKind::NnTransform3d => sys::chd_decoder_kind::CHD_DEC_NN_TRANSFORM3D,
            DecoderKind::LdzeugColorCnn => sys::chd_decoder_kind::CHD_DEC_LDZEUG_COLOR_CNN,
            DecoderKind::LdzeugLumaSep => sys::chd_decoder_kind::CHD_DEC_LDZEUG_LUMA_SEP,
            DecoderKind::LdzeugLumaSepFrame => sys::chd_decoder_kind::CHD_DEC_LDZEUG_LUMA_SEP_FRAME,
            DecoderKind::None => sys::chd_decoder_kind::CHD_DEC_NONE,
        }
    }
}

/// Video standard (`chd_video_standard_t`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
#[non_exhaustive]
pub enum VideoStandard {
    #[default]
    Unknown,
    Ntsc,
    Pal,
    PalM,
}

impl VideoStandard {
    pub(crate) fn raw(self) -> sys::chd_video_standard {
        match self {
            VideoStandard::Unknown => sys::chd_video_standard::CHD_STD_UNKNOWN,
            VideoStandard::Ntsc => sys::chd_video_standard::CHD_STD_NTSC,
            VideoStandard::Pal => sys::chd_video_standard::CHD_STD_PAL,
            VideoStandard::PalM => sys::chd_video_standard::CHD_STD_PAL_M,
        }
    }

    pub(crate) fn from_raw(raw: sys::chd_video_standard) -> VideoStandard {
        match raw {
            sys::chd_video_standard::CHD_STD_NTSC => VideoStandard::Ntsc,
            sys::chd_video_standard::CHD_STD_PAL => VideoStandard::Pal,
            sys::chd_video_standard::CHD_STD_PAL_M => VideoStandard::PalM,
            _ => VideoStandard::Unknown,
        }
    }
}

/// Sample encoding (`chd_sample_encoding_t`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
#[non_exhaustive]
pub enum SampleEncoding {
    #[default]
    Unknown,
    CvbsU10_4fsc,
    CvbsU16_4fsc,
    RawS16_28m,
    RawS16_40m,
    CvbsTpg21_4fsc,
}

impl SampleEncoding {
    pub(crate) fn raw(self) -> sys::chd_sample_encoding {
        match self {
            SampleEncoding::Unknown => sys::chd_sample_encoding::CHD_ENC_UNKNOWN,
            SampleEncoding::CvbsU10_4fsc => sys::chd_sample_encoding::CHD_ENC_CVBS_U10_4FSC,
            SampleEncoding::CvbsU16_4fsc => sys::chd_sample_encoding::CHD_ENC_CVBS_U16_4FSC,
            SampleEncoding::RawS16_28m => sys::chd_sample_encoding::CHD_ENC_RAW_S16_28M,
            SampleEncoding::RawS16_40m => sys::chd_sample_encoding::CHD_ENC_RAW_S16_40M,
            SampleEncoding::CvbsTpg21_4fsc => sys::chd_sample_encoding::CHD_ENC_CVBS_TPG21_4FSC,
        }
    }

    pub(crate) fn from_raw(raw: sys::chd_sample_encoding) -> SampleEncoding {
        match raw {
            sys::chd_sample_encoding::CHD_ENC_CVBS_U10_4FSC => SampleEncoding::CvbsU10_4fsc,
            sys::chd_sample_encoding::CHD_ENC_CVBS_U16_4FSC => SampleEncoding::CvbsU16_4fsc,
            sys::chd_sample_encoding::CHD_ENC_RAW_S16_28M => SampleEncoding::RawS16_28m,
            sys::chd_sample_encoding::CHD_ENC_RAW_S16_40M => SampleEncoding::RawS16_40m,
            sys::chd_sample_encoding::CHD_ENC_CVBS_TPG21_4FSC => SampleEncoding::CvbsTpg21_4fsc,
            _ => SampleEncoding::Unknown,
        }
    }
}

/// Signal state (`chd_signal_state_t`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
#[non_exhaustive]
pub enum SignalState {
    #[default]
    Unknown,
    StandardTbcLocked,
    StandardTbcUnlocked,
    StandardRaw,
    NonstandardTbcLocked,
    NonstandardTbcUnlocked,
    NonstandardRaw,
}

impl SignalState {
    pub(crate) fn raw(self) -> sys::chd_signal_state {
        match self {
            SignalState::Unknown => sys::chd_signal_state::CHD_SIG_UNKNOWN,
            SignalState::StandardTbcLocked => sys::chd_signal_state::CHD_SIG_STANDARD_TBC_LOCKED,
            SignalState::StandardTbcUnlocked => {
                sys::chd_signal_state::CHD_SIG_STANDARD_TBC_UNLOCKED
            }
            SignalState::StandardRaw => sys::chd_signal_state::CHD_SIG_STANDARD_RAW,
            SignalState::NonstandardTbcLocked => {
                sys::chd_signal_state::CHD_SIG_NONSTANDARD_TBC_LOCKED
            }
            SignalState::NonstandardTbcUnlocked => {
                sys::chd_signal_state::CHD_SIG_NONSTANDARD_TBC_UNLOCKED
            }
            SignalState::NonstandardRaw => sys::chd_signal_state::CHD_SIG_NONSTANDARD_RAW,
        }
    }

    pub(crate) fn from_raw(raw: sys::chd_signal_state) -> SignalState {
        match raw {
            sys::chd_signal_state::CHD_SIG_STANDARD_TBC_LOCKED => SignalState::StandardTbcLocked,
            sys::chd_signal_state::CHD_SIG_STANDARD_TBC_UNLOCKED => {
                SignalState::StandardTbcUnlocked
            }
            sys::chd_signal_state::CHD_SIG_STANDARD_RAW => SignalState::StandardRaw,
            sys::chd_signal_state::CHD_SIG_NONSTANDARD_TBC_LOCKED => {
                SignalState::NonstandardTbcLocked
            }
            sys::chd_signal_state::CHD_SIG_NONSTANDARD_TBC_UNLOCKED => {
                SignalState::NonstandardTbcUnlocked
            }
            sys::chd_signal_state::CHD_SIG_NONSTANDARD_RAW => SignalState::NonstandardRaw,
            _ => SignalState::Unknown,
        }
    }
}

/// Plane selector (`chd_plane_t`).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Plane {
    Y,
    Cb,
    Cr,
    R,
    G,
    B,
}

impl Plane {
    pub(crate) fn raw(self) -> sys::chd_plane {
        match self {
            Plane::Y => sys::chd_plane::CHD_PLANE_Y,
            Plane::Cb => sys::chd_plane::CHD_PLANE_CB,
            Plane::Cr => sys::chd_plane::CHD_PLANE_CR,
            Plane::R => sys::chd_plane::CHD_PLANE_R,
            Plane::G => sys::chd_plane::CHD_PLANE_G,
            Plane::B => sys::chd_plane::CHD_PLANE_B,
        }
    }
}

/// Output pixel format (`chd_pixel_format_t`).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[non_exhaustive]
pub enum PixelFormat {
    Yuv444p16,
    Yuv444ps,
    Rgb48,
    Rgbs,
    Gray16,
    Grays,
}

impl PixelFormat {
    pub(crate) fn from_raw(raw: sys::chd_pixel_format) -> Result<PixelFormat> {
        Ok(match raw {
            sys::chd_pixel_format::CHD_PIXEL_YUV444P16 => PixelFormat::Yuv444p16,
            sys::chd_pixel_format::CHD_PIXEL_YUV444PS => PixelFormat::Yuv444ps,
            sys::chd_pixel_format::CHD_PIXEL_RGB48 => PixelFormat::Rgb48,
            sys::chd_pixel_format::CHD_PIXEL_RGBS => PixelFormat::Rgbs,
            sys::chd_pixel_format::CHD_PIXEL_GRAY16 => PixelFormat::Gray16,
            sys::chd_pixel_format::CHD_PIXEL_GRAYS => PixelFormat::Grays,
            other => {
                return Err(Error::internal(&format!(
                    "unrecognized chd_pixel_format value {}",
                    other.0
                )));
            }
        })
    }

    /// True for the float formats (`f32` planes); false for the 16-bit
    /// integer formats.
    pub fn is_float(self) -> bool {
        matches!(
            self,
            PixelFormat::Yuv444ps | PixelFormat::Rgbs | PixelFormat::Grays
        )
    }
}

/// Source parameter overrides for the CVBS open functions
/// (`chd_video_params_t`). Zero/`Unknown` fields defer to metadata.
#[derive(Clone, Copy, Debug, Default)]
pub struct VideoParams {
    pub standard: VideoStandard,
    pub encoding: SampleEncoding,
    pub signal_state: SignalState,
    pub field_width: i32,
    pub field_height: i32,
    pub sample_rate_hz: f64,
    pub active_video_start: i32,
    pub active_video_end: i32,
    pub first_active_frame_line: i32,
    pub last_active_frame_line: i32,
    pub black_16b_ire: i32,
    pub white_16b_ire: i32,
    pub blanking_16b_ire: i32,
    pub is_widescreen: bool,
    pub is_subcarrier_locked: bool,
    pub is_first_field_first: bool,
}

impl VideoParams {
    pub(crate) fn raw(&self) -> sys::chd_video_params {
        sys::chd_video_params {
            standard: self.standard.raw(),
            encoding: self.encoding.raw(),
            signal_state: self.signal_state.raw(),
            field_width: self.field_width,
            field_height: self.field_height,
            sample_rate_hz: self.sample_rate_hz,
            active_video_start: self.active_video_start,
            active_video_end: self.active_video_end,
            first_active_frame_line: self.first_active_frame_line,
            last_active_frame_line: self.last_active_frame_line,
            black_16b_ire: self.black_16b_ire,
            white_16b_ire: self.white_16b_ire,
            blanking_16b_ire: self.blanking_16b_ire,
            is_widescreen: self.is_widescreen as _,
            is_subcarrier_locked: self.is_subcarrier_locked as _,
            is_first_field_first: self.is_first_field_first as _,
        }
    }
}

/// Source description (`chd_video_info_t`).
#[derive(Clone, Copy, Debug)]
#[non_exhaustive]
pub struct VideoInfo {
    pub standard: VideoStandard,
    pub encoding: SampleEncoding,
    pub signal_state: SignalState,
    pub field_width: i32,
    pub field_height: i32,
    pub sample_rate_hz: f64,
    pub fsc_hz: f64,
    pub active_video_start: i32,
    pub active_video_end: i32,
    pub first_active_frame_line: i32,
    pub last_active_frame_line: i32,
    pub black_16b_ire: i32,
    pub white_16b_ire: i32,
    pub blanking_16b_ire: i32,
    pub num_frames: i64,
    pub is_widescreen: bool,
    pub is_subcarrier_locked: bool,
    pub is_first_field_first: bool,
}

impl VideoInfo {
    pub(crate) fn from_raw(raw: &sys::chd_video_info) -> VideoInfo {
        VideoInfo {
            standard: VideoStandard::from_raw(raw.standard),
            encoding: SampleEncoding::from_raw(raw.encoding),
            signal_state: SignalState::from_raw(raw.signal_state),
            field_width: raw.field_width,
            field_height: raw.field_height,
            sample_rate_hz: raw.sample_rate_hz,
            fsc_hz: raw.fsc_hz,
            active_video_start: raw.active_video_start,
            active_video_end: raw.active_video_end,
            first_active_frame_line: raw.first_active_frame_line,
            last_active_frame_line: raw.last_active_frame_line,
            black_16b_ire: raw.black_16b_ire,
            white_16b_ire: raw.white_16b_ire,
            blanking_16b_ire: raw.blanking_16b_ire,
            num_frames: raw.num_frames,
            is_widescreen: raw.is_widescreen != 0,
            is_subcarrier_locked: raw.is_subcarrier_locked != 0,
            is_first_field_first: raw.is_first_field_first != 0,
        }
    }
}

/// Committed output framing (`chd_output_info_t`).
#[derive(Clone, Copy, Debug)]
#[non_exhaustive]
pub struct OutputInfo {
    pub format: PixelFormat,
    pub width: i32,
    pub height: i32,
    pub num_planes: i32,
    pub num_frames: i64,
}

/// Decoded frame description (`chd_frame_info_t`).
#[derive(Clone, Copy, Debug)]
#[non_exhaustive]
pub struct FrameInfo {
    pub format: PixelFormat,
    pub width: i32,
    pub height: i32,
    pub num_planes: i32,
    pub frame_index: i64,
}
