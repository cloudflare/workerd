#pragma once
// Shared helpers for kj-hyper's C++ tests (serve-test): connected stream pairs over the
// tokio-backed kj-rs-io network, patterned payloads, and chunked write / verify pumps. A copy of
// kj-rs-io/tests/io-test-helpers.h scoped to this crate's test namespace.

#include "kj-rs-io/async-io.h"

#include <kj/array.h>
#include <kj/async-io.h>
#include <kj/async.h>
#include <kj/debug.h>

#include <cstring>

#if _WIN32
#include <winsock2.h>

#include <kj/windows-sanity.h>
#else
#include <sys/socket.h>
#include <unistd.h>  // getpid()/unlink() for the unix-socket pair helper
#endif

namespace kj_hyper_test {

struct ConnectedPair {
  kj::Own<kj::ConnectionReceiver> listener;
  kj::Own<kj::AsyncIoStream> client;
  kj::Own<kj::AsyncIoStream> server;
};

inline kj::Own<kj::NetworkAddress> parseNow(
    kj_rs_io::TokioAsyncIoContext &io, kj::StringPtr addr, kj::uint portHint = 0) {
  return io.getNetwork().parseAddress(addr, portHint).wait(io.getWaitScope());
}

// A connected loopback TCP pair from the kj-rs-io network.
inline ConnectedPair makeTcpPair(kj_rs_io::TokioAsyncIoContext &io) {
  auto &ws = io.getWaitScope();
  auto listener = parseNow(io, "127.0.0.1")->listen();
  auto connectAddr = parseNow(io, kj::str("127.0.0.1:", listener->getPort()));
  auto acceptPromise = listener->accept();
  auto client = connectAddr->connect().wait(ws);
  auto server = acceptPromise.wait(ws);
  return ConnectedPair{kj::mv(listener), kj::mv(client), kj::mv(server)};
}

#if !_WIN32
// A short /tmp unix-socket path unique per process + call, so parallel/repeated runs never
// collide. Stale files are unlinked first.
inline kj::String freshUnixSocketPath(kj::StringPtr tag) {
  static uint counter = 0;
  auto path = kj::str("/tmp/kj-hyper-", tag, "-", ::getpid(), "-", counter++, ".sock");
  ::unlink(path.cStr());
  return path;
}

// A connected AF_UNIX stream-socket pair from the kj-rs-io network's `unix:` support. The bound
// path is unlinked once both ends are connected.
inline ConnectedPair makeUnixPair(kj_rs_io::TokioAsyncIoContext &io) {
  auto &ws = io.getWaitScope();
  auto path = freshUnixSocketPath("pair");
  auto addr = kj::str("unix:", path);
  auto listener = parseNow(io, addr)->listen();
  auto connectAddr = parseNow(io, addr);
  auto acceptPromise = listener->accept();
  auto client = connectAddr->connect().wait(ws);
  auto server = acceptPromise.wait(ws);
  ::unlink(path.cStr());
  return ConnectedPair{kj::mv(listener), kj::mv(client), kj::mv(server)};
}
#endif  // !_WIN32

// The stream's raw OS socket handle (kj::AsyncIoStream::getFd() / getWin32Handle()).
#if _WIN32
using RawSocket = SOCKET;
inline RawSocket rawSocketOf(kj::AsyncIoStream &stream) {
  return reinterpret_cast<SOCKET>(KJ_ASSERT_NONNULL(stream.getWin32Handle()));
}
#else
using RawSocket = int;
inline RawSocket rawSocketOf(kj::AsyncIoStream &stream) {
  return KJ_ASSERT_NONNULL(stream.getFd());
}
#endif

// Raw setsockopt(2)/getsockopt(2) on a stream's socket. kj-rs-io keeps kj::AsyncIoStream's
// default (UNIMPLEMENTED) get/setsockopt, so the tests that need a socket option go to the OS.
inline void setRawSockopt(
    kj::AsyncIoStream &stream, int level, int option, const void *value, size_t length) {
#if _WIN32
  KJ_REQUIRE(::setsockopt(rawSocketOf(stream), level, option, reinterpret_cast<const char *>(value),
                 static_cast<int>(length)) == 0,
      WSAGetLastError());
#else
  KJ_SYSCALL(setsockopt(rawSocketOf(stream), level, option, value, length));
#endif
}

inline int getRawSockoptInt(kj::AsyncIoStream &stream, int level, int option) {
  int value = 0;
#if _WIN32
  int length = sizeof(value);
  KJ_REQUIRE(::getsockopt(rawSocketOf(stream), level, option, reinterpret_cast<char *>(&value),
                 &length) == 0,
      WSAGetLastError());
#else
  socklen_t length = sizeof(value);
  KJ_SYSCALL(getsockopt(rawSocketOf(stream), level, option, &value, &length));
#endif
  return value;
}

inline kj::Array<kj::byte> makePatternedData(size_t size, kj::byte seed) {
  auto data = kj::heapArray<kj::byte>(size);
  for (size_t i = 0; i < size; i++) {
    data[i] = static_cast<kj::byte>((i * 31 + seed) & 0xff);
  }
  return data;
}

// Writes `data` to `out` in 64 KiB chunks (exercising write-all + backpressure).
inline kj::Promise<void> writeChunked(
    kj::AsyncOutputStream &out, kj::ArrayPtr<const kj::byte> data) {
  constexpr size_t CHUNK = 64 * 1024;
  size_t offset = 0;
  while (offset < data.size()) {
    size_t n = kj::min(CHUNK, data.size() - offset);
    co_await out.write(data.slice(offset, offset + n));
    offset += n;
  }
}

// Reads exactly `expected.size()` bytes from `in` and verifies they match `expected`.
inline kj::Promise<void> readExact(
    kj::AsyncInputStream &in, kj::ArrayPtr<const kj::byte> expected) {
  auto buffer = kj::heapArray<kj::byte>(expected.size());
  size_t total = co_await in.tryRead(buffer.begin(), buffer.size(), buffer.size());
  KJ_ASSERT(total == expected.size(), total, expected.size());
  KJ_ASSERT(memcmp(buffer.begin(), expected.begin(), expected.size()) == 0);
}

// `promise`, failing loudly (rather than hanging until bazel's timeout) if it is still pending
// after `timeout` on the context's KJ timer.
template <typename T>
kj::Promise<T> boundedBy(kj_rs_io::TokioAsyncIoContext &io,
    kj::Promise<T> promise,
    kj::Duration timeout,
    kj::StringPtr what) {
  return promise.exclusiveJoin(io.getTimer().afterDelay(timeout).then(
      [what]() -> T { KJ_FAIL_ASSERT("timed out waiting for", what); }));
}

}  // namespace kj_hyper_test
