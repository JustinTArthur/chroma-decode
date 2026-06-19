// SPDX-License-Identifier: GPL-3.0-or-later

use chromadec::{Cancel, Video};

#[test]
fn version_reports() {
    let (major, minor, patch) = chromadec::version();
    assert!(major >= 0 && minor >= 0 && patch >= 0);
    assert!(!chromadec::version_string().is_empty());
}

#[test]
fn feature_queries() {
    // SQLite is a required dependency of the library.
    assert!(chromadec::has_feature("sqlite"));
    assert!(!chromadec::has_feature("definitely-not-a-feature"));
}

#[test]
fn open_missing_tbc_reports_error_with_message() {
    let err = Video::open_composite("/nonexistent/missing.tbc", None, None).unwrap_err();
    assert!(
        err.message().is_some(),
        "expected a chd_last_error detail message"
    );
    let display = err.to_string();
    assert!(display.contains(err.message().unwrap()));
}

#[test]
fn cancel_roundtrip() {
    let cancel = Cancel::new().unwrap();
    assert!(!cancel.is_requested());
    cancel.request();
    assert!(cancel.is_requested());
}

#[cfg(feature = "tokio")]
#[test]
fn frame_stream_is_send_and_a_stream() {
    fn assert_send<T: Send>() {}
    fn assert_stream<T: futures_core::Stream<Item = chromadec::DecodedFrame>>() {}
    // The adapter must be movable across tasks and usable with StreamExt.
    assert_send::<chromadec::FrameStream>();
    assert_stream::<chromadec::FrameStream>();
}
