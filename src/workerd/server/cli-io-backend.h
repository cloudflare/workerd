// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// The two pieces of workerd's CLI main() whose implementation differs between the native kj event
// loop (default, cxx build) and the tokio loop (--//:io_backend=rust): the --watch file watcher
// and the SIGTERM graceful-drain signal. Both are presented here as backend-agnostic entry points
// so that workerd.c++ links against them unchanged in both configs -- no
// WORKERD_RUST_IO_BACKEND_RUST at the call site. The single compile-time #if that selects the
// backend lives in cli-io-backend.c++, mirroring how //src/workerd/util:setup-async-io makes
// kj::setupAsyncIo() transparent.

#include <kj/async-io.h>
#include <kj/async.h>
#include <kj/filesystem.h>
#include <kj/memory.h>

namespace workerd::server {

// Interface for watching the files the server depends on (parsed config files, worker source
// files, and the server binary itself) and alerting when any of them change; drives --watch. Two
// implementations, selected by the active I/O backend inside makeFileWatcher():
//
//  - the native watcher: inotify (Linux) / kqueue (macOS, BSDs) readiness observed through
//    kj::UnixEventPort::FdObserver. Requires the native KJ event loop.
//  - the tokio watcher: same platform backends, but readiness observed through tokio's AsyncFd
//    (kj_rs_io::FileWatcher from src/rust/cxx/kj-rs-io); used under --//:io_backend=rust, where no
//    UnixEventPort exists.
//
// Everything downstream (SchemaFileImpl's watch registration, waitForChanges()'s coalescing,
// serveImpl()'s re-exec loop) is shared between the two.
class FileWatcher {
 public:
  virtual ~FileWatcher() noexcept(false) = default;

  // False on platforms where watching is not implemented (callers report a CLI error).
  virtual bool isSupported() = 0;

  // Adds `path` to the watched set. `file` is an already-open handle for the same path, if
  // available (the kqueue backends watch the open file directly; others open by path).
  virtual void watch(kj::PathPtr path, kj::Maybe<const kj::ReadableFile&> file) = 0;

  // Resolves the next time any watched file changes. Changes are queued by the kernel, not
  // lost between calls; call again after resolution to wait for further changes.
  virtual kj::Promise<void> onChange() = 0;
};

// Constructs the FileWatcher appropriate for the active I/O backend. Under the native (cxx) loop
// this is the UnixEventPort::FdObserver-based watcher, driven by `io`'s event port; under
// --//:io_backend=rust it is the tokio AsyncFd-based watcher, which ignores `io`.
kj::Own<FileWatcher> makeFileWatcher(kj::AsyncIoContext& io);

#if !_WIN32
// Captures SIGTERM so the native loop can later deliver it to onSigterm(). Under the native (cxx)
// loop this is kj::UnixEventPort::captureSignal(SIGTERM); under --//:io_backend=rust it is a no-op
// (tokio's own signal driver handles SIGTERM, and capturing would block it from that driver). Call
// once in main() before the event loop is created.
void captureSigterm();

// Resolves when SIGTERM is received; used as Server::run()'s drainWhen promise. Under the native
// (cxx) loop this is io.unixEventPort.onSignal(SIGTERM); under --//:io_backend=rust it is a
// tokio-signal-backed promise from kj-rs-io (and `io` is ignored).
kj::Promise<void> onSigterm(kj::AsyncIoContext& io);
#endif

}  // namespace workerd::server
