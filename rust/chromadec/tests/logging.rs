// SPDX-License-Identifier: GPL-3.0-or-later
//
// The diagnostic sink is process-global state, and cargo runs tests in threads
// of one process, so this all lives in a single test rather than racing itself
// across several.

use std::sync::{Arc, Mutex};

use chromadec::Video;
use chromadec::log::{self, Level, LevelFilter};

#[derive(Default)]
struct Captured(Mutex<Vec<(Level, bool, String)>>);

impl Captured {
    fn record(&self, d: &log::Diagnostic<'_>) {
        self.0
            .lock()
            .unwrap()
            .push((d.level, d.returned, d.message.to_owned()));
    }

    fn take(&self) -> Vec<(Level, bool, String)> {
        std::mem::take(&mut *self.0.lock().unwrap())
    }
}

/// A minimal but openable ld-decode source: two fields of silence plus the
/// JSON sidecar describing them. Enough to make the reader talk.
fn write_source(dir: &std::path::Path) -> std::path::PathBuf {
    const FIELD_WIDTH: usize = 910;
    const FIELD_HEIGHT: usize = 263;
    let tbc = dir.join("logging.tbc");
    std::fs::write(&tbc, vec![0u8; FIELD_WIDTH * FIELD_HEIGHT * 2 * 2]).unwrap();
    std::fs::write(
        dir.join("logging.tbc.json"),
        r#"{"videoParameters":{"numberOfSequentialFields":2,"system":"NTSC",
            "fieldWidth":910,"fieldHeight":263,"isSourcePal":false,
            "black16bIre":16384,"white16bIre":54016,"fsc":0,
            "colourBurstStart":98,"colourBurstEnd":110,
            "activeVideoStart":134,"activeVideoEnd":894,
            "isSubcarrierLocked":true},
            "fields":[{"seqNo":1,"isFirstField":true,"syncConf":100,"medianBurstIRE":20,"fieldPhaseID":1},
                      {"seqNo":2,"isFirstField":false,"syncConf":100,"medianBurstIRE":20,"fieldPhaseID":2}]}"#,
    )
    .unwrap();
    tbc
}

#[test]
fn diagnostic_sink() {
    let dir = std::env::temp_dir().join("chromadec-logging-test");
    std::fs::create_dir_all(&dir).unwrap();
    let source = write_source(&dir);

    // Silent by default: nothing is installed until a consumer asks.
    assert_eq!(log::filter(), LevelFilter::Info);
    assert!(!log::is_enabled(Level::Error));
    assert!(Video::open_composite(&source, None, None).is_ok());

    let captured = Arc::new(Captured::default());
    let sink = Arc::clone(&captured);
    log::set_callback(move |d| sink.record(d));
    assert!(log::is_enabled(Level::Error));
    assert!(!log::is_enabled(Level::Debug), "debug is opt-in");

    // Real library diagnostics reach the closure.
    log::set_filter(LevelFilter::Debug);
    assert!(Video::open_composite(&source, None, None).is_ok());
    let records = captured.take();
    assert!(
        records.iter().any(|(level, _, _)| *level == Level::Debug),
        "expected debug diagnostics from the reader, got {records:?}"
    );
    assert!(
        records.iter().all(|(_, returned, _)| !returned),
        "a successful open has nothing on the return path to duplicate"
    );

    // A failing open still reports on the return path, which is the channel
    // that carries failures whether or not anyone is listening here.
    let err = Video::open_composite("/nonexistent/chromadec-log-test.tbc", None, None).unwrap_err();
    assert!(err.message().is_some());

    // A failure the library detects rather than guards against arrives on both
    // channels, and the copy on this one is marked so a consumer reporting the
    // Err can drop it. The sidecar here claims more fields than it lists.
    captured.take();
    let corrupt = dir.join("corrupt.tbc");
    std::fs::copy(&source, &corrupt).unwrap();
    std::fs::write(
        corrupt.with_extension("tbc.json"),
        std::fs::read_to_string(source.with_extension("tbc.json"))
            .unwrap()
            .replace(
                r#""numberOfSequentialFields":2"#,
                r#""numberOfSequentialFields":9"#,
            ),
    )
    .unwrap();
    let err = Video::open_composite(&corrupt, None, None).unwrap_err();
    let detail = err
        .message()
        .expect("corrupt sidecar names the inconsistency");
    let marked: Vec<_> = captured
        .take()
        .into_iter()
        .filter(|(_, returned, _)| *returned)
        .collect();
    assert_eq!(marked.len(), 1, "expected exactly one returned failure");
    let (level, _, message) = &marked[0];
    assert_eq!(*level, Level::Error);
    assert!(
        detail.contains(message.as_str()),
        "marked message {message:?} should be the detail carried by {detail:?}"
    );

    // The threshold gates delivery.
    captured.take();
    log::set_filter(LevelFilter::Off);
    assert!(!log::is_enabled(Level::Error));
    assert!(Video::open_composite(&source, None, None).is_ok());
    assert!(captured.take().is_empty());

    // Uninstalling returns the library to silence and drops the closure, so
    // the only strong reference left is ours.
    log::set_filter(LevelFilter::Debug);
    log::clear_callback();
    assert!(!log::is_enabled(Level::Error));
    assert!(Video::open_composite(&source, None, None).is_ok());
    assert!(captured.take().is_empty());
    assert_eq!(Arc::strong_count(&captured), 1);

    log::set_filter(LevelFilter::Info);
    std::fs::remove_dir_all(&dir).ok();
}
