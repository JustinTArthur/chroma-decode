// SPDX-License-Identifier: GPL-3.0-or-later

//! Diagnostic sink.
//!
//! libchromadec never writes to the console on its own. Failures come back as
//! [`Error`](crate::Error) on the return path; this module is the separate
//! channel for the running commentary a decode produces, which has no return
//! path to travel. Nothing is emitted until you install a sink, so a library
//! consumer that wants silence gets it by doing nothing.
//!
//! ```no_run
//! use chromadec::log::{self, LevelFilter};
//!
//! log::set_filter(LevelFilter::Debug);
//! log::set_callback(|d| eprintln!("[{}] {}", d.level, d.message));
//! ```
//!
//! A failure on its way back as an [`Error`](crate::Error) is also announced
//! here, at [`Level::Error`], carrying the same text. [`Diagnostic::returned`]
//! marks those, so a sink that already reports errors from the return path can
//! skip them instead of saying it twice:
//!
//! ```no_run
//! # use chromadec::log;
//! log::set_callback(|d| {
//!     if d.returned {
//!         return; // the `Err` carries this; reporting it here would duplicate
//!     }
//!     eprintln!("[{}] {}", d.level, d.message);
//! });
//! ```

use std::ffi::{CStr, c_char, c_void};
use std::fmt;
use std::panic::{self, AssertUnwindSafe};
use std::sync::{Arc, RwLock};

use chromadec_sys as sys;

type Sink = Arc<dyn Fn(&Diagnostic<'_>) + Send + Sync + 'static>;

/// One diagnostic handed to the sink.
///
/// Non-exhaustive: the library may learn to say more about a message than it
/// does today, so match on the fields you need rather than destructuring the
/// whole struct.
#[derive(Debug)]
#[non_exhaustive]
pub struct Diagnostic<'a> {
    /// Severity the message was emitted at.
    pub level: Level,
    /// The text, borrowed for the duration of the callback.
    pub message: &'a str,
    /// Whether this same text is also on its way to the caller as the detail
    /// of an [`Error`](crate::Error). Only ever true at [`Level::Error`].
    ///
    /// Diagnostics the library handles internally are never marked, so
    /// ignoring marked messages costs you nothing you would not learn from the
    /// `Err` you are about to receive.
    pub returned: bool,
}

static SINK: RwLock<Option<Sink>> = RwLock::new(None);

/// Severity of a diagnostic message, mirroring `log::Level`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum Level {
    Debug,
    Info,
    Warn,
    Error,
}

impl Level {
    fn from_raw(raw: sys::chd_log_level) -> Option<Level> {
        match raw {
            sys::chd_log_level::CHD_LOG_DEBUG => Some(Level::Debug),
            sys::chd_log_level::CHD_LOG_INFO => Some(Level::Info),
            sys::chd_log_level::CHD_LOG_WARN => Some(Level::Warn),
            sys::chd_log_level::CHD_LOG_ERROR => Some(Level::Error),
            _ => None,
        }
    }

    fn raw(self) -> sys::chd_log_level {
        match self {
            Level::Debug => sys::chd_log_level::CHD_LOG_DEBUG,
            Level::Info => sys::chd_log_level::CHD_LOG_INFO,
            Level::Warn => sys::chd_log_level::CHD_LOG_WARN,
            Level::Error => sys::chd_log_level::CHD_LOG_ERROR,
        }
    }

    /// Stable uppercase name, e.g. `"WARN"`.
    pub fn as_str(self) -> &'static str {
        match self {
            Level::Debug => "DEBUG",
            Level::Info => "INFO",
            Level::Warn => "WARN",
            Level::Error => "ERROR",
        }
    }
}

impl fmt::Display for Level {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.pad(self.as_str())
    }
}

/// Threshold below which diagnostics are dropped, mirroring
/// `log::LevelFilter`. [`Off`](LevelFilter::Off) drops everything.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum LevelFilter {
    Debug,
    Info,
    Warn,
    Error,
    Off,
}

impl LevelFilter {
    fn from_raw(raw: sys::chd_log_level) -> LevelFilter {
        match raw {
            sys::chd_log_level::CHD_LOG_DEBUG => LevelFilter::Debug,
            sys::chd_log_level::CHD_LOG_INFO => LevelFilter::Info,
            sys::chd_log_level::CHD_LOG_WARN => LevelFilter::Warn,
            sys::chd_log_level::CHD_LOG_ERROR => LevelFilter::Error,
            _ => LevelFilter::Off,
        }
    }

    fn raw(self) -> sys::chd_log_level {
        match self {
            LevelFilter::Debug => sys::chd_log_level::CHD_LOG_DEBUG,
            LevelFilter::Info => sys::chd_log_level::CHD_LOG_INFO,
            LevelFilter::Warn => sys::chd_log_level::CHD_LOG_WARN,
            LevelFilter::Error => sys::chd_log_level::CHD_LOG_ERROR,
            LevelFilter::Off => sys::chd_log_level::CHD_LOG_OFF,
        }
    }
}

impl From<Level> for LevelFilter {
    fn from(level: Level) -> LevelFilter {
        match level {
            Level::Debug => LevelFilter::Debug,
            Level::Info => LevelFilter::Info,
            Level::Warn => LevelFilter::Warn,
            Level::Error => LevelFilter::Error,
        }
    }
}

unsafe extern "C" fn trampoline(
    level: sys::chd_log_level,
    flags: sys::chd_log_flags_t,
    message: *const c_char,
    _user_data: *mut c_void,
) {
    let Some(level) = Level::from_raw(level) else {
        return;
    };
    if message.is_null() {
        return;
    }
    // Clone the sink out and release the lock before calling it, so a callback
    // that re-enters this module cannot deadlock against its own read guard.
    let sink = SINK
        .read()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
        .clone();
    let Some(sink) = sink else { return };
    let message = unsafe { CStr::from_ptr(message) }.to_string_lossy();
    let diagnostic = Diagnostic {
        level,
        message: &message,
        returned: flags & sys::CHD_LOG_F_RETURNED != 0,
    };
    // Unwinding into C is undefined; a panicking sink is swallowed rather than
    // taking a decode worker with it.
    let _ = panic::catch_unwind(AssertUnwindSafe(|| sink(&diagnostic)));
}

/// Route diagnostics to `f`, replacing any previous sink.
///
/// The callback may be invoked concurrently from the library's decode worker
/// threads, hence the `Send + Sync` bound. It must not call back into
/// libchromadec.
///
/// Check [`Diagnostic::returned`] if your program also reports the `Err` from
/// a failing call, so the same reason does not reach the user twice.
pub fn set_callback<F>(f: F)
where
    F: Fn(&Diagnostic<'_>) + Send + Sync + 'static,
{
    *SINK
        .write()
        .unwrap_or_else(|poisoned| poisoned.into_inner()) = Some(Arc::new(f));
    unsafe { sys::chd_set_log_callback(Some(trampoline), std::ptr::null_mut()) };
}

/// Uninstall the sink and return to the default silence. Once this returns the
/// previous callback is neither running nor reachable, so it is dropped here.
pub fn clear_callback() {
    // Detach on the C side first: that call does not return until any in-flight
    // dispatch has finished, which is what makes dropping the closure safe.
    unsafe { sys::chd_set_log_callback(None, std::ptr::null_mut()) };
    *SINK
        .write()
        .unwrap_or_else(|poisoned| poisoned.into_inner()) = None;
}

/// Install the library's built-in stderr sink, one line per message. A
/// convenience for command-line consumers.
pub fn to_stderr() {
    clear_callback();
    unsafe { sys::chd_log_to_stderr() };
}

/// Drop diagnostics below `filter`. Defaults to [`LevelFilter::Info`], so
/// debug output is opt-in.
pub fn set_filter(filter: LevelFilter) {
    unsafe { sys::chd_set_log_level(filter.raw()) };
}

/// The current threshold.
pub fn filter() -> LevelFilter {
    LevelFilter::from_raw(unsafe { sys::chd_get_log_level() })
}

/// Whether a message at this level would reach a sink.
pub fn is_enabled(level: Level) -> bool {
    unsafe { sys::chd_log_is_enabled(level.raw()) != 0 }
}

/// Forward diagnostics to the [`log`] crate facade, under the target
/// `"chromadec"`, and align the library's threshold with `log::max_level()`.
///
/// Call it after installing your logger, so the max level is already set.
///
/// Everything is forwarded, including failures also returned as an
/// [`Error`](crate::Error). A log facade is a record of what happened rather
/// than the program's error reporting, so a failure belongs in it either way;
/// write your own sink over [`Diagnostic::returned`] if you would rather it
/// were not.
#[cfg(feature = "log")]
pub fn forward_to_log_crate() {
    set_filter(match log::max_level() {
        log::LevelFilter::Off => LevelFilter::Off,
        log::LevelFilter::Error => LevelFilter::Error,
        log::LevelFilter::Warn => LevelFilter::Warn,
        log::LevelFilter::Info => LevelFilter::Info,
        log::LevelFilter::Debug | log::LevelFilter::Trace => LevelFilter::Debug,
    });
    set_callback(|d| {
        let level = match d.level {
            Level::Debug => log::Level::Debug,
            Level::Info => log::Level::Info,
            Level::Warn => log::Level::Warn,
            Level::Error => log::Level::Error,
        };
        let message = d.message;
        log::log!(target: "chromadec", level, "{message}");
    });
}
