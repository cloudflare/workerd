#pragma once
// kj-rs-io: tokio-backed implementations of KJ's async I/O interfaces.
//
// Each class here implements one KJ interface over an opaque Rust object (a tokio
// TcpStream/UnixStream, a listener set, an address list) by calling the bridged operations in
// ffi.rs, which return kj::Promises. What the adapters add, and where the rules live:
//
//  - Operation-start policy. A bridged Rust future is cold until first polled, whereas KJ's
//    native streams start their operation inside the call. Every promise-returning method here
//    therefore starts its operation before returning: coroutine bodies run to their first
//    co_await by construction, and non-coroutine methods wrap the bridged promise in started()
//    (async-io.c++: kj::Promise::eagerlyEvaluate, whose first poll is synchronous). A promise
//    that is kept but never awaited still completes as the loop turns. When the syscall itself
//    happens is tokio's decision, not KJ's (stream.rs, "When the syscall happens"). Dropping a
//    promise drops the Rust future, which releases the socket's readiness interest; a stream with
//    a cancelled read remains usable.
//  - Peer filtering (restrictPeers) is applied here, where KJ applies it: to each target before
//    connect() tries it, and to each accepted peer (PeerFilter, KJ's own policy). Addresses
//    cross the bridge typed (SocketAddress, ffi.rs); this file is the only place a raw
//    `struct sockaddr` is decoded (getSockaddr) or encoded (getsockname/getpeername, the filter).
//  - Ownership and threads are the Rust side's (lib.rs): an in-flight operation owns a share of
//    its socket, so a wrapper destroyed with a promise pending does not dangle -- except for the
//    caller's buffer, which a pending tryRead() still writes into, exactly as under KJ. Every
//    bridged operation checks it runs on the loop thread (lib.rs, "The tokio runtime").
//  - Process initialization: like kj::UnixEventPort's constructor, setting up a context ignores
//    SIGPIPE (once per process), so writes to a closed peer surface as EPIPE -> DISCONNECTED
//    exceptions instead of terminating the process.
//
// Scope: this is workerd's provider, not a drop-in for every KJ program (lib.rs, "Scope:
// workerd's provider"). Throws UNIMPLEMENTED: newPipeThread(), wrapConnectingSocketFd(),
// wrapListenSocketFd() with a caller-owned filter, capability streams (SCM_RIGHTS fd passing),
// the raw getsockopt()/setsockopt() passthroughs. wrap*Fd take sockets
// only, and the provider's pipes are socket pairs.

#include "kj-rs-io/ffi.rs.h"
#include "kj-rs-io/peer-filter.h"
#include "kj-rs-tokio/tokio-event-port.h"

#include <kj/async-io.h>
#include <kj/exception.h>
#include <kj/filesystem.h>
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

  // Resolves when new writes are doomed (peer reset/hangup observed), not on a mere half-close
  // (peer FIN), mirroring KJ. Never resolves on Windows (KJ-on-Windows behavior). Costs one
  // dup'd descriptor per stream, created on first use and shared by every call (stream.rs,
  // "whenWriteDisconnected costs a descriptor").
  kj::Promise<void> whenWriteDisconnected() override;

  // AsyncIoStream. abortRead() ends a pending read with EOF and makes later reads EOF (kj-http's
  // WebSocket abort and CONNECT error paths rely on this), on every platform: the Rust stream
  // records the abort and wakes the parked read itself, and also performs KJ's shutdown(SHUT_RD)
  // for the peer-facing effect (stream.rs explains why the shutdown alone is not enough under a
  // readiness poller).
  // getsockopt()/setsockopt() keep kj::AsyncIoStream's defaults (UNIMPLEMENTED): no workerd code
  // calls them; TCP_NODELAY is applied by the connect and accept paths themselves (KJ parity).
  void shutdownWrite() override;
  void abortRead() override;
  void getsockname(struct sockaddr *addr, kj::uint *length) override;
  void getpeername(struct sockaddr *addr, kj::uint *length) override;
  kj::Maybe<int> getFd() const override;
#if _WIN32
  // The underlying winsock SOCKET as a void* handle (kj convention; getFd() returns none).
  kj::Maybe<void *> getWin32Handle() const override;
#endif

 private:
  kj::Promise<void> writePieces(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces);

  ::rust::Box<TokioStream> inner;
};

// A kj::ConnectionReceiver backed by one or more native tokio listeners (one per socket address
// the kj::NetworkAddress resolved to -- KJ's aggregate receiver). Rust accepts (retrying KJ's
// transient per-connection failures); this class drops the peers `filter` disallows and keeps
// listening, like KJ's acceptImpl. The receiver co-owns the filter chain, so there is no lifetime
// coupling to the network that created it.
class TokioConnectionReceiver final: public kj::ConnectionReceiver {
 public:
  TokioConnectionReceiver(::rust::Box<TokioListener> inner, kj::Arc<PeerFilter> filter)
      : inner(kj::mv(inner)),
        filter(kj::mv(filter)) {}

  kj::Promise<kj::Own<kj::AsyncIoStream>> accept() override;
  kj::Promise<kj::AuthenticatedStream> acceptAuthenticated() override;

  // getPort()/getsockname() answer from the first socket, like KJ's aggregate receiver;
  // getsockopt()/setsockopt() keep kj::ConnectionReceiver's defaults (UNIMPLEMENTED).
  kj::uint getPort() override;
  void getsockname(struct sockaddr *addr, kj::uint *length) override;

 private:
  kj::Promise<kj::AuthenticatedStream> acceptImpl(bool authenticated);

  ::rust::Box<TokioListener> inner;
  kj::Arc<PeerFilter> filter;
};

// A kj::NetworkAddress holding pre-resolved socket addresses (DNS happens at parseAddress time,
// like KJ). connect() tries each address in order, skipping the ones `filter` disallows; the
// connect promise owns its own copy of the targets and a share of the filter, so the caller may
// drop this NetworkAddress while the connect is still pending (KJ's contract).
// connectAuthenticated() reports the address that connected as the peer's identity, as KJ does
// (a NetworkPeerIdentity for TCP, the peer's credentials for unix sockets); listen() binds all of
// them.
class TokioNetworkAddress final: public kj::NetworkAddress {
 public:
  TokioNetworkAddress(::rust::Box<TokioAddress> inner, kj::Arc<PeerFilter> filter)
      : inner(kj::mv(inner)),
        filter(kj::mv(filter)) {}

  kj::Promise<kj::Own<kj::AsyncIoStream>> connect() override;
  kj::Promise<kj::AuthenticatedStream> connectAuthenticated() override;
  kj::Own<kj::ConnectionReceiver> listen() override;
  kj::Own<kj::DatagramPort> bindDatagramPort() override;
  kj::Own<kj::NetworkAddress> clone() override;
  kj::String toString() override;

  const TokioAddress &getInner() const {
    return *inner;
  }

 private:
  ::rust::Box<TokioAddress> inner;
  kj::Arc<PeerFilter> filter;
};

class TokioDatagramPort final: public kj::DatagramPort {
 public:
  TokioDatagramPort(::rust::Box<TokioDatagram> inner, kj::Arc<PeerFilter> filter)
      : inner(kj::mv(inner)),
        filter(kj::mv(filter)) {}

  kj::Promise<size_t> send(
      kj::ArrayPtr<const kj::byte> buffer, kj::NetworkAddress &destination) override;
  kj::Promise<size_t> send(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces,
      kj::NetworkAddress &destination) override;
  kj::Own<kj::DatagramReceiver> makeReceiver(kj::DatagramReceiver::Capacity capacity) override;
  kj::uint getPort() override;

 private:
  class Receiver;

  ::rust::Box<TokioDatagram> inner;
  kj::Arc<PeerFilter> filter;
};

// The tokio-backed kj::Network. The address grammar is KJ's SocketAddress::parse for everything
// workerd's configs use (net.rs, "Address grammar"). restrictPeers() returns a network sharing
// this one's filter chain and loopback namespace, so derived networks, addresses and receivers
// remain valid regardless of the order the networks are destroyed in.
class TokioNetwork final: public kj::Network {
 public:
  // Allow-everything root network (matches KJ's root networks).
  TokioNetwork(): filter(kj::arc<PeerFilter>()), loopback(new_loopback_registry()) {}
  TokioNetwork(TokioNetwork &parent,
      kj::ArrayPtr<const kj::StringPtr> allow,
      kj::ArrayPtr<const kj::StringPtr> deny)
      : filter(kj::arc<PeerFilter>(allow, deny, parent.filter.addRef())),
        loopback(loopback_registry_clone(*parent.loopback)) {}

  // Makes parseAddress() accept "loopback:<name>" addresses -- connections serviced within this
  // process (loopback.rs) -- on this network and every network derived from it by
  // restrictPeers(). For `workerd test`, which uses them to exercise the network stack end to
  // end without an external socket; production configs use direct service bindings instead.
  void enableLoopback() {
    loopback_registry_enable(*loopback);
  }

  kj::Promise<kj::Own<kj::NetworkAddress>> parseAddress(
      kj::StringPtr addr, kj::uint portHint) override;
  kj::Own<kj::NetworkAddress> getSockaddr(const void *sockaddr, kj::uint len) override;
  kj::Own<kj::Network> restrictPeers(
      kj::ArrayPtr<const kj::StringPtr> allow, kj::ArrayPtr<const kj::StringPtr> deny) override;

 private:
  kj::Arc<PeerFilter> filter;
  ::rust::Box<LoopbackRegistry> loopback;
};

// The tokio-backed kj::LowLevelAsyncIoProvider. Each wrap*Fd hands the raw handle -- a Unix fd
// or a win32 SOCKET, widened to int64 -- and KJ's TAKE_OWNERSHIP/ALREADY_CLOEXEC/ALREADY_NONBLOCK
// flags verbatim to Rust, which applies the flags (dup when not taking ownership, CLOEXEC,
// non-blocking) and converts the handle into a typed owner at the bridge entry point (ffi.rs).
// Every wrap*Fd takes socket handles only, as kj's win32 provider defines them (lib.rs, "Scope");
// wrapConnectingSocketFd throws UNIMPLEMENTED; wrapUnixSocketFd (capability streams) and
// wrapDatagramSocketFd keep their default-throwing implementations.
class TokioLowLevelAsyncIoProvider final: public kj::LowLevelAsyncIoProvider {
 public:
  explicit TokioLowLevelAsyncIoProvider(kj::Timer &timer): timer(timer) {}

  kj::Own<kj::AsyncInputStream> wrapInputFd(Fd fd, kj::uint flags) override;
  kj::Own<kj::AsyncOutputStream> wrapOutputFd(Fd fd, kj::uint flags) override;
  kj::Own<kj::AsyncIoStream> wrapSocketFd(Fd fd, kj::uint flags) override;
  kj::Promise<kj::Own<kj::AsyncIoStream>> wrapConnectingSocketFd(
      Fd fd, const struct sockaddr *addr, kj::uint addrlen, kj::uint flags) override;
  // Only the allow-all filter KJ's two-argument overload passes is accepted (workerd's one
  // call, for inherited listen sockets); a caller-owned filter throws UNIMPLEMENTED rather than
  // being borrowed for the receiver's lifetime.
  kj::Own<kj::ConnectionReceiver> wrapListenSocketFd(
      Fd fd, NetworkFilter &filter, kj::uint flags) override;
  kj::Timer &getTimer() override {
    return timer;
  }

 private:
  kj::Timer &timer;
};

// The tokio-backed kj::AsyncIoProvider. Its pipes are socket pairs, not in-memory kj pipes: a
// write into an empty pipe completes without a reader waiting, and workerd's loopback transport
// (server/workerd.c++) gets the real sockets it asks the provider for. newPipeThread throws
// UNIMPLEMENTED (workerd does not use it); newCapabilityPipe keeps its default-throwing
// implementation.
class TokioAsyncIoProvider final: public kj::AsyncIoProvider {
 public:
  explicit TokioAsyncIoProvider(kj::Timer &timer): timer(timer) {}

  kj::OneWayPipe newOneWayPipe() override;
  kj::TwoWayPipe newTwoWayPipe() override;
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
// disposition (see signal.rs). On Windows, SIGTERM/SIGINT are mapped to the ctrl_break/ctrl_c
// console control events; the promise rejects for other signums.
kj::Promise<void> onSignal(int signum);

// Watches files for changes: kj-rs-io's watcher (watcher.rs, Rust over the `notify` crate --
// inotify on Linux, FSEvents on macOS, ReadDirectoryChangesW on Windows) behind a C++ interface.
// It watches each file's directory and judges changes by re-stamping the files, so replaced,
// recreated and symlinked files keep firing; a file need not exist yet, but its directory must.
// Its descriptors are CLOEXEC. Runtime-independent: it may be created before the loop is.
class FileWatcher {
 public:
  FileWatcher(): inner(new_file_watcher()) {}
  KJ_DISALLOW_COPY_AND_MOVE(FileWatcher);

  // Adds `path` to the watched set. Paths cross to Rust as the bytes kj::Path::toNativeString
  // produces (a unix path need not be UTF-8).
  void watch(kj::PathPtr path);

  // Resolves the next time any watched file changes (at once if one already has). The watch is
  // armed inside the call (operation-start policy above), so a caller that merely retains the
  // promise still has its files watched. Call again after resolution for the next change.
  kj::Promise<void> onChange();

 private:
  ::rust::Box<TokioFileWatcher> inner;
};

}  // namespace kj_rs_io
