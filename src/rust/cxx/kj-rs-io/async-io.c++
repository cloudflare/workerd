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

// =======================================================================================
// TokioConnectionReceiver

namespace {

// Builds the accepted connection's kj::PeerIdentity, mirroring KJ's SocketAddress::getIdentity()
// (kj/async-io-unix.c++): NetworkPeerIdentity wrapping the peer's address for TCP peers (its
// toString() is "ip:port" / "[v6]:port", byte-identical to KJ's format -- workerd's HTTP
// listener puts this string in the cf blob's clientIp), LocalPeerIdentity with the peer's
// process credentials for unix sockets, UnknownPeerIdentity otherwise.
kj::Own<kj::PeerIdentity> peerIdentityFromSockaddr(
    struct sockaddr *sa, kj::uint addrlen, [[maybe_unused]] kj::AsyncIoStream &stream) {
  switch (sa->sa_family) {
    case AF_INET:
    case AF_INET6: {
      // The identity's NetworkAddress uses an allow-all filter (not the listener's): it exists
      // for toString()/getAddress(); restrictPeers enforcement on this listener already happened
      // in the accept loop. (KJ instead threads the listener's filter through, which only
      // matters if a caller connect()s back through the identity address -- see PeerFilter's
      // immobility note for why we don't hold a reference to a possibly-narrower filter here.)
      static PeerFilter allowAll;
      auto address = network_get_sockaddr(
          ::rust::Slice<const uint8_t>(reinterpret_cast<const uint8_t *>(sa), addrlen));
      return kj::NetworkPeerIdentity::newInstance(
          kj::heap<TokioNetworkAddress>(kj::mv(address), allowAll));
    }
#if !_WIN32
    case AF_UNIX: {
      // Same credential sources and invalid-value handling as KJ (SO_PEERCRED on Linux,
      // LOCAL_PEERCRED/LOCAL_PEERPID on BSDs/macOS; OpenBSD defines SO_PEERCRED but with a
      // different interface, so it uses the LOCAL_PEERCRED arm).
      kj::LocalPeerIdentity::Credentials result;
#if defined(SO_PEERCRED) && !__OpenBSD__
      struct ucred creds;
      kj::uint length = sizeof(creds);
      stream.getsockopt(SOL_SOCKET, SO_PEERCRED, &creds, &length);
      if (creds.pid > 0) {
        result.pid = creds.pid;
      }
      if (creds.uid != static_cast<uid_t>(-1)) {
        result.uid = creds.uid;
      }
#elifdef LOCAL_PEERCRED
      struct xucred creds;
      kj::uint length = sizeof(creds);
      stream.getsockopt(SOL_LOCAL, LOCAL_PEERCRED, &creds, &length);
      KJ_ASSERT(length == sizeof(creds));
      if (creds.cr_uid != static_cast<uid_t>(-1)) {
        result.uid = creds.cr_uid;
      }
#ifdef LOCAL_PEERPID
      pid_t pid;
      length = sizeof(pid);
      stream.getsockopt(SOL_LOCAL, LOCAL_PEERPID, &pid, &length);
      KJ_ASSERT(length == sizeof(pid));
      if (pid > 0) {
        result.pid = pid;
      }
#endif
#endif
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

kj::Promise<kj::AuthenticatedStream> TokioConnectionReceiver::acceptImpl(bool authenticated) {
  for (;;) {
    auto stream = co_await listener_accept(*inner);
    // restrictPeers / NetworkFilter enforcement, mirroring KJ: a connection from a disallowed
    // peer is silently dropped and we keep accepting.
    struct sockaddr_storage addr;
    memset(&addr, 0, sizeof(addr));
    kj::uint addrlen = 0;
    KJ_IF_SOME(exception, kj::runCatchingExceptions([&]() {
      auto bytes = stream_peer_addr(*stream);
      KJ_ASSERT(bytes.size() <= sizeof(addr), "sockaddr too large");
      memcpy(&addr, bytes.data(), bytes.size());
      addrlen = bytes.size();
    })) {
      // The peer can reset the connection between tokio's accept() and this call, in which
      // case getpeername fails (EINVAL on macOS, ENOTCONN elsewhere). The connection is dead;
      // drop it and keep accepting. This must NOT throw: an exception here propagates out of
      // the server's accept loop and takes down the whole process (observed as a fatal
      // uncaught kj::Exception under client abort storms). Unlike KJ's native accept path,
      // which gets the peer address atomically from accept4(), we re-derive it and so must
      // tolerate the race. Log at INFO (off by default) for observability under abort storms.
      KJ_LOG(INFO, "dropping accepted connection; could not read peer address", exception);
      continue;
    }
    if (!filter.shouldAllow(reinterpret_cast<struct sockaddr *>(&addr), addrlen)) {
      // Drop the disallowed connection and wait for the next one.
      continue;
    }
    kj::AuthenticatedStream result;
    result.stream = kj::heap<TokioAsyncIoStream>(kj::mv(stream));
    if (authenticated) {
      result.peerIdentity = peerIdentityFromSockaddr(
          reinterpret_cast<struct sockaddr *>(&addr), addrlen, *result.stream);
    } else {
      result.peerIdentity = kj::UnknownPeerIdentity::newInstance();
    }
    co_return kj::mv(result);
  }
}

kj::uint TokioConnectionReceiver::getPort() {
  return listener_port(*inner);
}

void TokioConnectionReceiver::getsockopt(int level, int option, void *value, kj::uint *length) {
  *length = listener_getsockopt(
      *inner, level, option, ::rust::Slice<uint8_t>(reinterpret_cast<uint8_t *>(value), *length));
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
  // KJ contract (NetworkAddressImpl::connect() in kj/async-io-unix.c++): callers may drop the
  // NetworkAddress while the returned promise is still pending. We honor this by cloning the
  // resolved address list into a coroutine frame local: the frame owns the copy, so it
  // survives every co_await and is dropped on completion/cancellation. connect() may be called
  // repeatedly on the same address, so we clone rather than move `*inner` out of `this`.
  // `filter` is the network's filter (provider-owned, shared) and must outlive the promise,
  // per KJ (where it lives in the long-lived provider).
  auto addr = address_clone(*inner);
  size_t count = address_count(*addr);
  KJ_REQUIRE(count > 0, "no addresses to connect to");

  // Try each resolved address in order; a filter block or connect error falls through to the
  // next one, and the last address's exception propagates (KJ parity).
  kj::Maybe<kj::Exception> lastException;
  for (size_t i = 0; i < count; i++) {
    kj::Maybe<kj::Own<kj::AsyncIoStream>> stream;
    try {
      auto raw = address_raw_sockaddr(*addr, i);
      // Copy into sockaddr_storage for alignment (rust::Vec<u8> data is 1-aligned).
      struct sockaddr_storage storage;
      memset(&storage, 0, sizeof(storage));
      KJ_REQUIRE(raw.size() <= sizeof(storage), "sockaddr too large");
      memcpy(&storage, raw.data(), raw.size());
      if (!filter.shouldAllow(reinterpret_cast<struct sockaddr *>(&storage), raw.size())) {
        // Exact KJ error text; error-string parity matters to callers.
        lastException = KJ_EXCEPTION(FAILED, "connect() blocked by restrictPeers()");
      } else {
        stream = kj::heap<TokioAsyncIoStream>(co_await address_connect_index(*addr, i));
      }
    } catch (...) {
      // Note: getCaughtExceptionAsKj() rethrows kj::CanceledException, so cancellation still
      // propagates out of this coroutine instead of being folded into lastException.
      lastException = kj::getCaughtExceptionAsKj();
    }
    KJ_IF_SOME(s, stream) {
      co_return kj::mv(s);
    }
  }

  kj::throwFatalException(kj::mv(KJ_ASSERT_NONNULL(lastException)));
}

kj::Own<kj::ConnectionReceiver> TokioNetworkAddress::listen() {
  return kj::heap<TokioConnectionReceiver>(address_listen(*inner), filter);
}

kj::Own<kj::NetworkAddress> TokioNetworkAddress::clone() {
  return kj::heap<TokioNetworkAddress>(address_clone(*inner), filter);
}

kj::String TokioNetworkAddress::toString() {
  auto text = address_to_string(*inner);
  return kj::heapString(text.data(), text.size());
}

kj::Promise<kj::Own<kj::NetworkAddress>> TokioNetwork::parseAddress(
    kj::StringPtr addr, kj::uint portHint) {
  KJ_REQUIRE(portHint < 65536, "port hint too large", portHint);
  // The Rust side takes an owned copy: the caller's buffer need not outlive this call.
  //
  // Note: unlike KJ, disallowed (restrictPeers) DNS results are not dropped here; they are
  // rejected at connect()/accept() time instead. See TokioNetworkAddress.
  return network_parse_address(
      ::rust::String(addr.begin(), addr.size()), static_cast<uint16_t>(portHint))
      .then([this](::rust::Box<TokioAddress> address) -> kj::Own<kj::NetworkAddress> {
    return kj::heap<TokioNetworkAddress>(kj::mv(address), filter);
  });
}

kj::Own<kj::NetworkAddress> TokioNetwork::getSockaddr(const void *sockaddr, kj::uint len) {
  // KJ parity: getSockaddr() rejects filtered addresses eagerly (same error text as KJ).
  KJ_REQUIRE(filter.shouldAllow(reinterpret_cast<const struct sockaddr *>(sockaddr), len),
      "address blocked by restrictPeers()");
  return kj::heap<TokioNetworkAddress>(network_get_sockaddr(::rust::Slice<const uint8_t>(
                                           reinterpret_cast<const uint8_t *>(sockaddr), len)),
      filter);
}

kj::Own<kj::Network> TokioNetwork::restrictPeers(
    kj::ArrayPtr<const kj::StringPtr> allow, kj::ArrayPtr<const kj::StringPtr> deny) {
  // The child references this network's filter chain: this network must outlive the returned
  // one (same constraint as KJ's networks).
  return kj::heap<TokioNetwork>(*this, allow, deny);
}

// =======================================================================================
// TokioLowLevelAsyncIoProvider

namespace {

#if _WIN32
// Normalizes KJ's fd-wrapping flags so Rust always receives a SOCKET it owns, in non-blocking
// mode: the windows arm of prepareFd, mirroring the unix arm below in *effect*, not mechanism.
// kj's own win32 provider (capnproto async-io-win32.c++: OwnedFd, NEW_FD_FLAGS) never dups a
// borrowed socket -- it merely skips closesocket() on destruction when TAKE_OWNERSHIP is
// absent -- ignores ALREADY_CLOEXEC entirely (there is no CLOEXEC on Windows; handle
// inheritance is the analogue), and never toggles non-blocking mode (it uses overlapped I/O,
// not readiness). Rust's OwnedSocket has no "don't close" mode, so a borrowed socket is
// duplicated (WSADuplicateSocketW + WSASocketW, non-inheritable) into a handle Rust can own;
// and tokio/mio's readiness model requires non-blocking sockets, so FIONBIO is set unless the
// caller declared ALREADY_NONBLOCK (the duplicate shares the underlying socket state, so this
// is observed through the caller's handle too, matching the unix dup()+O_NONBLOCK behavior).
// Validated by Windows CI.
uintptr_t prepareFd(uintptr_t fd, kj::uint flags) {
  SOCKET sock = static_cast<SOCKET>(fd);
  if ((flags & kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP) == 0) {
    WSAPROTOCOL_INFOW info;
    KJ_WINSOCK(WSADuplicateSocketW(sock, GetCurrentProcessId(), &info));
    SOCKET duped = WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &info, 0,
        WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (duped == INVALID_SOCKET) {
      KJ_FAIL_WIN32("WSASocketW()", WSAGetLastError());
    }
    sock = duped;
  }
  // ALREADY_NONBLOCK does not exist on Windows (kj declares it under #if !_WIN32), so callers
  // cannot assert pre-set non-blocking mode; always enable it (idempotent).
  u_long mode = 1;
  KJ_WINSOCK(ioctlsocket(sock, FIONBIO, &mode));
  return static_cast<uintptr_t>(sock);
}
#else
// Normalizes KJ's fd-wrapping flags so Rust always receives an fd it owns, with CLOEXEC set and
// in non-blocking mode.
int prepareFd(int fd, kj::uint flags) {
  if ((flags & kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP) == 0) {
    // dup() shares the open file description — the O_NONBLOCK set below is observed through the
    // caller's fd too, matching KJ (which sets O_NONBLOCK on the caller's fd directly) — while
    // giving Rust a descriptor it can own and close.
    int duped;
    KJ_SYSCALL(duped = ::dup(fd));
    fd = duped;
    KJ_SYSCALL(fcntl(fd, F_SETFD, FD_CLOEXEC));
  } else if ((flags & kj::LowLevelAsyncIoProvider::ALREADY_CLOEXEC) == 0) {
    KJ_SYSCALL(fcntl(fd, F_SETFD, FD_CLOEXEC));
  }
  if ((flags & kj::LowLevelAsyncIoProvider::ALREADY_NONBLOCK) == 0) {
    int fl;
    KJ_SYSCALL(fl = fcntl(fd, F_GETFL));
    if ((fl & O_NONBLOCK) == 0) {
      KJ_SYSCALL(fcntl(fd, F_SETFL, fl | O_NONBLOCK));
    }
  }
  return fd;
}

class TokioInputStreamFd final: public kj::AsyncInputStream {
 public:
  explicit TokioInputStreamFd(::rust::Box<TokioInputFd> inner): inner(kj::mv(inner)) {}

  kj::Promise<size_t> tryRead(void *buffer, size_t minBytes, size_t maxBytes) override {
    return input_fd_try_read(
        *inner, ::rust::Slice<uint8_t>(reinterpret_cast<uint8_t *>(buffer), maxBytes), minBytes);
  }

 private:
  ::rust::Box<TokioInputFd> inner;
};

// kj::AsyncOutputStream over an arbitrary writable fd.
class TokioOutputStreamFd final: public kj::AsyncOutputStream {
 public:
  explicit TokioOutputStreamFd(::rust::Box<TokioOutputFd> inner): inner(kj::mv(inner)) {}

  kj::Promise<void> write(kj::ArrayPtr<const kj::byte> buffer) override {
    return output_fd_write(*inner, ::rust::Slice<const uint8_t>(buffer.begin(), buffer.size()));
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    for (auto piece: pieces) {
      co_await write(piece);
    }
  }

  // Pipes/arbitrary fds have no portable disconnect detection here; KJ allows a never-resolving
  // promise for such streams.
  kj::Promise<void> whenWriteDisconnected() override {
    return kj::NEVER_DONE;
  }

 private:
  ::rust::Box<TokioOutputFd> inner;
};
#endif  // _WIN32 (prepareFd platform arms; the pipe-fd stream classes above are unix-only)

}  // namespace

}  // namespace kj_rs_io
