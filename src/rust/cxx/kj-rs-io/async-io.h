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
//  - Operation-start policy: a bridged Rust future is cold until first polled, whereas KJ's
//    native streams issue the syscall inside the call (kj-http's header queue, for one, relies on
//    a write() it never awaits still going out). Every promise-returning method of these adapters
//    therefore starts its operation before returning -- coroutine bodies run to their first
//    co_await by construction, and non-coroutine methods wrap the bridged promise in started()
//    (async-io.c++: kj::Promise::eagerlyEvaluate). Cancellation is unchanged: dropping the
//    returned promise drops the Rust future, which releases the socket's readiness interest. A
//    stream with a canceled read remains usable.
//  - Ownership of in-flight operations: every bridged operation owns a share of the Rust object
//    behind the wrapper (Rc-shared state, see lib.rs), so destroying one of these C++ objects
//    while one of its promises is still pending is memory-safe -- the operation keeps the socket
//    alive until it settles or is cancelled. KJ's "no I/O promise may be outstanding on a
//    destroyed stream" remains the usage contract, not a memory-safety boundary.
//  - Unwrap fast path: every Rust-originated stream can be recovered as its native tokio object
//    (see unwrapTokioStream), so Rust servers can serve a connection natively
//    instead of crossing the FFI per read. Foreign kj streams are not unwrappable.
//  - Process initialization: like kj::UnixEventPort's constructor, setting up a context ignores
//    SIGPIPE (once per process), so writes to a closed peer surface as EPIPE -> DISCONNECTED
//    exceptions instead of terminating the process.
//
// Known stubs (all throw UNIMPLEMENTED, documented per method): newPipeThread(), capability
// streams (SCM_RIGHTS fd passing), datagram sockets, and named-service / abstract-unix-socket
// address forms. restrictPeers() IS implemented (PeerFilter, KJ's own kj::_::NetworkFilter with a
// refcounted chain); see TokioNetwork for the enforcement points.

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
  // EOF is reached first (in which case the short count signals EOF). `buffer` may be
  // uninitialized, as KJ allows; the Rust side treats it as such.
  kj::Promise<size_t> tryRead(void *buffer, size_t minBytes, size_t maxBytes) override;

  // AsyncOutputStream. Both overloads have write-all semantics; the multi-piece overload is one
  // bridged operation using vectored writes (writev), like KJ's own socket streams.
  kj::Promise<void> write(kj::ArrayPtr<const kj::byte> buffer) override;
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override;

  // Resolves when new writes are doomed (peer reset/hangup observed). Does not fire on a mere
  // half-close (peer FIN), mirroring KJ. On Windows the promise never resolves (KJ-on-Windows
  // behavior). Safe to call multiple times concurrently; the observation costs one dup'd fd per
  // stream, shared by every call.
  kj::Promise<void> whenWriteDisconnected() override;

  // AsyncIoStream. abortRead() is shutdown(SHUT_RD), as in KJ's AsyncStreamFd: a pending read
  // observes EOF (kj-http's WebSocket abort and CONNECT error paths rely on this).
  void shutdownWrite() override;
  void abortRead() override;
  void getsockopt(int level, int option, void *value, kj::uint *length) override;
  void setsockopt(int level, int option, const void *value, kj::uint length) override;
  void getsockname(struct sockaddr *addr, kj::uint *length) override;
  void getpeername(struct sockaddr *addr, kj::uint *length) override;
  kj::Maybe<int> getFd() const override;
#if _WIN32
  // On Windows the underlying socket is a winsock SOCKET, exposed as a void* handle (kj
  // convention; getFd() returns none there).
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

// A kj::ConnectionReceiver backed by one or more native tokio listeners (one per socket address
// the kj::NetworkAddress resolved to -- KJ's aggregate receiver). The accept loop -- dropping
// peers `filter` (restrictPeers) disallows, retrying KJ's transient per-connection failures,
// taking the peer address from accept(2) itself -- lives in Rust (net.rs listener_accept); each
// accept() hands it a share of the filter.
class TokioConnectionReceiver final: public kj::ConnectionReceiver {
 public:
  // Receiver for one of our own networks/addresses: shares ownership of the PeerFilter chain, so
  // there is no lifetime coupling to the network that created it.
  TokioConnectionReceiver(::rust::Box<TokioListener> inner, kj::Rc<PeerFilter> filter)
      : inner(kj::mv(inner)),
        shared(kj::mv(filter)) {}

  // Receiver over a caller-provided filter (the kj::LowLevelAsyncIoProvider::wrapListenSocketFd
  // entry point). KJ's interface hands the filter by reference and makes the caller responsible
  // for keeping it alive for the receiver's lifetime, exactly as with KJ's native providers; each
  // accept() hands Rust a kj::Own with kj::NullDisposer -- KJ's idiom for an explicitly
  // non-owning Own -- over it.
  TokioConnectionReceiver(
      ::rust::Box<TokioListener> inner, kj::LowLevelAsyncIoProvider::NetworkFilter &filter)
      : inner(kj::mv(inner)),
        borrowed(filter) {}

  kj::Promise<kj::Own<kj::AsyncIoStream>> accept() override;
  kj::Promise<kj::AuthenticatedStream> acceptAuthenticated() override;
  // getPort()/getsockopt()/getsockname() answer from the first socket and setsockopt() applies
  // to all of them, like KJ's aggregate receiver.
  kj::uint getPort() override;
  void getsockopt(int level, int option, void *value, kj::uint *length) override;
  void setsockopt(int level, int option, const void *value, kj::uint length) override;
  void getsockname(struct sockaddr *addr, kj::uint *length) override;

 private:
  kj::Promise<kj::AuthenticatedStream> acceptImpl(bool authenticated);

  // The filter share handed to each accept(): a refcount share of our own chain, or the
  // caller's filter behind a NullDisposer (see the constructors).
  kj::Own<kj::LowLevelAsyncIoProvider::NetworkFilter> filterShare();

  ::rust::Box<TokioListener> inner;
  // Exactly one of these is set (see the constructors).
  kj::Maybe<kj::Rc<PeerFilter>> shared;
  kj::Maybe<kj::LowLevelAsyncIoProvider::NetworkFilter &> borrowed;
};

// A kj::NetworkAddress holding pre-resolved socket addresses (DNS happens at parseAddress time,
// like KJ). connect() tries each address in order (net.rs address_connect; the future owns its
// own copy of the targets and a share of the filter, so the caller may drop this NetworkAddress
// while the connect is still pending -- KJ's NetworkAddressImpl::connect() contract); listen()
// binds all of them.
//
// `filter` is the restrictPeers filter chain of the kj::Network this address came from
// (allow-all for an unrestricted network); this address co-owns it, so it stays valid for this
// address and any promises it returns regardless of the network's lifetime. Filtering is
// enforced at parse time (disallowed literals are rejected and disallowed DNS results dropped,
// see TokioNetwork::parseAddress), at connect() time per address ("connect() blocked by
// restrictPeers()"), and at accept() time on listeners -- KJ's three enforcement points.
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
// restrictPeers() uses PeerFilter (KJ's own NetworkFilter policy behind a refcounted chain). The
// returned network owns a share of this one's filter chain (kj::Rc), so derived networks,
// addresses, and receivers all remain valid regardless of which order the networks are destroyed
// in.
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

// The tokio-backed kj::LowLevelAsyncIoProvider. Each wrap*Fd hands the raw handle -- a Unix fd
// or a win32 SOCKET, widened to int64 -- and KJ's TAKE_OWNERSHIP/ALREADY_CLOEXEC/ALREADY_NONBLOCK
// flags verbatim to Rust, which applies the flags (dup when not taking ownership, CLOEXEC,
// non-blocking) and converts the handle into a typed owner at the bridge entry point (ffi.rs
// prepare_fd). The pipe tier (wrapInputFd/wrapOutputFd) is Unix-only, as in kj's own win32
// provider.
// wrapUnixSocketFd (capability streams) and wrapDatagramSocketFd keep their default-throwing
// implementations.
class TokioLowLevelAsyncIoProvider final: public kj::LowLevelAsyncIoProvider {
 public:
  explicit TokioLowLevelAsyncIoProvider(kj::Timer &timer): timer(timer) {}

  kj::Own<kj::AsyncInputStream> wrapInputFd(Fd fd, kj::uint flags) override;
  kj::Own<kj::AsyncOutputStream> wrapOutputFd(Fd fd, kj::uint flags) override;
  kj::Own<kj::AsyncIoStream> wrapSocketFd(Fd fd, kj::uint flags) override;
  // AF_INET / AF_INET6 / (unix) AF_UNIX sockaddrs.
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
// for kj::setupAsyncIo(). Composes kj_rs_tokio::TokioAsyncIoContext (which owns the event port,
// the kj::EventLoop and the kj::WaitScope, and orders their teardown) with the tokio-backed I/O
// providers, held by value (the context is immovable anyway).
//
// Teardown is member order: the providers (which borrow the port's timer) go first, then the
// base context -- spawned tasks cancelled while the WaitScope is alive, then WaitScope, then the
// port (loop, runtime, timer). I/O objects created *through* the providers (streams, listeners,
// addresses) must be destroyed before the context, as with kj::setupAsyncIo().
struct TokioAsyncIoContext {
  // Also performs the once-per-process initialization KJ's UnixEventPort does (ignore SIGPIPE).
  TokioAsyncIoContext();
  KJ_DISALLOW_COPY_AND_MOVE(TokioAsyncIoContext);

  kj_rs_tokio::TokioAsyncIoContext base;
  TokioLowLevelAsyncIoProvider lowLevelProvider;
  TokioAsyncIoProvider provider;

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
    return provider.getNetwork();
  }
  kj::AsyncIoProvider &getProvider() {
    return provider;
  }
  kj::LowLevelAsyncIoProvider &getLowLevelProvider() {
    return lowLevelProvider;
  }
};

// Sets up the current thread with a tokio-driven KJ event loop plus tokio-backed I/O providers:
// the kj::setupAsyncIo() equivalent for the tokio loop. One per thread.
TokioAsyncIoContext setupTokioAsyncIo();

// Resolves when the process receives signal `signum`: the tokio-loop replacement for
// kj::UnixEventPort::onSignal() (workerd's SIGTERM graceful drain). Must be awaited on the
// thread owning the TokioEventPort. The handler is installed before this returns (the promise
// is started eagerly, per the operation-start policy above); unlike UnixEventPort, KJ does not
// block/capture the signal beforehand, so a signal delivered before the *call* takes its default
// disposition (see signal.rs). On Windows, SIGTERM/SIGINT are mapped to the
// ctrl_shutdown/ctrl_c console control events; the promise rejects for other signums.
kj::Promise<void> onSignal(int signum);

}  // namespace kj_rs_io
