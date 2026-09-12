#pragma once
// kj_rs_io::FileWatcher: the tokio-loop replacement for workerd's `--watch` file watcher.
//
// A thin KJ-interface wrapper over the Rust watcher in watcher.rs (the `notify` crate: inotify on
// Linux, kqueue on macOS/BSD, ReadDirectoryChangesW on Windows). Watches a set of individual
// files and resolves onChange() when any of them changes. Behavior workerd depends on:
//
//  - Watched by name: the file need not exist yet (only its parent directory must), and a file
//    replaced by rename (editors' atomic saves) or deleted and recreated keeps firing.
//  - Multiple rapid changes coalesce: onChange() resolves once for whatever is queued; calling it
//    again drains the queue before waiting, so changes are never lost between calls.
//  - Attribute-only changes and reads do not fire.
//  - Supported on every platform kj-rs-io builds for (isSupported() is always true).
//
// onChange() must be awaited on the thread owning the kj_rs_tokio::TokioEventPort, and at most
// one onChange() promise may be outstanding at a time (a second concurrent one rejects; workerd
// awaits it sequentially). The promise owns a share of the watcher's state, so it may outlive
// the FileWatcher object itself.

#include "kj-rs-io/ffi.rs.h"

#include <kj/async.h>
#include <kj/filesystem.h>

namespace kj_rs_io {

class FileWatcher {
 public:
  FileWatcher();
  KJ_DISALLOW_COPY_AND_MOVE(FileWatcher);

  // Always true: every platform has a backend.
  bool isSupported();

  // Adds `path` to the watched set. `file` (an already-open handle for the same path) is
  // accepted for interface compatibility with workerd's watcher and ignored: watching is by
  // name, so an open handle adds nothing.
  void watch(kj::PathPtr path, kj::Maybe<const kj::ReadableFile &> file);

  // Resolves the next time any watched file changes (immediately, if a change is already
  // queued). Started eagerly, per kj-rs-io's operation-start policy.
  kj::Promise<void> onChange();

 private:
  ::rust::Box<TokioFileWatcher> inner;
};

}  // namespace kj_rs_io
