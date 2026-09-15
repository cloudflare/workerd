// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// The two pieces of workerd's CLI main() that talk to the OS event loop directly: the --watch
// file watcher and the SIGTERM graceful-drain signal, behind small backend-agnostic entry points
// so that workerd.c++ does not name kj::UnixEventPort itself.

#include <kj/async-io.h>
#include <kj/async.h>
#include <kj/filesystem.h>
#include <kj/memory.h>

namespace workerd::server {

// Interface for watching the files the server depends on (parsed config files, worker source
// files, and the server binary itself) and alerting when any of them change; drives --watch. The
// implementation is inotify (Linux) / kqueue (macOS, BSDs) readiness observed through
// kj::UnixEventPort::FdObserver, constructed by makeFileWatcher(). Everything downstream
// (SchemaFileImpl's watch registration, waitForChanges()'s coalescing, serveImpl()'s re-exec
// loop) works against this interface.
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

// Constructs the FileWatcher for this platform, driven by `io`'s event port.
kj::Own<FileWatcher> makeFileWatcher(kj::AsyncIoContext& io);

#if !_WIN32
// Captures SIGTERM so the loop can later deliver it to onSigterm()
// (kj::UnixEventPort::captureSignal(SIGTERM)). Call once in main() before the event loop is
// created.
void captureSigterm();

// Resolves when SIGTERM is received (io.unixEventPort.onSignal(SIGTERM)); used as Server::run()'s
// drainWhen promise.
kj::Promise<void> onSigterm(kj::AsyncIoContext& io);
#endif

}  // namespace workerd::server
