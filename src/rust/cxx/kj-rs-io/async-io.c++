#include "kj-rs-io/async-io.h"

#include <kj/debug.h>

#include <cstring>

#if _WIN32
#include <winsock2.h>
#else
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#if __APPLE__ || __FreeBSD__ || __OpenBSD__ || __NetBSD__ || __DragonFly__
#include <sys/ucred.h>
#endif
#endif

namespace kj_rs_io {

// =======================================================================================
// TokioAsyncIoStream

kj::Promise<size_t> TokioAsyncIoStream::tryRead(void *buffer, size_t minBytes, size_t maxBytes) {
  return stream_try_read(
      *inner, ::rust::Slice<uint8_t>(reinterpret_cast<uint8_t *>(buffer), maxBytes), minBytes);
}

}  // namespace kj_rs_io
