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
// file. However, for our use case, we don't really want to watch a directory tree anyway, we
// want to watch the specific set of files which were opened while parsing the config. This is
// not so bad, probably.
//
// Apple provides the FSEvents API as an alternative, but it seems way more complicated and I
// can't tell if it would provide a real advantage. Plus, kqueue works on BSD systems.
class KjFileWatcher final: public FileWatcher {
 public:
  KjFileWatcher(kj::UnixEventPort& port)
      : kqueueFd(makeKqueue()),
        observer(port, kqueueFd, kj::UnixEventPort::FdObserver::OBSERVE_READ) {}

  bool isSupported() override {
    return true;
  }

  void watch(kj::PathPtr path, kj::Maybe<const kj::ReadableFile&> file) override {
    KJ_IF_SOME(f, file) {
      KJ_IF_SOME(fd, f.getFd()) {
        // We need to duplicate the FD because the original will probably be closed later and
        // closing the FD unregisters it from kqueue.
        watchFd(KJ_SYSCALL_FD(dup(fd)));
        return;
      }
    }

    // No existing file, open from disk.
    watchFd(KJ_SYSCALL_FD(open(path.toNativeString(true).cStr(), O_RDONLY)));
  }

  kj::Promise<void> onChange() override {
    for (;;) {
      struct kevent event;
      struct timespec timeout;
      memset(&event, 0, sizeof(event));
      memset(&timeout, 0, sizeof(timeout));

      int n;
      KJ_SYSCALL(n = kevent(kqueueFd, nullptr, 0, &event, 1, &timeout));

      if (n == 0) {
        // No events, wait for the kqueue to become readable indicating an event has been
        // delivered.
        co_await observer.whenBecomesReadable();
        continue;
      } else {
        // We only pay attention to events that indicate changes in the first place, so there's
        // no need to examine the event, it definitely means something changed.
        co_return;
      }
    }
  }

 private:
  kj::OwnFd kqueueFd;
  kj::UnixEventPort::FdObserver observer;
  kj::Vector<kj::OwnFd> filesWatched;

  static kj::OwnFd makeKqueue() {
    auto fd = KJ_SYSCALL_FD(kqueue());
    KJ_SYSCALL(fcntl(fd, F_SETFD, FD_CLOEXEC));
    return kj::mv(fd);
  }

  void watchFd(kj::OwnFd fd) {
    KJ_SYSCALL(fcntl(fd, F_SETFD, FD_CLOEXEC));

    struct kevent change;
    memset(&change, 0, sizeof(change));
    change.ident = fd.get();
    change.filter = EVFILT_VNODE;
    change.flags = EV_ADD | EV_CLEAR;
    change.fflags = NOTE_WRITE | NOTE_EXTEND | NOTE_DELETE | NOTE_RENAME;
    KJ_SYSCALL(kevent(kqueueFd, &change, 1, nullptr, 0, nullptr));
    filesWatched.add(kj::mv(fd));
  }
};

#elif _WIN32

class KjFileWatcher final: public FileWatcher {
 public:
  KjFileWatcher(kj::Win32EventPort& port) {}

  bool isSupported() override {
    return false;
  }

  void watch(kj::PathPtr path, kj::Maybe<const kj::ReadableFile&> file) override {}

  kj::Promise<void> onChange() override {
    return kj::NEVER_DONE;
  }

 private:
};

#else

// Dummy KjFileWatcher implementation for operating systems that aren't supported yet.
class KjFileWatcher final: public FileWatcher {
 public:
  KjFileWatcher(kj::UnixEventPort& port) {}

  bool isSupported() override {
    return false;
  }

  void watch(kj::PathPtr path, kj::Maybe<const kj::ReadableFile&> file) override {}

  kj::Promise<void> onChange() override {
    return kj::NEVER_DONE;
  }

 private:
};

#endif  // #__linux__, #else

#endif  // !WORKERD_RUST_IO_BACKEND_RUST

// FileWatcher for --//:io_backend=rust: the same inotify/kqueue backends as the native watcher
// (line-for-line ports living in src/rust/cxx/kj-rs-io), but fd readiness is awaited through
// tokio's AsyncFd instead of kj::UnixEventPort::FdObserver, which doesn't exist on the tokio
// loop. All fds it creates are CLOEXEC, so reloadFromConfigChange()'s execve() doesn't leak
// them (the deliberately-inherited sockets get FIONCLEX'd there explicitly).
#if WORKERD_RUST_IO_BACKEND_RUST
class TokioFileWatcher final: public FileWatcher {
 public:
  bool isSupported() override {
    return inner.isSupported();
  }

  void watch(kj::PathPtr path, kj::Maybe<const kj::ReadableFile&> file) override {
    inner.watch(path, file);
  }

  kj::Promise<void> onChange() override {
    return inner.onChange();
  }

 private:
  kj_rs_io::FileWatcher inner;
};
#endif  // WORKERD_RUST_IO_BACKEND_RUST

kj::Own<FileWatcher> makeFileWatcher([[maybe_unused]] kj::AsyncIoContext& io) {
#if WORKERD_RUST_IO_BACKEND_RUST
  // No UnixEventPort exists on the tokio loop; use the AsyncFd-backed watcher instead.
  return kj::heap<TokioFileWatcher>();
#elif _WIN32
  return kj::heap<KjFileWatcher>(io.win32EventPort);
#else
  return kj::heap<KjFileWatcher>(io.unixEventPort);
#endif
}

#if !_WIN32
void captureSigterm() {
#if !WORKERD_RUST_IO_BACKEND_RUST
  kj::UnixEventPort::captureSignal(SIGTERM);
#endif
  // Under --//:io_backend=rust this is a no-op: tokio's signal driver watches SIGTERM instead, and
  // capturing it here would block the signal in the thread's mask and prevent the tokio handler
  // from ever being invoked (there is no UnixEventPort under that backend anyway).
}

kj::Promise<void> onSigterm([[maybe_unused]] kj::AsyncIoContext& io) {
#if WORKERD_RUST_IO_BACKEND_RUST
  return kj_rs_io::onSignal(SIGTERM);
#else
  return io.unixEventPort.onSignal(SIGTERM).ignoreResult();
#endif
}
#endif  // !_WIN32

}  // namespace workerd::server
