// SPDX-License-Identifier: GPL-3.0-or-later

use std::ffi::CStr;
use std::fmt;

use chromadec_sys as sys;

/// Status codes from the C API (`chd_status_t`), minus `CHD_OK`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[non_exhaustive]
pub enum Status {
    InvalidArg,
    FileNotFound,
    Io,
    FormatUnsupported,
    MetadataMissing,
    MetadataCorrupt,
    PresetUnknown,
    DecoderUnknown,
    DecoderIncompatible,
    NnModelLoad,
    NnBackendUnavailable,
    NnInference,
    OutOfRange,
    Cancelled,
    Internal,
    Oom,
    Unsupported,
    /// A status value this crate doesn't know about (newer library).
    Other(u32),
}

impl Status {
    pub(crate) fn from_raw(raw: sys::chd_status) -> Status {
        match raw {
            sys::chd_status::CHD_E_INVALID_ARG => Status::InvalidArg,
            sys::chd_status::CHD_E_FILE_NOT_FOUND => Status::FileNotFound,
            sys::chd_status::CHD_E_IO => Status::Io,
            sys::chd_status::CHD_E_FORMAT_UNSUPPORTED => Status::FormatUnsupported,
            sys::chd_status::CHD_E_METADATA_MISSING => Status::MetadataMissing,
            sys::chd_status::CHD_E_METADATA_CORRUPT => Status::MetadataCorrupt,
            sys::chd_status::CHD_E_PRESET_UNKNOWN => Status::PresetUnknown,
            sys::chd_status::CHD_E_DECODER_UNKNOWN => Status::DecoderUnknown,
            sys::chd_status::CHD_E_DECODER_INCOMPATIBLE => Status::DecoderIncompatible,
            sys::chd_status::CHD_E_NN_MODEL_LOAD => Status::NnModelLoad,
            sys::chd_status::CHD_E_NN_BACKEND_UNAVAILABLE => Status::NnBackendUnavailable,
            sys::chd_status::CHD_E_NN_INFERENCE => Status::NnInference,
            sys::chd_status::CHD_E_OUT_OF_RANGE => Status::OutOfRange,
            sys::chd_status::CHD_E_CANCELLED => Status::Cancelled,
            sys::chd_status::CHD_E_INTERNAL => Status::Internal,
            sys::chd_status::CHD_E_OOM => Status::Oom,
            sys::chd_status::CHD_E_UNSUPPORTED => Status::Unsupported,
            other => Status::Other(other.0),
        }
    }

    fn raw(self) -> sys::chd_status {
        match self {
            Status::InvalidArg => sys::chd_status::CHD_E_INVALID_ARG,
            Status::FileNotFound => sys::chd_status::CHD_E_FILE_NOT_FOUND,
            Status::Io => sys::chd_status::CHD_E_IO,
            Status::FormatUnsupported => sys::chd_status::CHD_E_FORMAT_UNSUPPORTED,
            Status::MetadataMissing => sys::chd_status::CHD_E_METADATA_MISSING,
            Status::MetadataCorrupt => sys::chd_status::CHD_E_METADATA_CORRUPT,
            Status::PresetUnknown => sys::chd_status::CHD_E_PRESET_UNKNOWN,
            Status::DecoderUnknown => sys::chd_status::CHD_E_DECODER_UNKNOWN,
            Status::DecoderIncompatible => sys::chd_status::CHD_E_DECODER_INCOMPATIBLE,
            Status::NnModelLoad => sys::chd_status::CHD_E_NN_MODEL_LOAD,
            Status::NnBackendUnavailable => sys::chd_status::CHD_E_NN_BACKEND_UNAVAILABLE,
            Status::NnInference => sys::chd_status::CHD_E_NN_INFERENCE,
            Status::OutOfRange => sys::chd_status::CHD_E_OUT_OF_RANGE,
            Status::Cancelled => sys::chd_status::CHD_E_CANCELLED,
            Status::Internal => sys::chd_status::CHD_E_INTERNAL,
            Status::Oom => sys::chd_status::CHD_E_OOM,
            Status::Unsupported => sys::chd_status::CHD_E_UNSUPPORTED,
            Status::Other(v) => sys::chd_status(v),
        }
    }
}

impl fmt::Display for Status {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let s = unsafe { sys::chd_status_str(self.raw()) };
        if s.is_null() {
            return write!(f, "chd_status({:?})", self);
        }
        f.write_str(&unsafe { CStr::from_ptr(s) }.to_string_lossy())
    }
}

/// An error from libchromadec: a [`Status`] code plus the thread-local
/// detail message the library recorded (`chd_last_error`), if any.
#[derive(Clone, Debug)]
pub struct Error {
    status: Status,
    message: Option<String>,
}

impl Error {
    /// Builds an error from a raw status, capturing `chd_last_error()` from
    /// the calling thread. Must be called on the thread the failing C call
    /// ran on (the message is thread-local).
    pub(crate) fn from_raw(raw: sys::chd_status) -> Error {
        let message = unsafe {
            let p = sys::chd_last_error();
            if p.is_null() {
                None
            } else {
                let m = CStr::from_ptr(p).to_string_lossy().into_owned();
                if m.is_empty() { None } else { Some(m) }
            }
        };
        Error {
            status: Status::from_raw(raw),
            message,
        }
    }

    pub fn status(&self) -> Status {
        self.status
    }

    pub fn message(&self) -> Option<&str> {
        self.message.as_deref()
    }

    pub(crate) fn new(status: Status, message: Option<String>) -> Error {
        Error { status, message }
    }

    pub(crate) fn internal(message: &str) -> Error {
        Error {
            status: Status::Internal,
            message: Some(message.to_owned()),
        }
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match &self.message {
            Some(m) => write!(f, "{}: {}", self.status, m),
            None => write!(f, "{}", self.status),
        }
    }
}

impl std::error::Error for Error {}

pub type Result<T> = std::result::Result<T, Error>;

/// Maps a raw status to `Result<()>`, capturing the thread-local message on
/// failure.
pub(crate) fn check(raw: sys::chd_status) -> Result<()> {
    if raw == sys::chd_status::CHD_OK {
        Ok(())
    } else {
        Err(Error::from_raw(raw))
    }
}
