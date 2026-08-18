#pragma once
// kj-rs-io: tokio-backed implementations of KJ's async I/O interfaces.
//
// Everything here wraps an opaque Rust object (a native tokio TcpStream/UnixStream/TcpListener/
// address list) and implements the corresponding KJ interface by calling `async fn`s across the
// cxx bridge, which return kj::Promises. Design points:
//
//  - All promises must be awaited on the thread owning the kj_rs_tokio::TokioEventPort: the
//    tokio I/O driver that delivers readiness for these sockets only runs while that KJ loop
//    sleeps in the port's wait()/poll().
//  - Cancellation: dropping a returned kj::Promise drops the underlying Rust future, which
//    releases the socket's readiness interest. A stream with a canceled read remains usable.
//  - Unwrap fast path: every Rust-originated stream can be recovered as its native tokio object
//    (see unwrapTokioStream), so Rust servers can serve a connection natively
//    instead of crossing the FFI per read. Foreign kj streams are not unwrappable.
//
// Known stubs (all throw UNIMPLEMENTED, documented per method): newPipeThread(), capability
// streams (SCM_RIGHTS fd passing), datagram sockets, and named-service / abstract-unix-socket
// address forms. restrictPeers() IS implemented (via PeerFilter, a port of KJ's NetworkFilter); see
// TokioNetwork for enforcement points and the parse-time-filtering deviation.

#include "kj-rs-io/ffi.rs.h"
#include "kj-rs-io/peer-filter.h"
#include "kj-rs-tokio/tokio-event-port.h"

#include <kj/async-io.h>
#include <kj/timer.h>

namespace kj_rs_io {

// A kj::AsyncIoStream backed by a native tokio TcpStream or UnixStream.
class TokioAsyncIoStream final: public kj::AsyncIoStream {
 public:
  explicit TokioAsyncIoStream(::rust::Box<TokioStream> inner): inner(kj::mv(inner)) {}

  // AsyncInputStream. tryRead honors KJ's min-bytes contract: resolves with >= minBytes unless
  // EOF is reached first (in which case the short count signals EOF).
  kj::Promise<size_t> tryRead(void *buffer, size_t minBytes, size_t maxBytes) override;

  // AsyncOutputStream. write() has write-all semantics; the multi-piece overload writes the
  // pieces sequentially (no vectored-write optimization yet).
  kj::Promise<void> write(kj::ArrayPtr<const kj::byte> buffer) override;
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override;

  // Resolves when new writes are doomed (peer reset/hangup observed). Does not fire on a mere
  // half-close (peer FIN), mirroring KJ. On non-Unix platforms the promise never resolves
  // (KJ-on-Windows behavior). Safe to call multiple times concurrently.
  kj::Promise<void> whenWriteDisconnected() override;

  // AsyncIoStream.
  void shutdownWrite() override;
  void getsockopt(int level, int option, void *value, kj::uint *length) override;
  void setsockopt(int level, int option, const void *value, kj::uint length) override;
  void getsockname(struct sockaddr *addr, kj::uint *length) override;
  void getpeername(struct sockaddr *addr, kj::uint *length) override;
  kj::Maybe<int> getFd() const override;
#if _WIN32
  // Validated by Windows CI; mirrors getFd(): on Windows the underlying socket is a winsock
  // SOCKET, exposed as a void* handle (kj convention; getFd() returns none there).
  kj::Maybe<void *> getWin32Handle() const override;
#endif

  // Unwrap fast path: moves the native tokio stream out, leaving this wrapper hollow (all
  // further operations throw). No I/O promises may be in flight. Prefer the free function
  // unwrapTokioStream() when holding only a kj::AsyncIoStream&.
  ::rust::Box<TokioStream> unwrap() {
    return stream_take(*inner);
  }

 private:
  kj::Promise<void> writePieces(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces);

  ::rust::Box<TokioStream> inner;
};

// A kj::ConnectionReceiver backed by a native tokio TcpListener or UnixListener. Incoming
// connections from peers disallowed by `filter` (restrictPeers) are silently dropped and the
// accept loop continues, mirroring KJ. `filter` must outlive this receiver.
class TokioConnectionReceiver final: public kj::ConnectionReceiver {
 public:
  TokioConnectionReceiver(
      ::rust::Box<TokioListener> inner, kj::LowLevelAsyncIoProvider::NetworkFilter &filter)
      : inner(kj::mv(inner)),
        filter(filter) {}

  kj::Promise<kj::Own<kj::AsyncIoStream>> accept() override;
  kj::Promise<kj::AuthenticatedStream> acceptAuthenticated() override;
  kj::uint getPort() override;
  void getsockopt(int level, int option, void *value, kj::uint *length) override;
  void setsockopt(int level, int option, const void *value, kj::uint length) override;
  void getsockname(struct sockaddr *addr, kj::uint *length) override;

 private:
  kj::Promise<kj::AuthenticatedStream> acceptImpl(bool authenticated);

  ::rust::Box<TokioListener> inner;
  kj::LowLevelAsyncIoProvider::NetworkFilter &filter;
};

// A kj::NetworkAddress holding pre-resolved socket addresses (DNS happens at parseAddress time,
// like KJ). connect() tries each address in order; listen() binds the first.
//
// connect() honors KJ's lifetime contract (NetworkAddressImpl::connect() in
// kj/async-io-unix.c++): the returned promise is a coroutine whose frame owns a copy of the
// resolved address list, so the caller may drop this NetworkAddress while the connect is still
// pending.
//
// `filter` is the restrictPeers filter of the kj::Network this address came from (allow-all
// for an unrestricted network) and must outlive this address and any promises it returns
// (in KJ the filter equally lives in the long-lived provider, so this matches upstream).
// Filtering is enforced at connect() time per address ("connect() blocked by
// restrictPeers()", KJ parity) and at accept() time on listeners; unlike KJ, disallowed DNS
// results are not already dropped at parse time (they fail at connect instead).
class TokioNetworkAddress final: public kj::NetworkAddress {
 public:
  TokioNetworkAddress(::rust::Box<TokioAddress> inner, PeerFilter &filter)
      : inner(kj::mv(inner)),
        filter(filter) {}

  kj::Promise<kj::Own<kj::AsyncIoStream>> connect() override;
  kj::Own<kj::ConnectionReceiver> listen() override;
  kj::Own<kj::NetworkAddress> clone() override;
  kj::String toString() override;

 private:
  ::rust::Box<TokioAddress> inner;
  PeerFilter &filter;
};

}  // namespace kj_rs_io
