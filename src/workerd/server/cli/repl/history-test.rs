// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

/// A path for a history file in a fresh directory of the test's temp directory.
fn fresh_path() -> PathBuf {
    use std::sync::atomic::AtomicUsize;
    use std::sync::atomic::Ordering;

    static COUNTER: AtomicUsize = AtomicUsize::new(0);
    let tmp = std::env::var_os("TEST_TMPDIR").map_or_else(std::env::temp_dir, PathBuf::from);
    let dir = tmp.join(format!(
        "workerd-repl-history-test-{}-{}",
        std::process::id(),
        COUNTER.fetch_add(1, Ordering::Relaxed)
    ));
    fs::create_dir_all(&dir).unwrap();
    dir.join("history")
}

#[test]
fn the_environment_chooses_the_file() {
    let var = |value: &str| Some(OsString::from(value));
    assert_eq!(
        choose(var("/h/custom"), var("/home/ada")),
        Some(PathBuf::from("/h/custom"))
    );
    assert_eq!(
        choose(None, var("/home/ada")),
        Some(PathBuf::from("/home/ada/.workerd_repl_history"))
    );
    // Empty, the variable turns saving off.
    assert_eq!(choose(var(""), var("/home/ada")), None);
    assert_eq!(choose(None, None), None);
    assert_eq!(choose(None, var("")), None);
}

#[test]
fn recorded_entries_load_in_the_next_session() {
    let path = fresh_path();
    let mut file = HistoryFile::new(path.clone());
    file.record("1 + 1").unwrap();
    file.record("await f()").unwrap();

    let mut next = HistoryFile::new(path.clone());
    assert_eq!(next.load(10).unwrap(), ["1 + 1", "await f()"]);
    next.record("g()").unwrap();
    assert_eq!(
        fs::read_to_string(&path).unwrap(),
        "1 + 1\nawait f()\ng()\n"
    );
}

#[test]
fn a_missing_file_holds_nothing() {
    let path = fresh_path();
    assert!(HistoryFile::new(path.clone()).load(10).unwrap().is_empty());
    assert!(!path.exists());
}

#[test]
fn loading_keeps_the_latest_entries() {
    let path = fresh_path();
    fs::write(&path, "a\nb\nc\nd\ne\n").unwrap();
    assert_eq!(
        HistoryFile::new(path.clone()).load(3).unwrap(),
        ["c", "d", "e"]
    );
    assert_eq!(fs::read_to_string(&path).unwrap(), "c\nd\ne\n");

    // A file within the limit is left as it is.
    fs::write(&path, "x\n\ny\r\n").unwrap();
    assert_eq!(HistoryFile::new(path.clone()).load(3).unwrap(), ["x", "y"]);
    assert_eq!(fs::read_to_string(&path).unwrap(), "x\n\ny\r\n");
}

#[cfg(unix)]
#[test]
fn only_the_owner_can_read_the_file() {
    use std::os::unix::fs::PermissionsExt;

    let path = fresh_path();
    HistoryFile::new(path.clone()).record("secret").unwrap();
    let mode = fs::metadata(&path).unwrap().permissions().mode();
    assert_eq!(mode & 0o777, 0o600);
}
