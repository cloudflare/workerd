// kj_rs_io::FileWatcher implementation. Each platform backend is a line-for-line port of
// workerd's kj-mode FileWatcher (workerd.c++), with the one difference that waiting for the
// notification fd to become readable goes through tokio's AsyncFd (wait_fd_readable, see
// readiness.rs) instead of kj::UnixEventPort::FdObserver::whenBecomesReadable(). Everything
// else -- which fds get created, which watch masks are used, how events are drained and
// filtered -- is kept identical so that --watch behaves the same on either event loop.

#include "kj-rs-io/file-watcher.h"

#include "kj-rs-io/ffi.rs.h"

#include <kj/debug.h>
#include <kj/io.h>
#include <kj/map.h>
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
struct FileWatcher::Impl {
  kj::OwnFd inotifyFd;

  kj::HashMap<kj::String, int> watches;
  kj::HashMap<int, kj::HashSet<kj::String>> filesWatched;

  Impl(): inotifyFd(KJ_SYSCALL_FD(inotify_init1(IN_NONBLOCK | IN_CLOEXEC))) {}

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

  kj::Promise<void> onChange() {
    kj::byte buffer[4096]{};

    for (;;) {
      ssize_t n;
      KJ_NONBLOCKING_SYSCALL(n = read(inotifyFd, buffer, sizeof(buffer)));

      if (n < 0) {
        // No more data to read: wait for the inotify fd to become readable again.
        co_await wait_fd_readable(inotifyFd.get());
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

}  // namespace kj_rs_io
