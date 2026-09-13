#include "kj-rs-io/async-io.h"

#include <kj/debug.h>
#include <kj/io.h>

#include <cstring>

#if _WIN32
#include <winsock2.h>
// windows.h (pulled in by winsock2.h) defines ERROR as a macro, which breaks KJ_LOG(ERROR).
#include <kj/windows-sanity.h>
#else
#include <signal.h>
#include <sys/socket.h>
#endif

// The unwrap fast path (unwrapTokioStream / isTokioStream) recognizes kj-rs-io streams by
// dynamic_cast. Without RTTI every stream would silently be treated as foreign and take the
// pump path; make that a build error rather than a performance mystery.
#if KJ_NO_RTTI
#error "kj-rs-io's unwrap fast path requires RTTI (dynamicDowncastIfAvailable); KJ_NO_RTTI is set"
#endif

namespace kj_rs_io {

namespace {

// Operation-start policy (see async-io.h): bridged futures are cold until first polled, KJ's
// native streams start their syscall inside the call. Every non-coroutine adapter method wraps
// its bridged promise here so the operation is under way when the promise is returned, whether
// or not the caller ever awaits it. Coroutine methods (accept, connect, writePieces) start by
// construction: their bodies run up to the first co_await, which polls the bridged future.
template <typename T>
kj::Promise<T> started(kj::Promise<T> promise) {
  return promise.eagerlyEvaluate(nullptr);
}

}  // namespace

// =======================================================================================
// TokioAsyncIoStream

kj::Promise<size_t> TokioAsyncIoStream::tryRead(void *buffer, size_t minBytes, size_t maxBytes) {
  // `buffer` may be uninitialized (KJ allows it); it crosses as a raw pointer + length and the
  // Rust side treats it as MaybeUninit storage. Valid until the promise settles, per KJ.
  return started(stream_try_read(*inner, reinterpret_cast<uint8_t *>(buffer), maxBytes, minBytes));
}

kj::Promise<void> TokioAsyncIoStream::write(kj::ArrayPtr<const kj::byte> buffer) {
  return started(stream_write(*inner, ::rust::Slice<const uint8_t>(buffer.begin(), buffer.size())));
}

kj::Promise<void> TokioAsyncIoStream::write(
    kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) {
  return writePieces(pieces);
}

kj::Promise<void> TokioAsyncIoStream::writePieces(
    kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) {
  // One bridged operation for all pieces (writev on the Rust side). The bridged future borrows
  // the KjPieces it is handed, so it lives in this coroutine's frame; the pieces themselves are
  // the caller's, valid until the promise settles per the kj::AsyncOutputStream contract.
  KjPieces owned{pieces};
  co_await stream_write_pieces(*inner, owned);
}

kj::Promise<void> TokioAsyncIoStream::whenWriteDisconnected() {
  return started(stream_when_write_disconnected(*inner));
}

void TokioAsyncIoStream::shutdownWrite() {
  stream_shutdown_write(*inner);
}

void TokioAsyncIoStream::abortRead() {
  stream_abort_read(*inner);
}

void TokioAsyncIoStream::getsockopt(int level, int option, void *value, kj::uint *length) {
  // The platform seam lives on the Rust side (stream_getsockopt); errors surface as kj
  // exceptions, like KJ_SYSCALL. Raw socklen in/out semantics: the syscall's reported length
  // is mirrored back verbatim. `value` may be uninitialized, hence pointer + length.
  *length = stream_getsockopt(*inner, level, option, reinterpret_cast<uint8_t *>(value), *length);
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
  // getWin32Handle() below instead.
  return kj::none;
#else
  // On unix the raw socket handle is the fd, widened losslessly to int64 by the bridge; -1
  // means the wrapper is hollow (unwrapped).
  int64_t handle = stream_try_raw_handle(*inner);
  if (handle < 0) return kj::none;
  return static_cast<int>(handle);
#endif
}

#if _WIN32
// Mirrors the unix getFd() arm (and kj's own win32 AsyncStreamFd, which returns its SOCKET cast
// to void* -- capnproto async-io-win32.c++).
kj::Maybe<void *> TokioAsyncIoStream::getWin32Handle() const {
  int64_t handle = stream_try_raw_handle(*inner);
  if (handle < 0) return kj::none;
  return reinterpret_cast<void *>(static_cast<uintptr_t>(handle));
}
#endif

bool isTokioStream(const kj::AsyncIoStream &stream) {
  return kj::dynamicDowncastIfAvailable<const TokioAsyncIoStream>(stream) != kj::none;
}

::rust::Box<TokioStream> unwrapTokioStream(kj::AsyncIoStream &stream) {
  KJ_IF_SOME(tokioStream, kj::dynamicDowncastIfAvailable<TokioAsyncIoStream>(stream)) {
    return tokioStream.unwrap();
  }
  KJ_FAIL_REQUIRE("stream is not a kj-rs-io tokio-backed stream; cannot unwrap");
}

// =======================================================================================
// TokioConnectionReceiver

namespace {

// Builds the accepted connection's kj::PeerIdentity, mirroring KJ's SocketAddress::getIdentity()
// (kj/async-io-unix.c++): NetworkPeerIdentity wrapping the peer's address for TCP peers (its
// toString() is "ip:port" / "[v6]:port", byte-identical to KJ's format -- workerd's HTTP
// listener puts this string in the cf blob's clientIp), LocalPeerIdentity with the peer's
// process credentials for unix sockets, UnknownPeerIdentity otherwise.
kj::Own<kj::PeerIdentity> peerIdentityFromSockaddr(
    struct sockaddr *sa, kj::uint addrlen, [[maybe_unused]] const TokioStream &stream) {
  switch (sa->sa_family) {
    case AF_INET:
    case AF_INET6: {
      // The identity's NetworkAddress uses an allow-all filter (not the listener's): it exists
      // for toString()/getAddress(); restrictPeers enforcement on this listener already happened
      // in the accept loop. (KJ instead threads the listener's filter through, which only
      // matters if a caller connect()s back through the identity address.) A fresh filter per
      // identity, NOT a process-wide static: kj::Rc's refcount is not atomic, and accept loops
      // run on every tokio-ported loop thread, so a shared one would race.
      auto address = network_get_sockaddr(
          ::rust::Slice<const uint8_t>(reinterpret_cast<const uint8_t *>(sa), addrlen));
      return kj::NetworkPeerIdentity::newInstance(
          kj::heap<TokioNetworkAddress>(kj::mv(address), kj::rc<PeerFilter>()));
    }
#if !_WIN32
    case AF_UNIX: {
      // Same credential sources and validity rules as KJ (SO_PEERCRED on Linux, LOCAL_PEERCRED /
      // LOCAL_PEERPID on BSDs and macOS), read by the Rust side.
      auto creds = stream_peer_credentials(stream);
      kj::LocalPeerIdentity::Credentials result;
      if (creds.has_pid) result.pid = creds.pid;
      if (creds.has_uid) result.uid = creds.uid;
      return kj::LocalPeerIdentity::newInstance(result);
    }
#endif  // !_WIN32
    default:
      return kj::UnknownPeerIdentity::newInstance();
  }
}

}  // namespace

kj::Promise<kj::Own<kj::AsyncIoStream>> TokioConnectionReceiver::accept() {
  return acceptImpl(false).then(
      [](kj::AuthenticatedStream authenticated) { return kj::mv(authenticated.stream); });
}

kj::Promise<kj::AuthenticatedStream> TokioConnectionReceiver::acceptAuthenticated() {
  return acceptImpl(true);
}

kj::Own<kj::LowLevelAsyncIoProvider::NetworkFilter> TokioConnectionReceiver::filterShare() {
  KJ_IF_SOME(rc, shared) {
    return rc.addRef().toOwn();
  }
  return kj::Own<kj::LowLevelAsyncIoProvider::NetworkFilter>(
      &KJ_ASSERT_NONNULL(borrowed), kj::NullDisposer::instance);
}

kj::Promise<kj::AuthenticatedStream> TokioConnectionReceiver::acceptImpl(bool authenticated) {
  // Rust runs the accept loop: retries KJ's transient failures, tolerates TCP_NODELAY on a dead
  // socket, drops peers the filter disallows, and reports the peer address accept(2) returned.
  auto accepted = co_await listener_accept(*inner, filterShare());
  AlignedSockaddr peer(::rust::Slice<const uint8_t>(accepted.peer.data(), accepted.peer.size()));
  kj::AuthenticatedStream result;
  if (authenticated) {
    result.peerIdentity = peerIdentityFromSockaddr(peer.get(), peer.length, *accepted.stream);
  } else {
    result.peerIdentity = kj::UnknownPeerIdentity::newInstance();
  }
  result.stream = kj::heap<TokioAsyncIoStream>(kj::mv(accepted.stream));
  co_return kj::mv(result);
}

kj::uint TokioConnectionReceiver::getPort() {
  return listener_port(*inner);
}

void TokioConnectionReceiver::getsockopt(int level, int option, void *value, kj::uint *length) {
  *length = listener_getsockopt(*inner, level, option, reinterpret_cast<uint8_t *>(value), *length);
}

void TokioConnectionReceiver::setsockopt(
    int level, int option, const void *value, kj::uint length) {
  listener_setsockopt(*inner, level, option,
      ::rust::Slice<const uint8_t>(reinterpret_cast<const uint8_t *>(value), length));
}

void TokioConnectionReceiver::getsockname(struct sockaddr *addr, kj::uint *length) {
  auto bytes = listener_local_addr(*inner);
  // Mirror the raw syscall's truncation semantics (see TokioAsyncIoStream::getsockname).
  memcpy(addr, bytes.data(), kj::min(bytes.size(), *length));
  *length = bytes.size();
}

// =======================================================================================
// TokioNetworkAddress / TokioNetwork

kj::Promise<kj::Own<kj::AsyncIoStream>> TokioNetworkAddress::connect() {
  // Rust runs the try-each-address loop with the filter (KJ's NetworkAddressImpl::connect()
  // semantics, including the "connect() blocked by restrictPeers()" text). The future owns its
  // own copy of the targets and a share of the filter chain, so this NetworkAddress may be
  // destroyed while the promise is pending (KJ's contract).
  return started(address_connect(*inner, filter.addRef().toOwn())
                     .then([](::rust::Box<TokioStream> stream) -> kj::Own<kj::AsyncIoStream> {
    return kj::heap<TokioAsyncIoStream>(kj::mv(stream));
  }));
}

kj::Own<kj::ConnectionReceiver> TokioNetworkAddress::listen() {
  return kj::heap<TokioConnectionReceiver>(address_listen(*inner), filter.addRef());
}

kj::Own<kj::NetworkAddress> TokioNetworkAddress::clone() {
  return kj::heap<TokioNetworkAddress>(address_clone(*inner), filter.addRef());
}

kj::String TokioNetworkAddress::toString() {
  auto text = address_to_string(*inner);
  return kj::heapString(text.data(), text.size());
}

kj::Promise<kj::Own<kj::NetworkAddress>> TokioNetwork::parseAddress(
    kj::StringPtr addr, kj::uint portHint) {
  KJ_REQUIRE(portHint < 65536, "port hint too large", portHint);
  // The Rust side takes an owned copy of the text and a share of the filter, applies KJ's
  // parse-time check to literals, and the continuation owns another share for the address
  // (no `this` capture: the promise is independent of this network's lifetime). Started
  // eagerly like every other operation, so the DNS lookup is under way when this returns (KJ
  // starts its resolver thread inside the call).
  return started(
      network_parse_address(::rust::String(addr.begin(), addr.size()),
          static_cast<uint16_t>(portHint), filter.addRef())
          .then([filter = filter.addRef()](
                    ::rust::Box<TokioAddress> address) mutable -> kj::Own<kj::NetworkAddress> {
    return kj::heap<TokioNetworkAddress>(kj::mv(address), kj::mv(filter));
  }));
}

kj::Own<kj::NetworkAddress> TokioNetwork::getSockaddr(const void *sockaddr, kj::uint len) {
  // KJ parity: getSockaddr() rejects filtered addresses eagerly (same check and error text as
  // KJ's NetworkImpl::getSockaddr).
  KJ_REQUIRE(filter->shouldAllow(reinterpret_cast<const struct sockaddr *>(sockaddr), len),
      "address blocked by restrictPeers()");
  return kj::heap<TokioNetworkAddress>(network_get_sockaddr(::rust::Slice<const uint8_t>(
                                           reinterpret_cast<const uint8_t *>(sockaddr), len)),
      filter.addRef());
}

kj::Own<kj::Network> TokioNetwork::restrictPeers(
    kj::ArrayPtr<const kj::StringPtr> allow, kj::ArrayPtr<const kj::StringPtr> deny) {
  // The child owns a share of this network's filter chain (see TokioNetwork's constructor), so
  // it remains valid even if this network is destroyed first.
  return kj::heap<TokioNetwork>(*this, allow, deny);
}

// =======================================================================================
// TokioLowLevelAsyncIoProvider

namespace {

// KJ's fd-wrapping flags cross the bridge verbatim; Rust applies them (ffi.rs prepare_fd).
static_assert(kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP == 1 << 0);
static_assert(kj::LowLevelAsyncIoProvider::ALREADY_CLOEXEC == 1 << 1);
#if !_WIN32
static_assert(kj::LowLevelAsyncIoProvider::ALREADY_NONBLOCK == 1 << 2);

// kj::AsyncInputStream over an arbitrary readable fd (pipe, socket, character device).
class TokioInputStreamFd final: public kj::AsyncInputStream {
 public:
  explicit TokioInputStreamFd(::rust::Box<TokioInputFd> inner): inner(kj::mv(inner)) {}

  kj::Promise<size_t> tryRead(void *buffer, size_t minBytes, size_t maxBytes) override {
    return started(
        input_fd_try_read(*inner, reinterpret_cast<uint8_t *>(buffer), maxBytes, minBytes));
  }

 private:
  ::rust::Box<TokioInputFd> inner;
};

// kj::AsyncOutputStream over an arbitrary writable fd.
class TokioOutputStreamFd final: public kj::AsyncOutputStream {
 public:
  explicit TokioOutputStreamFd(::rust::Box<TokioOutputFd> inner): inner(kj::mv(inner)) {}

  kj::Promise<void> write(kj::ArrayPtr<const kj::byte> buffer) override {
    return started(
        output_fd_write(*inner, ::rust::Slice<const uint8_t>(buffer.begin(), buffer.size())));
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    // One bridged operation for all pieces, like TokioAsyncIoStream::writePieces: the future
    // owns its share of the fd, and nothing here touches `this` after the co_await, so
    // destroying this stream while the write is pending dangles nothing.
    KjPieces owned{pieces};
    co_await output_fd_write_pieces(*inner, owned);
  }

  // Pipes/arbitrary fds have no portable disconnect detection here; KJ allows a never-resolving
  // promise for such streams.
  kj::Promise<void> whenWriteDisconnected() override {
    return kj::NEVER_DONE;
  }

 private:
  ::rust::Box<TokioOutputFd> inner;
};

// KJ parity (kj::UnixEventPort's constructor, kj/async-unix.c++): "We disable SIGPIPE because
// users of UnixEventPort almost certainly don't want it." A write to a peer that has gone away
// then fails with EPIPE -- surfaced as a DISCONNECTED kj::Exception -- instead of terminating
// the process. Once per process; the C++ runtime does not install any disposition for us, and
// neither does linking Rust into a C++ executable (Rust's own `main` shim, which would ignore
// SIGPIPE, never runs here).
void ignoreSigpipe() {
  static const bool ignored = ([]() {
    while (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
      int error = errno;
      if (error != EINTR) {
        KJ_FAIL_SYSCALL("signal(SIGPIPE, SIG_IGN)", error);
      }
    }
    return true;
  })();
  (void)ignored;
}
#endif  // !_WIN32 (the pipe-fd stream classes and ignoreSigpipe are unix-only)

}  // namespace

kj::Own<kj::AsyncInputStream> TokioLowLevelAsyncIoProvider::wrapInputFd(Fd fd, kj::uint flags) {
#if _WIN32
  // KJ parity: on win32, LowLevelAsyncIoProvider::Fd is documented as a SOCKET (async-io.h:
  // "On Windows, the `fd` parameter to each of these methods must be a SOCKET"), and kj's own
  // win32 provider implements wrapInputFd/wrapOutputFd *identically* to wrapSocketFd
  // (async-io-win32.c++: all three wrap the SOCKET in AsyncStreamFd) -- even kj's "pipes" on
  // Windows are loopback-TCP socketpairs (newOsSocketpair). So there is no pipe-HANDLE tier to
  // implement; delegate to the socket path.
  return kj::heap<TokioAsyncIoStream>(wrap_socket_fd(static_cast<int64_t>(fd), flags));
#else
  return kj::heap<TokioInputStreamFd>(wrap_input_fd(fd, flags));
#endif
}

kj::Own<kj::AsyncOutputStream> TokioLowLevelAsyncIoProvider::wrapOutputFd(Fd fd, kj::uint flags) {
#if _WIN32
  // See wrapInputFd above: win32 Fd is a SOCKET and kj's win32 wrapOutputFd == wrapSocketFd.
  return kj::heap<TokioAsyncIoStream>(wrap_socket_fd(static_cast<int64_t>(fd), flags));
#else
  return kj::heap<TokioOutputStreamFd>(wrap_output_fd(fd, flags));
#endif
}

kj::Own<kj::AsyncIoStream> TokioLowLevelAsyncIoProvider::wrapSocketFd(Fd fd, kj::uint flags) {
  // `Fd` is int on unix and uintptr_t (SOCKET) on win32; the bridge carries it widened to
  // int64 (a "raw socket handle") either way, with -1 == INVALID_SOCKET as the one sentinel.
  return kj::heap<TokioAsyncIoStream>(wrap_socket_fd(static_cast<int64_t>(fd), flags));
}

kj::Promise<kj::Own<kj::AsyncIoStream>> TokioLowLevelAsyncIoProvider::wrapConnectingSocketFd(
    Fd fd, const struct sockaddr *addr, kj::uint addrlen, kj::uint flags) {
  // Materialize Rust ownership before any later fallible work. The generated async bridge then
  // moves this owner into its promise, so cancellation before the first poll still closes it.
  auto socket = own_connecting_socket(static_cast<int64_t>(fd), flags);
  // The Rust side takes an owned copy of the sockaddr: the caller's pointer need not outlive
  // this call (KJ's own implementation copies too).
  ::rust::Vec<uint8_t> addrCopy;
  addrCopy.reserve(addrlen);
  const uint8_t *addrBytes = reinterpret_cast<const uint8_t *>(addr);
  for (kj::uint i = 0; i < addrlen; i++) {
    addrCopy.push_back(addrBytes[i]);
  }
  return started(wrap_connecting_socket_fd(kj::mv(socket), kj::mv(addrCopy))
                     .then([](::rust::Box<TokioStream> stream) -> kj::Own<kj::AsyncIoStream> {
    return kj::heap<TokioAsyncIoStream>(kj::mv(stream));
  }));
}

kj::Own<kj::ConnectionReceiver> TokioLowLevelAsyncIoProvider::wrapListenSocketFd(
    Fd fd, NetworkFilter &filter, kj::uint flags) {
  // `filter` applies to accepted connections (KJ parity); it must outlive the receiver.
  return kj::heap<TokioConnectionReceiver>(wrap_listen_fd(static_cast<int64_t>(fd), flags), filter);
}

// =======================================================================================
// TokioAsyncIoProvider / setup

kj::AsyncIoProvider::PipeThread TokioAsyncIoProvider::newPipeThread(
    kj::Function<void(kj::AsyncIoProvider &, kj::AsyncIoStream &, kj::WaitScope &)> startFunc) {
  KJ_UNIMPLEMENTED("kj-rs-io does not implement newPipeThread() (workerd does not use it)");
}

TokioAsyncIoContext::TokioAsyncIoContext()
    : base(kj_rs_tokio::setupTokioAsyncIo()),
      lowLevelProvider(base.getTimer()),
      provider(base.getTimer()) {
#if !_WIN32
  ignoreSigpipe();
#endif
}

TokioAsyncIoContext setupTokioAsyncIo() {
  return TokioAsyncIoContext();
}

// =======================================================================================
// Signals

kj::Promise<void> onSignal(int signum) {
  // Started eagerly: the tokio handler is registered by the time this returns, so a caller that
  // merely retains the promise (workerd's drain watcher) still has the signal watched.
  return started(wait_for_signal(signum));
}

}  // namespace kj_rs_io
