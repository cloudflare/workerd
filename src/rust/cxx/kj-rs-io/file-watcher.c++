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

namespace kj_rs_io {}  // namespace kj_rs_io
