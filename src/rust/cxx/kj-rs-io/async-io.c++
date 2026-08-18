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

}  // namespace kj_rs_io
