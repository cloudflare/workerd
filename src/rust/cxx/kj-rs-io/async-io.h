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
#include <kj/exception.h>
#include <kj/timer.h>

namespace kj_rs_io {

// A kj::AsyncIoStream backed by a native tokio TcpStream or UnixStream.
class TokioAsyncIoStream final: public kj::AsyncIoStream {
 public:
  explicit TokioAsyncIoStream(::rust::Box<TokioStream> inner): inner(kj::mv(inner)) {}

  // AsyncInputStream. tryRead honors KJ's min-bytes contract: resolves with >= minBytes unless
  // EOF is reached first (in which case the short count signals EOF).
  kj::Promise<size_t> tryRead(void *buffer, size_t minBytes, size_t maxBytes) override;

  // AsyncOutputStream. Both overloads have write-all semantics; the multi-piece overload is one
  // bridged operation using vectored writes (writev), like KJ's own socket streams.
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
  // further operations throw). Throws if I/O promises are in flight -- the Rust side tracks
  // in-flight operations, so this is checked rather than a caller contract. Prefer the free
  // function unwrapTokioStream() when holding only a kj::AsyncIoStream&.
  ::rust::Box<TokioStream> unwrap() {
    return stream_take(*inner);
  }

 private:
  kj::Promise<void> writePieces(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces);

  ::rust::Box<TokioStream> inner;
};

// A kj::ConnectionReceiver backed by a native tokio TcpListener or UnixListener. Incoming
// connections from peers disallowed by `filter` (restrictPeers) are silently dropped and the
// accept loop continues, mirroring KJ.
class TokioConnectionReceiver final: public kj::ConnectionReceiver {
 public:
  // Receiver for one of our own networks/addresses: shares ownership of the PeerFilter chain, so
  // there is no lifetime coupling to the network that created it.
  TokioConnectionReceiver(::rust::Box<TokioListener> inner, kj::Rc<PeerFilter> filterParam)
      : inner(kj::mv(inner)),
        ownFilter(kj::mv(filterParam)),
        filter(*KJ_ASSERT_NONNULL(ownFilter)) {}

  // Receiver over a caller-provided filter (the kj::LowLevelAsyncIoProvider::wrapListenSocketFd
  // entry point). The bare reference is that KJ interface's own contract: the caller owns the
  // filter and must keep it alive for the receiver's lifetime, exactly as with KJ's native
  // providers.
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
  // Owned strong reference when constructed from one of our networks; kj::none on the
  // wrapListenSocketFd path, where `filter` refers to the caller-owned filter instead (KJ
  // interface contract; see the constructors). Declared before `filter`, which may alias it.
  kj::Maybe<kj::Rc<PeerFilter>> ownFilter;
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
// `filter` is the restrictPeers filter chain of the kj::Network this address came from
// (allow-all for an unrestricted network); this address co-owns it, so it stays valid for this
// address and any promises it returns regardless of the network's lifetime. Filtering is
// enforced at connect() time per address ("connect() blocked by restrictPeers()", KJ parity)
// and at accept() time on listeners; unlike KJ, disallowed DNS results are not already dropped
// at parse time (they fail at connect instead).
class TokioNetworkAddress final: public kj::NetworkAddress {
 public:
  TokioNetworkAddress(::rust::Box<TokioAddress> inner, kj::Rc<PeerFilter> filter)
      : inner(kj::mv(inner)),
        filter(kj::mv(filter)) {}

  kj::Promise<kj::Own<kj::AsyncIoStream>> connect() override;
  kj::Own<kj::ConnectionReceiver> listen() override;
  kj::Own<kj::NetworkAddress> clone() override;
  kj::String toString() override;

 private:
  ::rust::Box<TokioAddress> inner;
  kj::Rc<PeerFilter> filter;
};

// The tokio-backed kj::Network. Supports the KJ address grammar subset workerd uses; see
// net.rs for the exact forms and documented deviations (no named services, no unix-abstract,
// no IPv6 scope IDs).
//
// restrictPeers() uses PeerFilter, a faithful port of KJ's NetworkFilter (semantics identical to
// kj::setupAsyncIo()'s networks). The returned network owns a share of this one's filter chain
// (kj::Rc), so derived networks, addresses, and receivers all remain valid regardless of which
// order the networks are destroyed in. Enforcement points: per-address connect()-time checks and
// accept()-time peer checks; parse-time DNS-result dropping is NOT implemented (blocked
// addresses fail at connect instead) -- see TokioNetworkAddress.
class TokioNetwork final: public kj::Network {
 public:
  // Allow-everything root network (matches KJ's root networks).
  TokioNetwork(): filter(kj::rc<PeerFilter>()) {}
  TokioNetwork(TokioNetwork &parent,
      kj::ArrayPtr<const kj::StringPtr> allow,
      kj::ArrayPtr<const kj::StringPtr> deny)
      : filter(kj::rc<PeerFilter>(allow, deny, parent.filter.addRef())) {}

  kj::Promise<kj::Own<kj::NetworkAddress>> parseAddress(
      kj::StringPtr addr, kj::uint portHint) override;
  kj::Own<kj::NetworkAddress> getSockaddr(const void *sockaddr, kj::uint len) override;
  kj::Own<kj::Network> restrictPeers(
      kj::ArrayPtr<const kj::StringPtr> allow, kj::ArrayPtr<const kj::StringPtr> deny) override;

 private:
  kj::Rc<PeerFilter> filter;
};

// The tokio-backed kj::LowLevelAsyncIoProvider. Implements the socket-wrapping entry points on
// Unix and Windows (each wrap*Fd normalizes KJ's TAKE_OWNERSHIP/ALREADY_CLOEXEC/ALREADY_NONBLOCK
// flags, then hands an owned, non-blocking raw socket handle -- a Unix fd or a win32 SOCKET,
// widened to int64 -- to Rust). The pipe tier (wrapInputFd/wrapOutputFd) is Unix-only for now.
// wrapUnixSocketFd (capability streams) and wrapDatagramSocketFd keep their default-throwing
// implementations.
class TokioLowLevelAsyncIoProvider final: public kj::LowLevelAsyncIoProvider {
 public:
  explicit TokioLowLevelAsyncIoProvider(kj::Timer &timer): timer(timer) {}

  kj::Own<kj::AsyncInputStream> wrapInputFd(Fd fd, kj::uint flags) override;
  kj::Own<kj::AsyncOutputStream> wrapOutputFd(Fd fd, kj::uint flags) override;
  kj::Own<kj::AsyncIoStream> wrapSocketFd(Fd fd, kj::uint flags) override;
  kj::Promise<kj::Own<kj::AsyncIoStream>> wrapConnectingSocketFd(
      Fd fd, const struct sockaddr *addr, kj::uint addrlen, kj::uint flags) override;
  // `filter` applies to accepted connections (disallowed peers are dropped and the accept
  // loop continues, like KJ); it must outlive the returned receiver.
  kj::Own<kj::ConnectionReceiver> wrapListenSocketFd(
      Fd fd, NetworkFilter &filter, kj::uint flags) override;
  kj::Timer &getTimer() override {
    return timer;
  }

 private:
  kj::Timer &timer;
};

// The tokio-backed kj::AsyncIoProvider. Pipes are KJ's in-memory pipes (port-agnostic, like
// kj::newOneWayPipe/newTwoWayPipe themselves); newPipeThread throws UNIMPLEMENTED (workerd does
// not use it); newCapabilityPipe keeps its default-throwing implementation.
class TokioAsyncIoProvider final: public kj::AsyncIoProvider {
 public:
  explicit TokioAsyncIoProvider(kj::Timer &timer): timer(timer) {}

  kj::OneWayPipe newOneWayPipe() override {
    return kj::newOneWayPipe();
  }
  kj::TwoWayPipe newTwoWayPipe() override {
    return kj::newTwoWayPipe();
  }
  kj::Network &getNetwork() override {
    return network;
  }
  PipeThread newPipeThread(
      kj::Function<void(kj::AsyncIoProvider &, kj::AsyncIoStream &, kj::WaitScope &)> startFunc)
      override;
  kj::Timer &getTimer() override {
    return timer;
  }

 private:
  TokioNetwork network;
  kj::Timer &timer;
};

// Mirror of kj::AsyncIoContext (kj/async-io.h) for the tokio-backed loop: a drop-in replacement
// for kj::setupAsyncIo() at workerd.c++:1570. Composes kj_rs_tokio::TokioAsyncIoContext (which
// owns the event port, the kj::EventLoop and the kj::WaitScope, and orders their teardown) with
// the tokio-backed I/O providers.
//
// Teardown is member order: the providers (which borrow the port's timer) go first, then the
// base context -- spawned tasks cancelled while the WaitScope is alive, then WaitScope, then the
// port (loop, runtime, timer). I/O objects created *through* the providers (streams, listeners,
// addresses) must be destroyed before the context, as with kj::setupAsyncIo().
struct TokioAsyncIoContext {
  TokioAsyncIoContext(kj_rs_tokio::TokioAsyncIoContext base,
      kj::Own<TokioLowLevelAsyncIoProvider> lowLevelProvider,
      kj::Own<TokioAsyncIoProvider> provider)
      : base(kj::mv(base)),
        lowLevelProvider(kj::mv(lowLevelProvider)),
        provider(kj::mv(provider)) {}
  // Same move rules as the base context (move-constructible for return-by-value; no
  // move-assignment, which would bypass the base's teardown ordering).
  TokioAsyncIoContext(TokioAsyncIoContext &&) = default;
  TokioAsyncIoContext &operator=(TokioAsyncIoContext &&) = delete;
  KJ_DISALLOW_COPY(TokioAsyncIoContext);

  kj_rs_tokio::TokioAsyncIoContext base;
  kj::Own<TokioLowLevelAsyncIoProvider> lowLevelProvider;
  kj::Own<TokioAsyncIoProvider> provider;

  kj_rs_tokio::TokioEventPort &getPort() {
    return base.getPort();
  }
  kj::EventLoop &getLoop() {
    return base.getLoop();
  }
  kj::WaitScope &getWaitScope() {
    return base.getWaitScope();
  }
  kj::Timer &getTimer() {
    return base.getTimer();
  }
  kj::Network &getNetwork() {
    return provider->getNetwork();
  }
  kj::AsyncIoProvider &getProvider() {
    return *provider;
  }
  kj::LowLevelAsyncIoProvider &getLowLevelProvider() {
    return *lowLevelProvider;
  }
};

// Sets up the current thread with a tokio-driven KJ event loop plus tokio-backed I/O providers:
// the kj::setupAsyncIo() equivalent for the tokio loop. One per thread.
TokioAsyncIoContext setupTokioAsyncIo();

// Resolves when the process receives signal `signum`: the tokio-loop replacement for
// kj::UnixEventPort::onSignal() (workerd's SIGTERM graceful drain). Must be awaited on the
// thread owning the TokioEventPort. Unlike UnixEventPort, KJ does not block/capture the signal
// beforehand: the tokio handler is registered when the promise is first polled, so a signal
// delivered before the event loop first runs takes its default disposition (see signal.rs).
// On Windows, SIGTERM/SIGINT are mapped to the ctrl_shutdown/ctrl_c console control events;
// the promise rejects for other signums.
kj::Promise<void> onSignal(int signum);

}  // namespace kj_rs_io
