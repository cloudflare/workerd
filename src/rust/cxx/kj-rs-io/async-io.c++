#include "kj-rs-io/async-io.h"

#include <kj/debug.h>
#include <kj/exception.h>
#include <kj/io.h>
#include <kj/one-of.h>

#include <cstring>

#if _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>  // struct sockaddr_in6
// windows.h (pulled in by winsock2.h) defines ERROR as a macro, which breaks KJ_LOG(ERROR).
#include <kj/windows-sanity.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#endif

namespace kj_rs_io {

namespace {

// Operation-start policy (async-io.h): a bridged future is cold until first polled; KJ's own
// streams start their operation inside the call. eagerlyEvaluate's first poll is synchronous
// (kj::_::EagerPromiseNodeBase's constructor), so this starts the Rust body before returning.
template <typename T>
kj::Promise<T> started(kj::Promise<T> promise) {
  return promise.eagerlyEvaluate(nullptr);
}

// =======================================================================================
// struct sockaddr <-> SocketAddress. The only place in kj-rs-io a raw sockaddr is read or
// written: KJ's interface speaks them at getSockaddr(), getsockname()/getpeername() and
// NetworkFilter::shouldAllow(); everything else, on both sides of the bridge, is typed.

struct RawSockaddr {
  struct sockaddr_storage storage;
  kj::uint length;

  const struct sockaddr *get() const {
    return reinterpret_cast<const struct sockaddr *>(&storage);
  }
};

template <typename T>
RawSockaddr rawFrom(const T &encoded, size_t length) {
  RawSockaddr raw;
  memset(&raw.storage, 0, sizeof(raw.storage));
  memcpy(&raw.storage, &encoded, length);
  raw.length = length;
  return raw;
}

// A typed address as the struct KJ's NetworkFilter and getsockname() callers read. Every byte
// of the result is written (zero-filled, then the family's fields).
RawSockaddr encodeSockaddr(const SocketAddress &addr) {
  switch (addr.kind) {
    case AddressKind::Ipv4: {
      struct sockaddr_in in;
      memset(&in, 0, sizeof(in));
      in.sin_family = AF_INET;
      in.sin_port = htons(addr.port);
      memcpy(&in.sin_addr, addr.ip.data(), sizeof(in.sin_addr));
      return rawFrom(in, sizeof(in));
    }
    case AddressKind::Ipv6: {
      struct sockaddr_in6 in6;
      memset(&in6, 0, sizeof(in6));
      in6.sin6_family = AF_INET6;
      in6.sin6_port = htons(addr.port);
      in6.sin6_flowinfo = htonl(addr.flowinfo);
      memcpy(&in6.sin6_addr, addr.ip.data(), sizeof(in6.sin6_addr));
      in6.sin6_scope_id = addr.scope_id;
      return rawFrom(in6, sizeof(in6));
    }
#if !_WIN32
    case AddressKind::UnixPath:
    case AddressKind::UnixAbstract:
    case AddressKind::UnixUnnamed: {
      constexpr size_t pathOffset = offsetof(struct sockaddr_un, sun_path);
      struct sockaddr_un un;
      memset(&un, 0, sizeof(un));
      un.sun_family = AF_UNIX;
      size_t nameBytes = addr.name.size();
      if (addr.kind == AddressKind::UnixUnnamed) {
        return rawFrom(un, pathOffset);
      } else if (addr.kind == AddressKind::UnixAbstract) {
        // KJ's form: a leading NUL, the name, and a length covering exactly that.
        KJ_REQUIRE(nameBytes + 1 <= sizeof(un.sun_path), "abstract unix socket name too long");
        memcpy(un.sun_path + 1, addr.name.data(), nameBytes);
        return rawFrom(un, pathOffset + 1 + nameBytes);
      } else {
        // A pathname: the path, plus its NUL if there is room for it (a path filling sun_path
        // entirely is legal without one).
        KJ_REQUIRE(nameBytes <= sizeof(un.sun_path), "unix socket path too long");
        memcpy(un.sun_path, addr.name.data(), nameBytes);
        return rawFrom(un, pathOffset + nameBytes + (nameBytes < sizeof(un.sun_path) ? 1 : 0));
      }
    }
#endif
    case AddressKind::Loopback:
      KJ_FAIL_REQUIRE("a loopback: address has no struct sockaddr");
    default:
      KJ_FAIL_REQUIRE("unsupported socket address kind", static_cast<int>(addr.kind));
  }
}

::rust::Vec<uint8_t> bytesVec(const uint8_t *data, size_t size) {
  ::rust::Vec<uint8_t> out;
  out.reserve(size);
  for (size_t i = 0; i < size; i++) out.push_back(data[i]);
  return out;
}

// A caller's struct sockaddr (getSockaddr) as a typed address. KJ's interface promises only that
// the family's *fields* are initialized -- a caller may fill a sockaddr_in and never touch
// `sin_zero`, or hand over a sockaddr_storage with a length larger than the family's struct --
// so only the fields are read, through a copy of the family's struct. Unix addresses follow
// KJ's safeUnixPath: a leading NUL means an abstract name spanning the whole given length, a
// pathname ends at its first NUL. Unsupported families and short lengths throw.
SocketAddress decodeSockaddr(const void *addr, kj::uint addrlen) {
  KJ_REQUIRE(addrlen <= sizeof(struct sockaddr_storage), "sockaddr too large", addrlen);
  struct sockaddr header;
  memset(&header, 0, sizeof(header));
  KJ_REQUIRE(addrlen >= offsetof(struct sockaddr, sa_family) + sizeof(header.sa_family),
      "sockaddr too short", addrlen);
  memcpy(&header, addr, kj::min<size_t>(addrlen, sizeof(header)));

  SocketAddress out;
  out.ip.fill(0);
  out.port = 0;
  out.flowinfo = 0;
  out.scope_id = 0;
  switch (header.sa_family) {
    case AF_INET: {
      KJ_REQUIRE(addrlen >= sizeof(struct sockaddr_in), "sockaddr_in too short", addrlen);
      struct sockaddr_in in;
      memcpy(&in, addr, sizeof(in));
      out.kind = AddressKind::Ipv4;
      memcpy(out.ip.data(), &in.sin_addr, sizeof(in.sin_addr));
      out.port = ntohs(in.sin_port);
      return out;
    }
    case AF_INET6: {
      KJ_REQUIRE(addrlen >= sizeof(struct sockaddr_in6), "sockaddr_in6 too short", addrlen);
      struct sockaddr_in6 in6;
      memcpy(&in6, addr, sizeof(in6));
      out.kind = AddressKind::Ipv6;
      memcpy(out.ip.data(), &in6.sin6_addr, sizeof(in6.sin6_addr));
      out.port = ntohs(in6.sin6_port);
      out.flowinfo = ntohl(in6.sin6_flowinfo);
      out.scope_id = in6.sin6_scope_id;
      return out;
    }
#if !_WIN32
    case AF_UNIX: {
      constexpr size_t pathOffset = offsetof(struct sockaddr_un, sun_path);
      KJ_REQUIRE(addrlen >= pathOffset, "sockaddr_un too short", addrlen);
      struct sockaddr_un un;
      memset(&un, 0, sizeof(un));
      memcpy(&un, addr, kj::min<size_t>(addrlen, sizeof(un)));
      size_t pathBytes = kj::min<size_t>(addrlen - pathOffset, sizeof(un.sun_path));
      const uint8_t *path = reinterpret_cast<const uint8_t *>(un.sun_path);
      if (pathBytes == 0) {
        out.kind = AddressKind::UnixUnnamed;
      } else if (un.sun_path[0] == '\0') {
        out.kind = AddressKind::UnixAbstract;
        out.name = bytesVec(path + 1, pathBytes - 1);
      } else {
        out.kind = AddressKind::UnixPath;
        out.name = bytesVec(path, strnlen(un.sun_path, pathBytes));
      }
      return out;
    }
#endif
    default:
      KJ_FAIL_REQUIRE("unsupported socket address family", header.sa_family);
  }
}

// KJ's getsockname()/getpeername() convention: copy what fits, report the full length.
void copyOut(const RawSockaddr &raw, struct sockaddr *addr, kj::uint *length) {
  memcpy(addr, &raw.storage, kj::min(raw.length, *length));
  *length = raw.length;
}

// restrictPeers(): KJ's decision for a typed address. A loopback address is not a network peer
// (loopback.rs) and has no sockaddr for the filter to judge; it is always allowed.
bool allowed(const PeerFilter &filter, const SocketAddress &addr) {
  if (addr.kind == AddressKind::Loopback) return true;
  auto raw = encodeSockaddr(addr);
  return filter.allows(raw.get(), raw.length);
}

// A connection's kj::PeerIdentity from the peer's typed address (what accept(2) reported, or the
// address connect(2) was given), mirroring KJ's SocketAddress::getIdentity(): a
// NetworkPeerIdentity wrapping the peer's address for TCP peers (its toString() is "ip:port" /
// "[v6]:port", byte-identical to KJ's -- workerd's HTTP listener puts it in the cf blob's
// clientIp), a LocalPeerIdentity with the peer's process credentials for unix sockets. `filter`
// is the receiver's / address's own chain, threaded into the identity's NetworkAddress as KJ
// does, so a connect() back through the identity is restricted the same way.
kj::Own<kj::PeerIdentity> peerIdentity(const PeerStream &peer, kj::Arc<PeerFilter> filter) {
  switch (peer.peer.kind) {
    case AddressKind::Ipv4:
    case AddressKind::Ipv6:
      return kj::NetworkPeerIdentity::newInstance(
          kj::heap<TokioNetworkAddress>(network_address_from(peer.peer), kj::mv(filter)));
#if !_WIN32
    case AddressKind::UnixPath:
    case AddressKind::UnixAbstract:
    case AddressKind::UnixUnnamed: {
      // Same credential sources and validity rules as KJ (SO_PEERCRED on Linux, LOCAL_PEERCRED /
      // LOCAL_PEERPID on BSDs and macOS), read by the Rust side.
      auto creds = stream_peer_credentials(*peer.stream);
      kj::LocalPeerIdentity::Credentials result;
      if (creds.has_pid) result.pid = creds.pid;
      if (creds.has_uid) result.uid = creds.uid;
      return kj::LocalPeerIdentity::newInstance(result);
    }
#endif
    default:
      return kj::UnknownPeerIdentity::newInstance();
  }
}

// KJ's connect loop (NetworkAddressImpl::connect, kj/async-io-unix.c++): the targets in order,
// skipping the ones the filter disallows, the last failure reported if none connects. A free
// coroutine owning copies of everything it needs (`address` is its own handle to the parsed
// address), so the kj::NetworkAddress that started it may be destroyed while it is pending.
kj::Promise<PeerStream> connectAny(::rust::Box<TokioAddress> address,
    ::rust::Vec<SocketAddress> targets,
    kj::Arc<PeerFilter> filter) {
  kj::Maybe<kj::Exception> lastError;
  for (auto &target: targets) {
    if (!allowed(*filter, target)) {
      lastError = KJ_EXCEPTION(FAILED, "connect() blocked by restrictPeers()");
      continue;
    }
    auto outcome =
        co_await connect_target(*address, SocketAddress(target))
            .then(
                [](::rust::Box<TokioStream> stream)
                    -> kj::OneOf<::rust::Box<TokioStream>, kj::Exception> {
      return kj::mv(stream);
    },
                [](kj::Exception &&e) -> kj::OneOf<::rust::Box<TokioStream>, kj::Exception> {
      return kj::mv(e);
    });
    KJ_SWITCH_ONEOF(outcome) {
      KJ_CASE_ONEOF(stream, ::rust::Box<TokioStream>) {
        co_return PeerStream{kj::mv(stream), target};
      }
      KJ_CASE_ONEOF(e, kj::Exception) {
        lastError = kj::mv(e);
      }
    }
  }
  KJ_IF_SOME(e, lastError) {
    kj::throwFatalException(kj::mv(e));
  }
  KJ_FAIL_REQUIRE("connect(): no addresses to connect to");
}

// KJ's accept loop (acceptImpl): peers the filter disallows are dropped and the receiver keeps
// listening. Owns a share of the listener, so the kj::ConnectionReceiver may be destroyed while
// an accept is pending.
kj::Promise<PeerStream> acceptAllowed(
    ::rust::Box<TokioListener> listener, kj::Arc<PeerFilter> filter) {
  while (true) {
    auto accepted = co_await listener_accept(*listener);
    if (allowed(*filter, accepted.peer)) co_return kj::mv(accepted);
  }
}

}  // namespace

// =======================================================================================
// TokioAsyncIoStream

kj::Promise<size_t> TokioAsyncIoStream::tryRead(void *buffer, size_t minBytes, size_t maxBytes) {
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
  // The coroutine frame owns the KjPieces the Rust side reads through (bridge.h) for as long as
  // the write is pending; the piece buffers themselves are the caller's, per KJ's contract.
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

void TokioAsyncIoStream::getsockname(struct sockaddr *addr, kj::uint *length) {
  copyOut(encodeSockaddr(stream_local_addr(*inner)), addr, length);
}

void TokioAsyncIoStream::getpeername(struct sockaddr *addr, kj::uint *length) {
  copyOut(encodeSockaddr(stream_peer_addr(*inner)), addr, length);
}

kj::Maybe<int> TokioAsyncIoStream::getFd() const {
#if _WIN32
  return kj::none;
#else
  return static_cast<int>(stream_raw_handle(*inner));
#endif
}

#if _WIN32
kj::Maybe<void *> TokioAsyncIoStream::getWin32Handle() const {
  return reinterpret_cast<void *>(static_cast<uintptr_t>(stream_raw_handle(*inner)));
}
#endif

// =======================================================================================
// TokioConnectionReceiver

kj::Promise<kj::Own<kj::AsyncIoStream>> TokioConnectionReceiver::accept() {
  return acceptImpl(false).then(
      [](kj::AuthenticatedStream authenticated) { return kj::mv(authenticated.stream); });
}

kj::Promise<kj::AuthenticatedStream> TokioConnectionReceiver::acceptAuthenticated() {
  return acceptImpl(true);
}

kj::Promise<kj::AuthenticatedStream> TokioConnectionReceiver::acceptImpl(bool authenticated) {
  auto identityFilter = filter.addRef();
  return acceptAllowed(listener_clone(*inner), filter.addRef())
      .then([authenticated, identityFilter = kj::mv(identityFilter)](
                PeerStream accepted) mutable -> kj::AuthenticatedStream {
    kj::AuthenticatedStream result;
    result.peerIdentity = authenticated ? peerIdentity(accepted, kj::mv(identityFilter))
                                        : kj::UnknownPeerIdentity::newInstance();
    result.stream = kj::heap<TokioAsyncIoStream>(kj::mv(accepted.stream));
    return kj::mv(result);
  });
}

kj::uint TokioConnectionReceiver::getPort() {
  return listener_port(*inner);
}

void TokioConnectionReceiver::getsockname(struct sockaddr *addr, kj::uint *length) {
  copyOut(encodeSockaddr(listener_local_addr(*inner)), addr, length);
}

// =======================================================================================
// TokioNetworkAddress

kj::Promise<kj::Own<kj::AsyncIoStream>> TokioNetworkAddress::connect() {
  return connectAny(address_clone(*inner), address_targets(*inner), filter.addRef())
      .then([](PeerStream connected) -> kj::Own<kj::AsyncIoStream> {
    return kj::heap<TokioAsyncIoStream>(kj::mv(connected.stream));
  });
}

kj::Promise<kj::AuthenticatedStream> TokioNetworkAddress::connectAuthenticated() {
  return connectAny(address_clone(*inner), address_targets(*inner), filter.addRef())
      .then([identityFilter = filter.addRef()](
                PeerStream connected) mutable -> kj::AuthenticatedStream {
    kj::AuthenticatedStream result;
    result.peerIdentity = peerIdentity(connected, kj::mv(identityFilter));
    result.stream = kj::heap<TokioAsyncIoStream>(kj::mv(connected.stream));
    return kj::mv(result);
  });
}

kj::Own<kj::ConnectionReceiver> TokioNetworkAddress::listen() {
  return kj::heap<TokioConnectionReceiver>(address_listen(*inner), filter.addRef());
}

kj::Own<kj::DatagramPort> TokioNetworkAddress::bindDatagramPort() {
  return kj::heap<TokioDatagramPort>(address_bind_datagram(*inner), filter.addRef());
}

kj::Own<kj::NetworkAddress> TokioNetworkAddress::clone() {
  return kj::heap<TokioNetworkAddress>(address_clone(*inner), filter.addRef());
}

kj::String TokioNetworkAddress::toString() {
  auto text = address_to_string(*inner);
  return kj::heapString(reinterpret_cast<const char *>(text.data()), text.size());
}

// =======================================================================================
// TokioDatagramPort

class TokioDatagramPort::Receiver final: public kj::DatagramReceiver {
 public:
  Receiver(TokioDatagramPort &port, Capacity capacity): port(port), capacity(capacity) {}

  kj::Promise<void> receive() override {
    for (;;) {
      auto received = co_await datagram_receive(*port.inner, capacity.content);
      if (!allowed(*port.filter, received.source)) continue;

      source = kj::heap<TokioNetworkAddress>(
          network_address_from(received.source), port.filter.addRef());
      current = kj::mv(received);
      co_return;
    }
  }

  MaybeTruncated<kj::ArrayPtr<const kj::byte>> getContent() override {
    auto &received = KJ_REQUIRE_NONNULL(current, "Haven't received a datagram yet.");
    return {
      kj::arrayPtr(reinterpret_cast<const kj::byte *>(received.data.data()), received.data.size()),
      received.truncated};
  }

  MaybeTruncated<kj::ArrayPtr<const kj::AncillaryMessage>> getAncillary() override {
    return {nullptr, false};
  }

  kj::NetworkAddress &getSource() override {
    return *KJ_REQUIRE_NONNULL(source, "Haven't received a datagram yet.");
  }

 private:
  TokioDatagramPort &port;
  Capacity capacity;
  kj::Maybe<ReceivedDatagram> current;
  kj::Maybe<kj::Own<TokioNetworkAddress>> source;
};

kj::Promise<size_t> TokioDatagramPort::send(
    kj::ArrayPtr<const kj::byte> buffer, kj::NetworkAddress &destination) {
  auto targets = address_targets(kj::downcast<TokioNetworkAddress>(destination).getInner());
  KJ_REQUIRE(targets.size() > 0, "send() destination has no addresses");
  KJ_REQUIRE(allowed(*filter, targets[0]), "send() blocked by restrictPeers()");
  return started(datagram_send(*inner,
      ::rust::Slice<const uint8_t>(
          reinterpret_cast<const uint8_t *>(buffer.begin()), buffer.size()),
      kj::mv(targets[0])));
}

kj::Promise<size_t> TokioDatagramPort::send(
    kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces, kj::NetworkAddress &destination) {
  size_t size = 0;
  for (const auto &piece: pieces) size += piece.size();
  auto buffer = kj::heapArray<kj::byte>(size);
  auto pos = buffer.begin();
  for (auto piece: pieces) {
    memcpy(pos, piece.begin(), piece.size());
    pos += piece.size();
  }
  return send(buffer, destination).attach(kj::mv(buffer));
}

kj::Own<kj::DatagramReceiver> TokioDatagramPort::makeReceiver(
    kj::DatagramReceiver::Capacity capacity) {
  KJ_REQUIRE(capacity.ancillary == 0, "Ancillary datagram messages are not implemented");
  return kj::heap<Receiver>(*this, capacity);
}

kj::uint TokioDatagramPort::getPort() {
  return datagram_port(*inner);
}

// =======================================================================================
// TokioNetwork

kj::Promise<kj::Own<kj::NetworkAddress>> TokioNetwork::parseAddress(
    kj::StringPtr addr, kj::uint portHint) {
  KJ_REQUIRE(portHint < 65536, "port hint too large", portHint);
  return started(
      network_parse_address(::rust::Slice<const uint8_t>(
                                reinterpret_cast<const uint8_t *>(addr.begin()), addr.size()),
          static_cast<uint16_t>(portHint), *loopback)
          .then([filter = filter.addRef()](
                    ::rust::Box<TokioAddress> address) mutable -> kj::Own<kj::NetworkAddress> {
    return kj::heap<TokioNetworkAddress>(kj::mv(address), kj::mv(filter));
  }));
}

kj::Own<kj::NetworkAddress> TokioNetwork::getSockaddr(const void *sockaddr, kj::uint len) {
  return kj::heap<TokioNetworkAddress>(
      network_address_from(decodeSockaddr(sockaddr, len)), filter.addRef());
}

kj::Own<kj::Network> TokioNetwork::restrictPeers(
    kj::ArrayPtr<const kj::StringPtr> allow, kj::ArrayPtr<const kj::StringPtr> deny) {
  return kj::heap<TokioNetwork>(*this, allow, deny);
}

// =======================================================================================
// TokioLowLevelAsyncIoProvider

namespace {
// KJ's flag values cross the bridge verbatim (ffi.rs applies them).
static_assert(kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP == 1 << 0);
#if !_WIN32
static_assert(kj::LowLevelAsyncIoProvider::ALREADY_CLOEXEC == 1 << 1);
static_assert(kj::LowLevelAsyncIoProvider::ALREADY_NONBLOCK == 1 << 2);
#endif  // !_WIN32

// A raw handle must gain an owner before any early return: KJ's owning wrap*Fd overloads release
// their RAII owner before the virtual call and pass TAKE_OWNERSHIP, so throwing before Rust
// adopts the handle would leak it. The UNIMPLEMENTED stubs below close a transferred handle
// *before* they throw -- a plain call, not a scope guard, so nothing runs during unwinding.
void closeTransferredFd(kj::LowLevelAsyncIoProvider::Fd fd, kj::uint flags) {
  if (flags & kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP) {
#if _WIN32
    closesocket(static_cast<SOCKET>(fd));  // a SOCKET, not a HANDLE: closesocket, not CloseHandle
#else
    kj::OwnFd owned(fd);
#endif
  }
}

}  // namespace

// wrapInputFd / wrapOutputFd take sockets, as kj's win32 provider defines them: workerd hands
// the provider no pipe or character-device descriptors, and a unix pipe tier is exactly the kind
// of hand-written fd code this crate leaves out (lib.rs, "Scope").
kj::Own<kj::AsyncInputStream> TokioLowLevelAsyncIoProvider::wrapInputFd(Fd fd, kj::uint flags) {
  return kj::heap<TokioAsyncIoStream>(wrap_socket_fd(static_cast<int64_t>(fd), flags));
}

kj::Own<kj::AsyncOutputStream> TokioLowLevelAsyncIoProvider::wrapOutputFd(Fd fd, kj::uint flags) {
  return kj::heap<TokioAsyncIoStream>(wrap_socket_fd(static_cast<int64_t>(fd), flags));
}

kj::Own<kj::AsyncIoStream> TokioLowLevelAsyncIoProvider::wrapSocketFd(Fd fd, kj::uint flags) {
  return kj::heap<TokioAsyncIoStream>(wrap_socket_fd(static_cast<int64_t>(fd), flags));
}

// No workerd caller (it connects through kj::Network), and the one KJ operation that would need
// a hand-written non-blocking connect(2) + SO_ERROR sequence over a foreign descriptor.
kj::Promise<kj::Own<kj::AsyncIoStream>> TokioLowLevelAsyncIoProvider::wrapConnectingSocketFd(
    Fd fd, const struct sockaddr *, kj::uint, kj::uint flags) {
  closeTransferredFd(fd, flags);
  KJ_UNIMPLEMENTED("wrapConnectingSocketFd is not implemented by the tokio backend; connect "
                   "through kj::Network instead");
}

kj::Own<kj::ConnectionReceiver> TokioLowLevelAsyncIoProvider::wrapListenSocketFd(
    Fd fd, NetworkFilter &filter, kj::uint flags) {
  // KJ's interface lends the filter by reference for the receiver's lifetime. workerd's only
  // call (inherited listen sockets, server/workerd.c++) uses the two-argument overload, whose
  // filter is KJ's static allow-all; that one is recognised by identity and given an owned
  // allow-all filter. Anything else would need a borrowed reference to outlive its owner by
  // contract alone, which this backend does not do.
  if (&filter != &NetworkFilter::getAllAllowed()) {
    closeTransferredFd(fd, flags);
    KJ_UNIMPLEMENTED("wrapListenSocketFd with a caller-owned NetworkFilter is not implemented by "
                     "the tokio backend (no workerd caller); use the two-argument overload");
  }
  // Rust adopts the descriptor synchronously, including on its error paths (ffi.rs).
  return kj::heap<TokioConnectionReceiver>(
      wrap_listen_fd(static_cast<int64_t>(fd), flags), kj::arc<PeerFilter>());
}

// =======================================================================================
// TokioAsyncIoProvider

// Both pipes are socket pairs: kj's in-memory pipes make a small write wait for a reader, and
// workerd's loopback transport expects real sockets from the provider (see async-io.h).
kj::OneWayPipe TokioAsyncIoProvider::newOneWayPipe() {
  auto pair = new_socket_pair();
  return kj::OneWayPipe{kj::heap<TokioAsyncIoStream>(kj::mv(pair.first)),
    kj::heap<TokioAsyncIoStream>(kj::mv(pair.second))};
}

kj::TwoWayPipe TokioAsyncIoProvider::newTwoWayPipe() {
  auto pair = new_socket_pair();
  return kj::TwoWayPipe{{kj::heap<TokioAsyncIoStream>(kj::mv(pair.first)),
    kj::heap<TokioAsyncIoStream>(kj::mv(pair.second))}};
}

kj::AsyncIoProvider::PipeThread TokioAsyncIoProvider::newPipeThread(
    kj::Function<void(kj::AsyncIoProvider &, kj::AsyncIoStream &, kj::WaitScope &)> startFunc) {
  KJ_UNIMPLEMENTED("kj-rs-io does not implement newPipeThread() (workerd does not use it)");
}

// =======================================================================================
// Context

TokioAsyncIoContext::TokioAsyncIoContext()
    : base(kj_rs_tokio::setupTokioAsyncIo()),
      lowLevelProvider(base.getTimer()),
      provider(base.getTimer()) {
#if !_WIN32
  ignore_sigpipe_once();
#endif
}

TokioAsyncIoContext setupTokioAsyncIo() {
  return TokioAsyncIoContext();
}

kj::Promise<void> onSignal(int signum) {
  return started(wait_for_signal(signum));
}

// =======================================================================================
// FileWatcher

void FileWatcher::watch(kj::PathPtr path) {
  auto native = path.toNativeString(true);
  file_watcher_watch(*inner,
      ::rust::Slice<const uint8_t>(
          reinterpret_cast<const uint8_t *>(native.begin()), native.size()));
}

kj::Promise<void> FileWatcher::onChange() {
  return started(file_watcher_on_change(*inner));
}

}  // namespace kj_rs_io
