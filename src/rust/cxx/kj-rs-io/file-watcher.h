#pragma once
// kj_rs_io::FileWatcher: the tokio-loop replacement for workerd's `--watch` file watcher.
//
// Watches a set of individual files and resolves onChange() when any of them changes. The
// platform backends mirror workerd's kj FileWatcher exactly — inotify on the parent directory
// on Linux, kqueue EVFILT_VNODE per open file on macOS/BSD — but the notification fd's
// readiness is awaited through tokio's AsyncFd (kj_rs_io::wait_fd_readable) instead of
// kj::UnixEventPort::FdObserver, so it works on the tokio-backed event loop where no
// UnixEventPort exists.
//
// Behavior notes (all matching the kj version):
//  - Multiple rapid changes coalesce: onChange() resolves once for whatever is queued; calling
//    it again drains the queue before waiting, so changes are never lost between calls.
//  - Linux: the watched file itself need not exist (only its parent directory must), and a
//    file deleted and re-created is picked up again. macOS/BSD: watch() opens the file (or
//    dups the provided already-open fd), so watching a nonexistent file throws; a
//    replaced-by-rename file still fires on the old inode.
//  - All internal fds are CLOEXEC: --watch reloads via execve(), which must not leak them.
//  - Unsupported platforms (Windows, ...): isSupported() returns false, watch() is a no-op and
//    onChange() never resolves, mirroring workerd's dummy watcher.
//
// onChange() must be awaited on the thread owning the kj_rs_tokio::TokioEventPort, and at most
// one onChange() promise may be outstanding at a time (workerd awaits it sequentially). The
// promise co-owns the watcher's state, so it may outlive the FileWatcher object itself.

#include <kj/async.h>
#include <kj/filesystem.h>

namespace kj_rs_io {

class FileWatcher {
 public:
  FileWatcher();
  ~FileWatcher() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(FileWatcher);

  // False on platforms with no watcher implementation (callers should report an error).
  bool isSupported();

  // Adds `path` to the watched set. `file`, if provided, is an already-open handle for the
  // same path (the kqueue backend watches it directly via a dup'd fd; the inotify backend
  // ignores it and watches the parent directory by name).
  void watch(kj::PathPtr path, kj::Maybe<const kj::ReadableFile &> file);

  // Resolves the next time any watched file changes (immediately, if a change is already
  // queued). Eagerly evaluated, per kj-rs-io convention for I/O promises. The promise co-owns
  // the watcher's state (the notification fd, its I/O-driver registration, the watched set), so
  // destroying the FileWatcher while it is pending is safe: the state lives until the promise
  // settles or is dropped.
  kj::Promise<void> onChange();

 private:
  struct Impl;
  kj::Own<Impl> impl;
};

}  // namespace kj_rs_io
