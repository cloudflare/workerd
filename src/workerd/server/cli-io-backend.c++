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

namespace workerd::server {}  // namespace workerd::server
