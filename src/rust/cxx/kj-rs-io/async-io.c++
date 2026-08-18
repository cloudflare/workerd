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

kj::Promise<void> TokioAsyncIoStream::write(kj::ArrayPtr<const kj::byte> buffer) {
  return stream_write(*inner, ::rust::Slice<const uint8_t>(buffer.begin(), buffer.size()));
}

kj::Promise<void> TokioAsyncIoStream::write(
    kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) {
  // Sequential write-all of each piece (each single write is eager-by-default, preserving
  // hot-write semantics for the whole sequence).
  // TODO(perf): vectored writes via try_write_vectored.
  return writePieces(pieces);
}

kj::Promise<void> TokioAsyncIoStream::writePieces(
    kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) {
  for (auto piece: pieces) {
    co_await write(piece);
  }
}

kj::Promise<void> TokioAsyncIoStream::whenWriteDisconnected() {
  return stream_when_write_disconnected(*inner);
}

void TokioAsyncIoStream::shutdownWrite() {
  stream_shutdown_write(*inner);
}

void TokioAsyncIoStream::getsockopt(int level, int option, void *value, kj::uint *length) {
  // The platform seam lives on the Rust side (stream_getsockopt); errors surface as kj
  // exceptions, like KJ_SYSCALL. Raw socklen in/out semantics: the syscall's reported length
  // is mirrored back verbatim.
  *length = stream_getsockopt(
      *inner, level, option, ::rust::Slice<uint8_t>(reinterpret_cast<uint8_t *>(value), *length));
}

void TokioAsyncIoStream::setsockopt(int level, int option, const void *value, kj::uint length) {
  stream_setsockopt(*inner, level, option,
      ::rust::Slice<const uint8_t>(reinterpret_cast<const uint8_t *>(value), length));
}

void TokioAsyncIoStream::getsockname(struct sockaddr *addr, kj::uint *length) {
  auto bytes = stream_local_addr(*inner);
  // Mirror the raw syscall's truncation semantics: copy what fits into the caller's buffer,
  // report the address's full length.
  memcpy(addr, bytes.data(), kj::min(bytes.size(), *length));
  *length = bytes.size();
}

void TokioAsyncIoStream::getpeername(struct sockaddr *addr, kj::uint *length) {
  auto bytes = stream_peer_addr(*inner);
  memcpy(addr, bytes.data(), kj::min(bytes.size(), *length));
  *length = bytes.size();
}

kj::Maybe<int> TokioAsyncIoStream::getFd() const {
#if _WIN32
  // On Windows the underlying handle is a winsock SOCKET, not a Unix fd; it is exposed via
  // getWin32Handle() below instead. (Validated by Windows CI.)
  return kj::none;
#else
  int64_t handle = -1;
  if (kj::runCatchingExceptions([&]() { handle = stream_raw_handle(*inner); }) == kj::none) {
    // On unix the raw socket handle is the fd, widened losslessly to int64 by the bridge.
    return static_cast<int>(handle);
  }
  return kj::none;
#endif
}

#if _WIN32
// Validated by Windows CI; mirrors the unix getFd() arm (and kj's own win32 AsyncStreamFd,
// which returns its SOCKET cast to void* -- capnproto async-io-win32.c++).
kj::Maybe<void *> TokioAsyncIoStream::getWin32Handle() const {
  int64_t handle = -1;
  if (kj::runCatchingExceptions([&]() { handle = stream_raw_handle(*inner); }) == kj::none) {
    return reinterpret_cast<void *>(static_cast<uintptr_t>(handle));
  }
  return kj::none;
}
#endif

::rust::Box<TokioStream> unwrapTokioStream(kj::AsyncIoStream &stream) {
  KJ_IF_SOME(tokioStream, kj::dynamicDowncastIfAvailable<TokioAsyncIoStream>(stream)) {
    return tokioStream.unwrap();
  }
  KJ_FAIL_REQUIRE("stream is not a kj-rs-io tokio-backed stream; cannot unwrap");
}

}  // namespace kj_rs_io
