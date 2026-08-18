// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Backend-specific implementations of workerd's CLI --watch file watcher and SIGTERM graceful
// drain (see cli-io-backend.h). The single WORKERD_RUST_IO_BACKEND_RUST #if that picks the native
// kj loop vs the tokio loop is confined to this TU, so workerd.c++'s call sites stay
// backend-agnostic -- exactly as //src/workerd/util:setup-async-io does for kj::setupAsyncIo().

#include "cli-io-backend.h"

#if _WIN32
#include <windows.h>
#include <winsock2.h>

#if !WORKERD_RUST_IO_BACKEND_RUST
// --//:io_backend=rust ships kj-async-core only, which has no Win32EventPort and doesn't provide
// this header. The native watcher sites that need it are all gated out below.
#include <kj/async-win32.h>
#endif
#include <kj/win32-api-version.h>
#include <kj/windows-sanity.h>
#else
#include <fcntl.h>
#include <unistd.h>

#include <csignal>
#include <cstring>

#if !WORKERD_RUST_IO_BACKEND_RUST
// --//:io_backend=rust ships kj-async-core only, which has no UnixEventPort and doesn't provide
// this header. The native watcher + captureSignal/onSignal sites that need it are all gated out
// below; the tokio (kj-rs-io) loop covers those paths.
#include <kj/async-unix.h>
#endif
#endif

#if __linux__
#include <sys/inotify.h>
#elif __APPLE__ || __FreeBSD__ || __OpenBSD__ || __NetBSD__ || __DragonFly__
#define WORKERD_USE_KQUEUE_FOR_FILE_WATCHER 1
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#endif

#if WORKERD_RUST_IO_BACKEND_RUST
#include <kj-rs-io/async-io.h>
#include <kj-rs-io/file-watcher.h>
#endif

#include <kj/debug.h>
#include <kj/map.h>
#include <kj/vector.h>

namespace workerd::server {

// The native (kj loop, kj::UnixEventPort::FdObserver) watcher is compiled only in the cxx config.
// Under --//:io_backend=rust there is no UnixEventPort, so --watch always uses the tokio watcher
// (below), which observes the same inotify/kqueue fds through tokio's AsyncFd.
#if !WORKERD_RUST_IO_BACKEND_RUST

#if __linux__

// Class which uses inotify to watch a set of files and alert when they change.
class KjFileWatcher final: public FileWatcher {
 public:
  KjFileWatcher(kj::UnixEventPort& port)
      : inotifyFd(makeInotify()),
        observer(port, inotifyFd, kj::UnixEventPort::FdObserver::OBSERVE_READ) {}

  bool isSupported() override {
    return true;
  }

  void watch(kj::PathPtr path, kj::Maybe<const kj::ReadableFile&> file) override {
    // `file` is provided if available. The Linux implementation doesn't use it.

    auto pathStr = path.parent().toNativeString(true);

    int wd = watches.findOrCreate(pathStr, [&]() {
      int wd;
      uint32_t mask = IN_DELETE | IN_MODIFY | IN_MOVE | IN_CREATE;
      KJ_SYSCALL(wd = inotify_add_watch(inotifyFd, pathStr.cStr(), mask));
      return decltype(watches)::Entry{kj::mv(pathStr), wd};
    });

    auto& files =
        filesWatched.findOrCreate(wd, [&]() { return decltype(filesWatched)::Entry{wd, {}}; });

    files.upsert(kj::str(path.basename()[0]), [](auto&&...) {});
  }

  kj::Promise<void> onChange() override {
    kj::byte buffer[4096]{};

    for (;;) {
      ssize_t n;
      KJ_NONBLOCKING_SYSCALL(n = read(inotifyFd, buffer, sizeof(buffer)));

      if (n < 0) {
        // No more data to read.
        co_await observer.whenBecomesReadable();
        continue;
      }

      kj::byte* ptr = buffer;
      while (n > 0) {
        KJ_ASSERT(n >= sizeof(struct inotify_event));

        auto& event = *reinterpret_cast<struct inotify_event*>(ptr);
        size_t eventSize = sizeof(struct inotify_event) + event.len;
        KJ_ASSERT(n >= eventSize);
        KJ_ASSERT(eventSize % sizeof(void*) == 0);
        ptr += eventSize;
        n -= eventSize;

        if (event.len > 0 && event.name[0] != '\0') {
          auto& watched = KJ_ASSERT_NONNULL(filesWatched.find(event.wd));
          if (watched.find(kj::StringPtr(event.name)) != kj::none) {
            // HIT! We saw a change.
            co_return;
          }
        }
      }
    }
  }

 private:
  kj::OwnFd inotifyFd;
  kj::UnixEventPort::FdObserver observer;

  kj::HashMap<kj::String, int> watches;
  kj::HashMap<int, kj::HashSet<kj::String>> filesWatched;

  static kj::OwnFd makeInotify() {
    return KJ_SYSCALL_FD(inotify_init1(IN_NONBLOCK | IN_CLOEXEC));
  }
};

#elif WORKERD_USE_KQUEUE_FOR_FILE_WATCHER

// Class which uses inotify to watch a set of files and alert when they change.
//
// This version uses kqueue to watch for changes in files. kqueue typically doesn't scale well
// to watching whole directory trees, since it must keep a file descriptor open for each watched

}  // namespace workerd::server
