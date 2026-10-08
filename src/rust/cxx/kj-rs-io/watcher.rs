//! The `--watch` file watcher, over the `notify` crate (inotify on Linux, `FSEvents` on macOS,
//! `ReadDirectoryChangesW` on Windows).
//!
//! workerd's `--watch` (`server/cli/watch.rs`) uses this from Rust; `kj_rs_io::FileWatcher`
//! (async-io.h) wraps the three bridged calls in ffi.rs for C++. The design in one sentence: **the backend says "look again"; the files say
//! what changed.**
//!
//! - Each watched file's *parent directory* is watched (non-recursively), and, when the path is a
//!   symlink, the directory of its resolved target as well -- edits to a target elsewhere would
//!   otherwise produce no event in the watched directory. No per-file watch and no per-entry
//!   descriptor exist, so the file need not exist yet and a file replaced by `rename(2)` (an
//!   editor's atomic save), deleted and recreated, or retargeted keeps firing. A retargeted
//!   symlink's new target directory is registered when the retarget is reported, so edits there
//!   keep firing too. Former target directories stay registered until the watcher is dropped:
//!   a retarget is a rare, human-driven event, and an event in a stale directory costs one
//!   re-stamp, so no pruning machinery is worth it.
//! - Every watched file has a metadata stamp (size, mtime, inode on unix). Whenever the backend
//!   reports anything at all -- an event on any entry of a watched directory, a kernel-queue
//!   overflow, an error -- `on_change` re-stamps every watched file and resolves if a stamp
//!   differs from the last reported one. That is the whole rule. What it deliberately does
//!   *not* do: judge events by kind or path, hash content, or track ctime. An earlier version
//!   did all three to catch a same-size rewrite whose mtime was restored (timestamp-preserving
//!   copies) and to tell a replayed pre-watch event from a real change; a stamp compare gets
//!   every case a developer produces under `--watch` -- save, atomic rename, delete, recreate,
//!   retarget -- and the residue is not worth the machinery. That residue, stated exactly: a
//!   rewrite of the same inode to the same length whose mtime equals the stamped one. Besides
//!   `touch -r`, file timestamps have the kernel's tick granularity (milliseconds on Linux
//!   before 6.13's multigrain timestamps), so two same-length writes landing within one tick,
//!   with a change reported between them, count as one. That is a lost second reload a few
//!   milliseconds after the first, from a tool rewriting a file twice in a burst -- tests here
//!   change the length when they mean a change. An attribute-only change (chmod) never fires:
//!   it moves no field of the stamp. A replayed or coalesced event for a
//!   write that preceded `watch()` (`FSEvents` does that) finds the stamp `watch()` took and fires
//!   nothing.
//! - Nothing depends on the backend's path spelling or event kinds: the callback does not look
//!   at the event at all.
//! - Bursts coalesce into one resolution (`on_change` re-stamps once per wake), and unrelated
//!   files in the same directory do not fire (their events mark nothing dirty and move no stamp).
//!   Re-stamping every watched file per wake assumes the watched set is small -- a config file and a handful of sources, as under `--watch`; it
//!   is not a design for watching a tree.
//! - notify delivers events on its own thread. The producer calls `notify_one()` on a
//!   `tokio::sync::Notify` and nothing else (no queue, so nothing can grow while workerd is
//!   busy); a `Notify` stores a permit when nobody is waiting, so a wake-up that lands between
//!   two of the consumer's re-stamps is never lost. Every `on_change` future owns a share of the watcher state (lib.rs,
//!   "Ownership of in-flight operations"), so a `FileWatcher` destroyed while a promise is
//!   pending is memory-safe and the promise still resolves.

use std::collections::HashMap;
use std::collections::HashSet;
use std::future::Future;
use std::path::Path;
use std::path::PathBuf;
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::MutexGuard;
use std::sync::atomic::AtomicBool;
use std::sync::atomic::Ordering;

use notify::Config;
use notify::Event;
use notify::RecommendedWatcher;
use notify::RecursiveMode;
use notify::Watcher;
use tokio::sync::Notify;

use crate::error::KjIoError;
use crate::error::Result;

/// See the module docs.
pub struct TokioFileWatcher {
    shared: Arc<Shared>,
}

struct Shared {
    backend: Mutex<Backend>,
    /// The watched files, keyed by the path given to `watch()`, with their last stamps.
    files: Mutex<Files>,
    /// "Look again", from notify's thread (see the module docs).
    wake: Arc<Notify>,
    /// Whether an `on_change` is waiting. `Notify::notify_one` wakes one waiter, so a second
    /// concurrent waiter could miss a change; workerd awaits `onChange()` sequentially, and a
    /// second caller gets an error rather than that race.
    waiting: AtomicBool,
}

/// The notify watcher and what has been registered with it, under one lock.
struct Backend {
    watcher: RecommendedWatcher,
    /// Directories registered, by canonical path.
    dirs: HashSet<PathBuf>,
}

/// Locks one of the watcher's mutexes; a poisoned lock (a panic on notify's thread) is an error.
fn lock<'a, T>(mutex: &'a Mutex<T>, op: &'static str) -> Result<MutexGuard<'a, T>> {
    mutex
        .lock()
        .map_err(|_| KjIoError::other(op, "watcher state poisoned"))
}

/// The watched files and what was last known about them.
#[derive(Default)]
struct Files {
    /// Watched file -> its state. The key is the path as given to `watch()`; stamps are taken
    /// through it, following symlinks.
    by_path: HashMap<PathBuf, FileState>,
}

struct FileState {
    /// Where the path resolved to when last stamped (the directory to watch for a symlink's
    /// target); `None` while the file does not exist.
    canonical: Option<PathBuf>,
    /// As of the last reported change (or `watch()`); `None` while the file does not exist.
    stamp: Option<Stamp>,
}

/// What we last knew about a watched file's metadata: enough to tell "changed / appeared /
/// disappeared / replaced by another inode" apart from "nothing happened".
#[derive(Clone, Copy, PartialEq, Eq)]
struct Stamp {
    len: u64,
    modified: Option<std::time::SystemTime>,
    #[cfg(unix)]
    ino: u64,
}

fn stamp(path: &Path) -> Option<Stamp> {
    #[cfg(unix)]
    use std::os::unix::fs::MetadataExt;
    let meta = std::fs::metadata(path).ok()?;
    Some(Stamp {
        len: meta.len(),
        modified: meta.modified().ok(),
        #[cfg(unix)]
        ino: meta.ino(),
    })
}

fn notify_error(op: &'static str) -> impl Fn(notify::Error) -> KjIoError {
    move |e| KjIoError::other(op, e)
}

/// A path as `kj::Path::toNativeString` produced it: arbitrary bytes on unix, UTF-8 on Windows.
#[cfg_attr(
    unix,
    expect(
        clippy::unnecessary_wraps,
        reason = "one signature for both platforms; only the Windows arm can fail"
    )
)]
fn native_path(bytes: &[u8]) -> Result<PathBuf> {
    #[cfg(unix)]
    {
        use std::os::unix::ffi::OsStrExt;
        Ok(PathBuf::from(std::ffi::OsStr::from_bytes(bytes)))
    }
    #[cfg(windows)]
    {
        std::str::from_utf8(bytes)
            .map(PathBuf::from)
            .map_err(|_| KjIoError::other("FileWatcher::watch", "path is not valid UTF-8"))
    }
}

/// The canonical form of a path whose final component may not exist: the canonical parent plus
/// the file name.
/// Where `path` resolves to: its canonical path if it exists, else the canonical directory it
/// would appear in plus its name -- following a dangling symlink to where *its target* would
/// appear, so a `--watch` on a link whose target is created later (a bundler's output) gets the
/// target's directory watched from the start. The depth cap mirrors the kernel's link-chain
/// limit; a chain the kernel would refuse resolves to `None` (nothing extra to watch).
fn canonical(path: &Path) -> Option<PathBuf> {
    const MAX_LINK_DEPTH: usize = 40;
    let mut path = path.to_path_buf();
    for _ in 0..MAX_LINK_DEPTH {
        if let Ok(resolved) = std::fs::canonicalize(&path) {
            return Some(resolved);
        }
        if let Ok(target) = std::fs::read_link(&path) {
            path = if target.is_absolute() {
                target
            } else {
                path.parent()?.join(target)
            };
        } else {
            let dir = std::fs::canonicalize(path.parent()?).ok()?;
            return Some(dir.join(path.file_name()?));
        }
    }
    None
}

impl Files {
    /// Re-stamps every watched file; `true` if any stamp differs from the last reported one.
    /// A changed file's canonical path is resolved again (a symlink may point elsewhere now);
    /// the directories newly resolved-to are pushed onto `new_dirs` for the caller to watch.
    fn rescan(&mut self, new_dirs: &mut Vec<PathBuf>) -> bool {
        let mut changed = false;
        for (path, state) in &mut self.by_path {
            let now = stamp(path);
            let file_changed = now != state.stamp;
            if file_changed {
                state.stamp = now;
                changed = true;
            }
            if file_changed || state.stamp.is_none() || state.canonical.is_none() {
                let resolved = canonical(path);
                if resolved != state.canonical {
                    if let Some(dir) = resolved.as_deref().and_then(Path::parent) {
                        new_dirs.push(dir.to_path_buf());
                    }
                    state.canonical = resolved;
                }
            }
        }
        changed
    }
}

impl TokioFileWatcher {
    /// Creates the platform watcher, bound to this thread's loop.
    pub fn new() -> Result<Self> {
        let wake = Arc::new(Notify::new());
        let producer_wake = Arc::clone(&wake);
        // The backend's thread only says "look again"; what changed is decided by re-stamping
        // (see the module docs). Errors and overflows are wake-ups like any other event.
        let watcher = RecommendedWatcher::new(
            move |_event: notify::Result<Event>| producer_wake.notify_one(),
            Config::default(),
        )
        .map_err(notify_error("FileWatcher"))?;
        Ok(Self {
            shared: Arc::new(Shared {
                backend: Mutex::new(Backend {
                    watcher,
                    dirs: HashSet::new(),
                }),
                files: Mutex::new(Files::default()),
                wake,
                waiting: AtomicBool::new(false),
            }),
        })
    }

    /// Adds `path` (a file, as native path bytes; its parent directory must exist, the file need
    /// not) to the watched set. See the module docs.
    pub fn watch(&self, path: &[u8]) -> Result<()> {
        let shared = &self.shared;
        let path = native_path(path)?;
        let Some(dir) = path.parent().filter(|_| path.file_name().is_some()) else {
            return Err(KjIoError::other(
                "FileWatcher::watch",
                format!("not a file path: {}", path.display()),
            ));
        };
        shared.watch_dir(dir)?;
        // A symlink's target may live elsewhere: watch that directory too, or edits to the
        // target produce no event here. (Best effort: the target may not exist yet.)
        let resolved = canonical(&path);
        if let Some(target_dir) = resolved.as_deref().and_then(Path::parent) {
            shared.watch_dir(target_dir)?;
        }
        let state = FileState {
            canonical: resolved,
            stamp: stamp(&path),
        };
        lock(&shared.files, "FileWatcher::watch")?
            .by_path
            .insert(path, state);
        Ok(())
    }

    /// Resolves the next time a watched file changes (immediately if it already has). The future
    /// owns a share of the watcher, not a borrow of `self`.
    pub fn on_change(&self) -> impl Future<Output = Result<()>> + use<> {
        let shared = Arc::clone(&self.shared);
        async move { shared.on_change().await }
    }
}

/// Clears the "an `on_change` is waiting" flag when that future settles or is dropped.
struct WaitGuard<'a>(&'a AtomicBool);

impl Drop for WaitGuard<'_> {
    fn drop(&mut self) {
        self.0.store(false, Ordering::Release);
    }
}

impl Shared {
    /// Registers `dir` with the backend, once per directory however it is spelled.
    fn watch_dir(&self, dir: &Path) -> Result<()> {
        let key = std::fs::canonicalize(dir).unwrap_or_else(|_| dir.to_path_buf());
        let mut backend = lock(&self.backend, "FileWatcher::watch")?;
        if backend.dirs.contains(&key) {
            return Ok(());
        }
        backend
            .watcher
            .watch(dir, RecursiveMode::NonRecursive)
            .map_err(notify_error("FileWatcher::watch"))?;
        backend.dirs.insert(key);
        drop(backend);
        Ok(())
    }

    /// Re-stamps the watched files and registers the directories retargeted symlinks now point
    /// into.
    fn rescan(&self) -> Result<bool> {
        let mut new_dirs = Vec::new();
        let changed = lock(&self.files, "FileWatcher::onChange")?.rescan(&mut new_dirs);
        for dir in new_dirs {
            self.watch_dir(&dir)?;
        }
        Ok(changed)
    }

    async fn on_change(&self) -> Result<()> {
        if self.waiting.swap(true, Ordering::AcqRel) {
            return Err(KjIoError::other(
                "FileWatcher::onChange",
                "at most one onChange() promise may be outstanding at a time",
            ));
        }
        let _guard = WaitGuard(&self.waiting);
        loop {
            if self.rescan()? {
                return Ok(());
            }
            // A notification that arrived during `rescan()` left a permit here, so this returns
            // at once and the next iteration sees the change.
            self.wake.notified().await;
        }
    }
}

// ======================================================================================
// Bridge entry points (see ffi.rs).

pub fn new_file_watcher() -> Result<Box<TokioFileWatcher>> {
    Ok(Box::new(TokioFileWatcher::new()?))
}

pub fn file_watcher_watch(watcher: &TokioFileWatcher, path: &[u8]) -> Result<()> {
    watcher.watch(path)
}

pub fn file_watcher_on_change(
    watcher: &TokioFileWatcher,
) -> impl Future<Output = Result<()>> + use<> {
    watcher.on_change()
}

#[cfg(test)]
#[path = "watcher-test.rs"]
mod tests;
