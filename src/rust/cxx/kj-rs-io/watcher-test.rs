use std::task::Context;
use std::task::Poll;
use std::task::Waker;

use static_assertions::assert_impl_all;

use super::*;

// Send + Sync like every kj-rs-io handle (lib.rs asserts this at compile time).
assert_impl_all!(TokioFileWatcher: Send, Sync);

fn scratch_dir(tag: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("kj-rs-io-watcher-{tag}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

fn watch_path(watcher: &TokioFileWatcher, path: &Path) {
    #[cfg(unix)]
    {
        use std::os::unix::ffi::OsStrExt;
        watcher.watch(path.as_os_str().as_bytes()).unwrap();
    }
    #[cfg(windows)]
    watcher.watch(path.to_string_lossy().as_bytes()).unwrap();
}

/// `rescan` for tests that only ask whether something changed.
fn changed(files: &mut Files) -> bool {
    files.rescan(&mut Vec::new())
}

/// A `Files` table on its own, with no backend running: the unit tests below drive `rescan`
/// by hand, so a live backend's own wake-ups cannot interleave with them (the C++ tests
/// cover the real backends end to end).
fn files_for(paths: &[&Path]) -> Files {
    let mut files = Files::default();
    for path in paths {
        files.by_path.insert(
            path.to_path_buf(),
            FileState {
                canonical: canonical(path),
                stamp: stamp(path),
            },
        );
    }
    files
}

#[test]
fn watch_rejects_a_path_without_a_file_name() {
    let _port = kj_rs_tokio::TokioPort::new();
    let watcher = TokioFileWatcher::new().unwrap();
    let err = cxx::KjError::from(watcher.watch(b"/").unwrap_err());
    assert!(
        err.description().contains("not a file path"),
        "{}",
        err.description()
    );
}

#[test]
fn watch_requires_the_parent_directory_to_exist() {
    let _port = kj_rs_tokio::TokioPort::new();
    let watcher = TokioFileWatcher::new().unwrap();
    assert!(
        watcher
            .watch(b"/nonexistent-kj-rs-io-dir/file.txt")
            .is_err(),
        "a missing directory cannot be watched (the file itself may be missing)"
    );
}

/// The rule: a stamp change fires whatever the backend said; no stamp change fires nothing.
#[test]
fn rescan_fires_exactly_on_stamp_changes() {
    let dir = scratch_dir("rescan");
    let file = dir.join("watched.txt");
    let other = dir.join("other.txt");
    std::fs::write(&file, b"one").unwrap();
    let mut files = files_for(&[&file]);

    assert!(!changed(&mut files), "nothing changed since watch()");
    std::fs::write(&other, b"unrelated").unwrap();
    assert!(
        !changed(&mut files),
        "an unrelated file in the directory is not a change"
    );
    std::fs::write(&file, b"one more").unwrap();
    assert!(changed(&mut files), "a content change is");
    assert!(!changed(&mut files), "...once");
    std::fs::remove_file(&file).unwrap();
    assert!(changed(&mut files), "a deletion is");
    std::fs::write(&file, b"back").unwrap();
    assert!(changed(&mut files), "a recreation is");
    let _ = std::fs::remove_dir_all(&dir);
}

/// A watched symlink is stamped through to its target, and the target's directory is
/// registered so edits there wake the watcher.
#[cfg(unix)]
#[test]
fn a_symlinked_file_is_followed_and_its_target_directory_is_watched() {
    let _port = kj_rs_tokio::TokioPort::new();
    let link_dir = scratch_dir("link");
    let target_dir = scratch_dir("target");
    let target = target_dir.join("real.txt");
    std::fs::write(&target, b"one").unwrap();
    let link = link_dir.join("link.txt");
    std::os::unix::fs::symlink(&target, &link).unwrap();
    let watcher = TokioFileWatcher::new().unwrap();
    watch_path(&watcher, &link);
    assert!(
        watcher
            .shared
            .backend
            .lock()
            .unwrap()
            .dirs
            .contains(&std::fs::canonicalize(&target_dir).unwrap()),
        "the target's directory is watched"
    );
    let mut files = files_for(&[&link]);
    assert!(!changed(&mut files));
    std::fs::write(&target, b"one more").unwrap();
    assert!(changed(&mut files), "an edit to the target is a change");
    let _ = std::fs::remove_dir_all(&link_dir);
    let _ = std::fs::remove_dir_all(&target_dir);
}

/// The hand-off's contract: a notification that lands after a re-stamp and before the wait
/// is not lost (the stored permit), a wake with nothing changed resolves nothing, and a second
/// concurrent waiter is rejected rather than racing for the one permit.
#[test]
fn a_wake_before_the_wait_is_not_lost_and_a_second_waiter_is_rejected() {
    let _port = kj_rs_tokio::TokioPort::new();
    let dir = scratch_dir("wake");
    let file = dir.join("watched.txt");
    std::fs::write(&file, b"one").unwrap();
    let watcher = TokioFileWatcher::new().unwrap();
    watch_path(&watcher, &file);
    let mut cx = Context::from_waker(Waker::noop());

    // The producer fires between the consumer's re-stamp and its wait: the permit makes the
    // wait return at once; with no change the consumer simply waits again.
    watcher.shared.wake.notify_one();
    let mut first = Box::pin(watcher.on_change());
    assert!(first.as_mut().poll(&mut cx).is_pending());
    // A change plus a wake resolves it. (A different length: "one" -> "two" within one
    // kernel timestamp tick is the documented blind spot, and CI's Linux is that fast.)
    std::fs::write(&file, b"one more").unwrap();
    watcher.shared.wake.notify_one();
    assert!(matches!(first.as_mut().poll(&mut cx), Poll::Ready(Ok(()))));

    // A wake with nothing changed (an overflow notice, an unrelated file) is no change.
    assert!(!watcher.shared.rescan().unwrap());

    let mut second = Box::pin(watcher.on_change());
    assert!(second.as_mut().poll(&mut cx).is_pending());
    let mut third = Box::pin(watcher.on_change());
    let Poll::Ready(Err(e)) = third.as_mut().poll(&mut cx) else {
        panic!("a second concurrent onChange() must be rejected");
    };
    assert!(
        cxx::KjError::from(e)
            .description()
            .contains("at most one onChange()")
    );
    drop(second);
    let mut fourth = Box::pin(watcher.on_change());
    assert!(
        fourth.as_mut().poll(&mut cx).is_pending(),
        "the guard reset on drop"
    );
    let _ = std::fs::remove_dir_all(&dir);
}

#[cfg(unix)]
#[test]
fn rescan_follows_a_retargeted_symlink_and_reports_its_new_directory() {
    let dir = scratch_dir("retarget");
    let (old_dir, new_dir) = (dir.join("old"), dir.join("new"));
    std::fs::create_dir_all(&old_dir).unwrap();
    std::fs::create_dir_all(&new_dir).unwrap();
    std::fs::write(old_dir.join("real.txt"), "one").unwrap();
    std::fs::write(new_dir.join("real.txt"), "uno").unwrap();
    let link = dir.join("link.txt");
    std::os::unix::fs::symlink(old_dir.join("real.txt"), &link).unwrap();

    let mut files = files_for(&[&link]);
    let mut new_dirs = Vec::new();
    assert!(!files.rescan(&mut new_dirs));
    assert!(new_dirs.is_empty());

    std::fs::remove_file(&link).unwrap();
    std::os::unix::fs::symlink(new_dir.join("real.txt"), &link).unwrap();
    assert!(files.rescan(&mut new_dirs), "a retarget is a change");
    assert_eq!(new_dirs, vec![std::fs::canonicalize(&new_dir).unwrap()]);
    assert_eq!(
        files.by_path[&link].canonical,
        Some(std::fs::canonicalize(new_dir.join("real.txt")).unwrap())
    );
    new_dirs.clear();
    assert!(!files.rescan(&mut new_dirs), "...once");
    assert!(new_dirs.is_empty());
    let _ = std::fs::remove_dir_all(&dir);
}

#[cfg(unix)]
#[test]
fn rescan_follows_a_dangling_symlink_retargeted_to_another_missing_file() {
    let dir = scratch_dir("dangling-retarget");
    let (old_dir, new_dir) = (dir.join("old"), dir.join("new"));
    std::fs::create_dir_all(&old_dir).unwrap();
    std::fs::create_dir_all(&new_dir).unwrap();
    let link = dir.join("link.txt");
    std::os::unix::fs::symlink(old_dir.join("missing.txt"), &link).unwrap();

    let mut files = files_for(&[&link]);
    std::fs::remove_file(&link).unwrap();
    std::os::unix::fs::symlink(new_dir.join("missing.txt"), &link).unwrap();

    let mut new_dirs = Vec::new();
    files.rescan(&mut new_dirs);
    assert_eq!(new_dirs, vec![std::fs::canonicalize(&new_dir).unwrap()]);
    assert_eq!(
        files.by_path[&link].canonical,
        Some(std::fs::canonicalize(&new_dir).unwrap().join("missing.txt")),
        "resolved through the dangling link into its (canonical) target directory"
    );
    let _ = std::fs::remove_dir_all(&dir);
}
