// kj_rs_io::FileWatcher implementation. Each platform backend is a line-for-line port of
// workerd's kj-mode FileWatcher (workerd.c++), with the one difference that waiting for the
// notification fd to become readable goes through tokio's AsyncFd (TokioFdWatcher, see
// readiness.rs) instead of kj::UnixEventPort::FdObserver::whenBecomesReadable(). Everything
// else -- which fds get created, which watch masks are used, how events are drained and
// filtered -- is kept identical so that --watch behaves the same on either event loop.

#include "kj-rs-io/file-watcher.h"

#include "kj-rs-io/ffi.rs.h"

#include <kj/debug.h>
#include <kj/io.h>
#include <kj/map.h>
#include <kj/refcount.h>
#include <kj/vector.h>

#include <cstring>

#if __linux__
#include <fcntl.h>
#include <sys/inotify.h>
#include <unistd.h>
#elif __APPLE__ || __FreeBSD__ || __OpenBSD__ || __NetBSD__ || __DragonFly__
#define KJ_RS_IO_USE_KQUEUE_FOR_FILE_WATCHER 1
#include <fcntl.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace kj_rs_io {

#if __linux__

// inotify backend. Watches each file's parent directory (IN_DELETE | IN_MODIFY | IN_MOVE |
// IN_CREATE) and filters events by basename, so files replaced by rename (editors' atomic
// saves) or deleted-and-recreated keep firing, and the watched file itself need not exist yet.
struct FileWatcher::Impl: public kj::Refcounted {
  kj::OwnFd inotifyFd;
  // Owns a dup of `inotifyFd` and its one registration with the tokio I/O driver (see
  // readiness.rs): declared after the fd it was created from, destroyed before it.
  ::rust::Box<TokioFdWatcher> watcher;

  kj::HashMap<kj::String, int> watches;
  kj::HashMap<int, kj::HashSet<kj::String>> filesWatched;

  Impl()
      : inotifyFd(KJ_SYSCALL_FD(inotify_init1(IN_NONBLOCK | IN_CLOEXEC))),
        watcher(new_fd_watcher(inotifyFd.get())) {}

  bool isSupported() {
    return true;
  }

  void watch(kj::PathPtr path, kj::Maybe<const kj::ReadableFile &> file) {
    // The inotify backend doesn't use `file`; it watches the parent directory.

    auto pathStr = path.parent().toNativeString(true);

    int wd = watches.findOrCreate(pathStr, [&]() {
      int wd;
      uint32_t mask = IN_DELETE | IN_MODIFY | IN_MOVE | IN_CREATE;
      KJ_SYSCALL(wd = inotify_add_watch(inotifyFd, pathStr.cStr(), mask));
      return decltype(watches)::Entry{kj::mv(pathStr), wd};
    });

    auto &files =
        filesWatched.findOrCreate(wd, [&]() { return decltype(filesWatched)::Entry{wd, {}}; });

    files.upsert(kj::str(path.basename()[0]), [](auto &&...) {});
  }

  // `self` is this Impl's own refcount, held by the coroutine frame: the frame reads `this`
  // across every co_await, so it keeps its referent alive itself rather than relying on the
  // FileWatcher outliving the promise.
  kj::Promise<void> onChange(kj::Own<Impl> self) {
    kj::byte buffer[4096]{};

    for (;;) {
      ssize_t n;
      KJ_NONBLOCKING_SYSCALL(n = read(inotifyFd, buffer, sizeof(buffer)));

      if (n < 0) {
        // No more data to read: wait for the inotify fd to become readable again.
        co_await watcher->readable();
        continue;
      }

      kj::byte *ptr = buffer;
      while (n > 0) {
        KJ_ASSERT(n >= sizeof(struct inotify_event));

        auto &event = *reinterpret_cast<struct inotify_event *>(ptr);
        size_t eventSize = sizeof(struct inotify_event) + event.len;
        KJ_ASSERT(n >= eventSize);
        KJ_ASSERT(eventSize % sizeof(void *) == 0);
        ptr += eventSize;
        n -= eventSize;

        if (event.len > 0 && event.name[0] != '\0') {
          auto &watched = KJ_ASSERT_NONNULL(filesWatched.find(event.wd));
          if (watched.find(kj::StringPtr(event.name)) != kj::none) {
            // HIT! We saw a change.
            co_return;
          }
        }
      }
    }
  }
};

#elif KJ_RS_IO_USE_KQUEUE_FOR_FILE_WATCHER

// kqueue backend. One EVFILT_VNODE registration per watched file (dup of the already-open
// config fd when available, else opened by path -- so the path must exist). NOTE_DELETE /
// NOTE_RENAME on the old inode cover atomic-rename saves. kqueue doesn't scale to whole
// directory trees, but we only watch the specific files opened while parsing the config.
struct FileWatcher::Impl: public kj::Refcounted {
  kj::OwnFd kqueueFd;
  // Owns a dup of `kqueueFd` and its one registration with the tokio I/O driver (see
  // readiness.rs): declared after the fd it was created from, destroyed before it.
  ::rust::Box<TokioFdWatcher> watcher;
  kj::Vector<kj::OwnFd> filesWatched;

  Impl(): kqueueFd(makeKqueue()), watcher(new_fd_watcher(kqueueFd.get())) {}

  bool isSupported() {
    return true;
  }

  void watch(kj::PathPtr path, kj::Maybe<const kj::ReadableFile &> file) {
    KJ_IF_SOME(f, file) {
      KJ_IF_SOME(fd, f.getFd()) {
        // We need to duplicate the fd because the original will probably be closed later, and
        // closing the fd unregisters it from kqueue.
        watchFd(KJ_SYSCALL_FD(dup(fd)));
        return;
      }
    }

    // No existing file, open from disk.
    watchFd(KJ_SYSCALL_FD(open(path.toNativeString(true).cStr(), O_RDONLY)));
  }

  // `self` is this Impl's own refcount, held by the coroutine frame (see the inotify backend).
  kj::Promise<void> onChange(kj::Own<Impl> self) {
    for (;;) {
      struct kevent event;
      struct timespec timeout;
      memset(&event, 0, sizeof(event));
      memset(&timeout, 0, sizeof(timeout));

      int n;
      KJ_SYSCALL(n = kevent(kqueueFd, nullptr, 0, &event, 1, &timeout));

      if (n == 0) {
        // No events: wait for the kqueue fd to become readable, indicating an event has been
        // delivered.
        co_await watcher->readable();
        continue;
      } else {
        // We only registered for events that indicate changes in the first place, so there's
        // no need to examine the event: it definitely means something changed.
        co_return;
      }
    }
  }

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

#else

// Dummy backend for platforms without an implementation (Windows, ...), mirroring workerd's:
// isSupported() returns false, which workerd surfaces as a clean CLI error for --watch
// ("File watching is not yet implemented on your OS") rather than a crash. A real Windows
// backend (e.g. ReadDirectoryChangesW, perhaps via the notify crate) is a potential follow-up.
struct FileWatcher::Impl: public kj::Refcounted {
  bool isSupported() {
    return false;
  }

  void watch(kj::PathPtr path, kj::Maybe<const kj::ReadableFile &> file) {}

  kj::Promise<void> onChange(kj::Own<Impl>) {
    return kj::NEVER_DONE;
  }
};

#endif

FileWatcher::FileWatcher(): impl(kj::refcounted<Impl>()) {}
FileWatcher::~FileWatcher() noexcept(false) = default;

bool FileWatcher::isSupported() {
  return impl->isSupported();
}

void FileWatcher::watch(kj::PathPtr path, kj::Maybe<const kj::ReadableFile &> file) {
  impl->watch(path, file);
}

kj::Promise<void> FileWatcher::onChange() {
  // The coroutine frame co-owns the Impl (see Impl::onChange), so the returned promise stays
  // valid even if this FileWatcher is destroyed first: no outlive-the-watcher contract.
  return impl->onChange(kj::addRef(*impl));
}

}  // namespace kj_rs_io
