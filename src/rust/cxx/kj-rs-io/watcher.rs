//! The `--watch` file watcher, over the `notify` crate (inotify on Linux, kqueue on macOS/BSD,
//! `ReadDirectoryChangesW` on Windows).
//!
//! This backs `kj_rs_io::FileWatcher` (file-watcher.h), the tokio-loop replacement for workerd's
//! native watcher. Semantics workerd depends on, and how they are met here:
//!
//! - **Watch by name.** Each watched file's *parent directory* is watched (non-recursively), so
//!   the file need not exist yet, and a file replaced by `rename(2)` (an editor's atomic save) or
//!   deleted and recreated keeps firing: events are matched on (directory, basename). This is
//!   what the native inotify backend does; the native kqueue backend watched the open fd and
//!   went blind after a replace, which this does not.
//! - **Modifications.** The file path itself is watched too (best effort: it may not exist yet),
//!   because kqueue reports writes to a file only through a watch on that file, and re-watched
//!   after a create or rename so the new inode keeps reporting.
//! - **Coalescing.** `on_change` drains every event already queued, resolves once if any of them
//!   was relevant, and otherwise waits for the next; a burst of writes is one resolution, and
//!   calling it again after a change drains the rest of that burst before waiting.
//! - **Filtering.** Attribute-only changes (`chmod`, `touch -a`) and access events do not fire,
//!   matching the native watch masks (`IN_DELETE|IN_MODIFY|IN_MOVE|IN_CREATE`,
//!   `NOTE_WRITE|EXTEND|DELETE|RENAME`).
//! - **Threads.** notify delivers events on its own thread; they cross to the KJ loop through an
//!   unbounded tokio channel whose receiver is awaited on the loop (the wake goes through the
//!   thread-safe kj-rs waker bridge). Every `on_change` future owns a share of the watcher state
//!   (see stream.rs, "Operations own their state"), so a `FileWatcher` destroyed while a promise
//!   is pending is memory-safe and the promise still resolves.

use std::cell::RefCell;
use std::collections::HashMap;
use std::collections::HashSet;
use std::ffi::OsString;
use std::future::Future;
use std::path::Path;
use std::path::PathBuf;
use std::rc::Rc;

use notify::Config;
use notify::Event;
use notify::EventKind;
use notify::RecommendedWatcher;
use notify::RecursiveMode;
use notify::Watcher;
use notify::event::ModifyKind;
use tokio::sync::mpsc;

use crate::error::KjIoError;
use crate::error::Result;

/// See the module docs. Loop-thread only (`!Send`), like the C++ `FileWatcher` owning it.
pub struct TokioFileWatcher {
    shared: Rc<Shared>,
}

struct Shared {
    watcher: RefCell<RecommendedWatcher>,
    /// Watched basenames per watched directory.
    watched: RefCell<HashMap<PathBuf, HashSet<OsString>>>,
    /// Events from notify's thread. Only one `on_change` may hold it at a time (workerd awaits
    /// `onChange()` sequentially); a second concurrent caller gets an error, not a panic.
    events: RefCell<mpsc::UnboundedReceiver<notify::Result<Event>>>,
}

fn notify_error(op: &'static str) -> impl Fn(notify::Error) -> KjIoError {
    move |e| KjIoError::other(op, e)
}

impl TokioFileWatcher {
    /// Creates the platform watcher. Needs the loop runtime: the event channel's receiver is
    /// awaited there.
    pub fn new() -> Result<Self> {
        crate::runtime::require_loop_runtime()?;
        let (tx, rx) = mpsc::unbounded_channel();
        let watcher = RecommendedWatcher::new(
            move |event: notify::Result<Event>| {
                // The receiver being gone means the watcher is being torn down; nothing to do.
                let _ = tx.send(event);
            },
            Config::default(),
        )
        .map_err(notify_error("FileWatcher"))?;
        Ok(Self {
            shared: Rc::new(Shared {
                watcher: RefCell::new(watcher),
                watched: RefCell::new(HashMap::new()),
                events: RefCell::new(rx),
            }),
        })
    }

    /// Adds `path` (a file; its parent directory must exist, the file need not) to the watched
    /// set. See the module docs.
    pub fn watch(&self, path: &str) -> Result<()> {
        let path = PathBuf::from(path);
        let (Some(dir), Some(name)) = (path.parent(), path.file_name()) else {
            return Err(KjIoError::other(
                "FileWatcher::watch",
                format!("not a file path: {}", path.display()),
            ));
        };
        let mut watcher = self.shared.watcher.borrow_mut();
        let mut registry = self.shared.watched.borrow_mut();
        if !registry.contains_key(dir) {
            watcher
                .watch(dir, RecursiveMode::NonRecursive)
                .map_err(notify_error("FileWatcher::watch"))?;
        }
        registry
            .entry(dir.to_path_buf())
            .or_default()
            .insert(name.to_os_string());
        // Best effort: reports writes to the file where the directory watch alone would not
        // (kqueue). A missing file is watched by name through the directory until it appears.
        let _ = watcher.watch(&path, RecursiveMode::NonRecursive);
        Ok(())
    }

    /// Resolves the next time a watched file changes (immediately if a change is already
    /// queued). The future owns a share of the watcher, not a borrow of `self`.
    pub fn on_change(&self) -> impl Future<Output = Result<()>> + use<> {
        let shared = Rc::clone(&self.shared);
        async move { shared.on_change().await }
    }
}

impl Shared {
    /// Whether `event` concerns a watched file in a way that counts as a change.
    fn is_relevant(&self, event: &Event) -> bool {
        if matches!(
            event.kind,
            EventKind::Access(_) | EventKind::Modify(ModifyKind::Metadata(_))
        ) {
            return false;
        }
        let watched = self.watched.borrow();
        event.paths.iter().any(|path| {
            let (Some(dir), Some(name)) = (path.parent(), path.file_name()) else {
                return false;
            };
            watched.get(dir).is_some_and(|names| names.contains(name))
        })
    }

    /// After a create or rename of a watched file, re-watch the path so the *new* inode's writes
    /// are reported (kqueue watches inodes, not names). Failures are fine: the directory watch
    /// still covers the file by name.
    fn rearm(&self, event: &Event) {
        if !matches!(
            event.kind,
            EventKind::Create(_) | EventKind::Modify(ModifyKind::Name(_))
        ) {
            return;
        }
        let mut watcher = self.watcher.borrow_mut();
        for path in &event.paths {
            if Path::new(path).is_file() {
                let _ = watcher.unwatch(path);
                let _ = watcher.watch(path, RecursiveMode::NonRecursive);
            }
        }
    }

    /// Consumes one event; `Ok(true)` if it was a relevant change.
    fn consume(&self, event: notify::Result<Event>) -> Result<bool> {
        let event = event.map_err(notify_error("FileWatcher"))?;
        if self.is_relevant(&event) {
            self.rearm(&event);
            Ok(true)
        } else {
            Ok(false)
        }
    }

    // Holding the RefMut across the await is the "one onChange() at a time" guard: a concurrent
    // second call fails at `try_borrow_mut` with a clear error instead of racing for events.
    #[expect(
        clippy::await_holding_refcell_ref,
        reason = "the held RefMut is the one-waiter guard; a concurrent caller gets an error"
    )]
    async fn on_change(&self) -> Result<()> {
        let mut events = self.events.try_borrow_mut().map_err(|_| {
            KjIoError::other(
                "FileWatcher::onChange",
                "at most one onChange() promise may be outstanding at a time",
            )
        })?;
        let mut changed = false;
        loop {
            // Drain everything already queued: a burst is one resolution.
            loop {
                match events.try_recv() {
                    Ok(event) => changed |= self.consume(event)?,
                    Err(mpsc::error::TryRecvError::Empty) => break,
                    Err(mpsc::error::TryRecvError::Disconnected) => {
                        return Err(KjIoError::other(
                            "FileWatcher::onChange",
                            "the file watcher's event thread is gone",
                        ));
                    }
                }
            }
            if changed {
                return Ok(());
            }
            match events.recv().await {
                Some(event) => changed |= self.consume(event)?,
                None => {
                    return Err(KjIoError::other(
                        "FileWatcher::onChange",
                        "the file watcher's event thread is gone",
                    ));
                }
            }
        }
    }
}

// ======================================================================================
// Bridge entry points (see ffi.rs).

pub fn new_file_watcher() -> Result<Box<TokioFileWatcher>> {
    Ok(Box::new(TokioFileWatcher::new()?))
}

pub fn file_watcher_watch(watcher: &TokioFileWatcher, path: &str) -> Result<()> {
    watcher.watch(path)
}

pub fn file_watcher_on_change(
    watcher: &TokioFileWatcher,
) -> impl Future<Output = Result<()>> + use<> {
    watcher.on_change()
}

#[cfg(test)]
mod tests {
    use static_assertions::assert_not_impl_any;

    use super::*;

    // A loop-thread handle to Rc-shared state, like the other kj-rs-io objects.
    assert_not_impl_any!(TokioFileWatcher: Send, Sync);

    #[test]
    fn watch_rejects_a_path_without_a_file_name() {
        let _port = kj_rs_tokio::TokioPort::new();
        let watcher = TokioFileWatcher::new().unwrap();
        let err = cxx::KjError::from(watcher.watch("/").unwrap_err());
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
            watcher.watch("/nonexistent-kj-rs-io-dir/file.txt").is_err(),
            "a missing directory cannot be watched (the file itself may be missing)"
        );
    }

    #[test]
    fn relevance_is_by_directory_and_basename_and_ignores_metadata_and_access() {
        let _port = kj_rs_tokio::TokioPort::new();
        let watcher = TokioFileWatcher::new().unwrap();
        let dir = std::env::temp_dir();
        let file = dir.join("kj-rs-io-watcher-relevance.txt");
        watcher.watch(&file.to_string_lossy()).unwrap();
        let shared = &watcher.shared;
        let ev = |kind: EventKind, path: &Path| Event::new(kind).add_path(path.to_path_buf());
        assert!(shared.is_relevant(&ev(EventKind::Modify(ModifyKind::Any), &file)));
        assert!(shared.is_relevant(&ev(
            EventKind::Remove(notify::event::RemoveKind::Any),
            &file
        )));
        assert!(!shared.is_relevant(&ev(
            EventKind::Modify(ModifyKind::Any),
            &dir.join("kj-rs-io-watcher-other.txt")
        )));
        assert!(!shared.is_relevant(&ev(
            EventKind::Modify(ModifyKind::Metadata(notify::event::MetadataKind::Any)),
            &file
        )));
        assert!(!shared.is_relevant(&ev(
            EventKind::Access(notify::event::AccessKind::Any),
            &file
        )));
    }
}
