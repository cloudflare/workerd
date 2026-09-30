// Tests for kj-rs-io: tokio-backed implementations of KJ's async I/O interfaces, driven by a
// kj::EventLoop on a TokioEventPort. Following kj-rs conventions, C++ KJ_TESTs drive; Rust
// helpers (tests/lib.rs) provide pre-bound fds and deterministic multi-address lists.

#include "io-test-helpers.h"
#include "kj-rs-io-test/lib.rs.h"
#include "kj-rs-io/async-io.h"

#include <kj/array.h>
#include <kj/async.h>
#include <kj/debug.h>
#include <kj/io.h>
#include <kj/test.h>
#include <kj/thread.h>

#include <cstring>
#include <type_traits>

#if _WIN32
#include <windows.h>  // Win32 APIs used by the Windows-only test arms below.

// After windows.h: un-breaks macros it leaks over KJ's, notably ERROR (which otherwise breaks
// the KJ_LOG(ERROR, ...) inside KJ_FAIL_* expansions).
#include <kj/windows-sanity.h>
#else
#include <fcntl.h>
#include <netdb.h>  // getservbyname()
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <climits>
#include <cstdlib>
#if __APPLE__
#include <mach-o/dyld.h>  // _NSGetExecutablePath, for the re-exec'd SIGPIPE child
#endif
#endif

namespace kj_rs_io_test {
namespace {

using kj_rs_io::setupTokioAsyncIo;
using kj_rs_io::TokioAsyncIoContext;

static_assert(!std::is_move_constructible_v<TokioAsyncIoContext>);

// =======================================================================================
// Helpers (shared ones: io-test-helpers.h)

// Waits for `connectPromise` (a connect to a certainly-closed port) to fail and returns the
// exception, bounded by a KJ timer so a never-settling connect fails the test with a message
// instead of eating the binary's bazel timeout. (An earlier version of this helper carried a
// watchdog thread and CPU accounting to diagnose a Windows CI wedge: a lost connect-readiness
// wake caused by the then single-threaded waker bridge. That bridge is thread-safe now and the
// wedge is gone with it; the timer bound is kept as a plain test hygiene measure.)
// The exception a connect() promise fails with. A connect that neither succeeds nor fails within
// 30 s fails the test (the timeout is not itself the "failure" -- the outcome is captured as a
// value, so only the connect's own rejection can be returned).
kj::Exception expectConnectFailure(
    TokioAsyncIoContext &io, kj::Promise<kj::Own<kj::AsyncIoStream>> connectPromise) {
  auto outcome = connectPromise.then([](kj::Own<kj::AsyncIoStream>) -> kj::Maybe<kj::Exception> {
    return kj::none;
  }, [](kj::Exception &&e) -> kj::Maybe<kj::Exception> { return kj::mv(e); });
  auto timeout = io.getTimer().afterDelay(30 * kj::SECONDS).then([]() -> kj::Maybe<kj::Exception> {
    KJ_FAIL_ASSERT("connect() neither succeeded nor failed within 30s");
  });
  return KJ_ASSERT_NONNULL(outcome.exclusiveJoin(kj::mv(timeout)).wait(io.getWaitScope()),
      "connect() unexpectedly succeeded");
}

// =======================================================================================
// Stream contract

KJ_TEST("tryRead waits for minBytes, then returns what is available up to "
        "maxBytes") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto pair = makeTcpPair(io);

  kj::byte buffer[16];

  // Exactly-min: 3 bytes written, min 3 -> resolves with 3.
  pair.client->write("abc"_kjb).wait(ws);
  KJ_EXPECT(pair.server->tryRead(buffer, 3, sizeof(buffer)).wait(ws) == 3);
  KJ_EXPECT(kj::ArrayPtr<kj::byte>(buffer, 3) == "abc"_kjb);

  // Blocks until minBytes: 2 available < min 5 -> pending; 3 more arrive -> resolves with 5.
  pair.client->write("de"_kjb).wait(ws);
  auto readPromise = pair.server->tryRead(buffer, 5, sizeof(buffer));
  KJ_EXPECT(!readPromise.poll(ws));
  pair.client->write("fgh"_kjb).wait(ws);
  KJ_EXPECT(readPromise.wait(ws) == 5);
  KJ_EXPECT(kj::ArrayPtr<kj::byte>(buffer, 5) == "defgh"_kjb);
}

KJ_TEST("EOF before minBytes returns a short count; half-close keeps the other "
        "direction usable") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto pair = makeTcpPair(io);

  pair.client->write("ab"_kjb).wait(ws);
  pair.client->shutdownWrite();

  // EOF-before-min: only 2 bytes then FIN -> tryRead(min 5) resolves with 2.
  kj::byte buffer[16];
  KJ_EXPECT(pair.server->tryRead(buffer, 5, sizeof(buffer)).wait(ws) == 2);
  KJ_EXPECT(kj::ArrayPtr<kj::byte>(buffer, 2) == "ab"_kjb);
  // Subsequent reads keep reporting EOF.
  KJ_EXPECT(pair.server->tryRead(buffer, 1, sizeof(buffer)).wait(ws) == 0);

  // Half-close: server -> client direction still works after client's shutdownWrite.
  pair.server->write("reply"_kjb).wait(ws);
  KJ_EXPECT(pair.client->tryRead(buffer, 5, sizeof(buffer)).wait(ws) == 5);
  KJ_EXPECT(kj::ArrayPtr<kj::byte>(buffer, 5) == "reply"_kjb);
}

KJ_TEST("multi-megabyte transfers in both directions with concurrent read+write "
        "per stream") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto pair = makeTcpPair(io);

  constexpr size_t SIZE = 8 * 1024 * 1024;
  auto dataA = makePatternedData(SIZE, 1);
  auto dataB = makePatternedData(SIZE, 2);

  // All four directions at once: each stream is simultaneously reading and writing, and each
  // transfer is far larger than the socket buffers (forcing many readiness round-trips).
  auto builder = kj::heapArrayBuilder<kj::Promise<void>>(4);
  builder.add(writeChunked(*pair.client, dataA));
  builder.add(readExact(*pair.server, dataA));
  builder.add(writeChunked(*pair.server, dataB));
  builder.add(readExact(*pair.client, dataB));
  kj::joinPromisesFailFast(builder.finish()).wait(ws);
}

KJ_TEST("multi-piece write() writes all pieces in order") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto pair = makeTcpPair(io);

  const kj::ArrayPtr<const kj::byte> pieces[] = {"one,"_kjb, "two,"_kjb, "three"_kjb};
  pair.client->write(kj::arrayPtr(pieces, 3)).wait(ws);

  kj::byte buffer[32];
  KJ_EXPECT(pair.server->tryRead(buffer, 13, sizeof(buffer)).wait(ws) == 13);
  KJ_EXPECT(kj::ArrayPtr<kj::byte>(buffer, 13) == "one,two,three"_kjb);
}

KJ_TEST("canceling a blocked read releases the socket for reuse") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto pair = makeTcpPair(io);

  kj::byte buffer[16];
  {
    // A read blocked in tokio (registered with the I/O driver, no data available)...
    auto blocked = pair.server->tryRead(buffer, 1, sizeof(buffer));
    KJ_EXPECT(!blocked.poll(ws));
    // ...is canceled by dropping the promise, which must drop the Rust future and release the
    // read interest.
  }

  // The stream remains fully usable: a fresh read gets the next bytes.
  pair.client->write("later"_kjb).wait(ws);
  KJ_EXPECT(pair.server->tryRead(buffer, 5, sizeof(buffer)).wait(ws) == 5);
  KJ_EXPECT(kj::ArrayPtr<kj::byte>(buffer, 5) == "later"_kjb);

#if !_WIN32
  // (unix only: winsock's loopback path buffers a 16 MiB write outright, so nothing is pending
  // to cancel there.)
  // Canceling a write that is blocked on backpressure: the write is really pending (a peer
  // that has not read), the cancellation drops it mid-stream (bytes may be lost, like KJ), and
  // the socket remains usable -- a marker written afterwards arrives, in order, after whatever
  // prefix of the cancelled write made it out.
  {
    auto data = makePatternedData(16 * 1024 * 1024, 7);
    auto bigWrite = pair.client->write(data);
    KJ_EXPECT(!bigWrite.poll(ws), "a 16 MiB write to a non-reading peer must be pending");
  }
  // The peer reads from here on. It has to: the pending write filled the send buffer (on Linux,
  // a loopback write that returned EAGAIN has filled both sockets' buffers exactly), so the
  // marker can only leave once the peer drains, and that drain must run while the marker's
  // promise is awaited.
  auto drainAll = [](kj::AsyncIoStream &s) -> kj::Promise<kj::Array<kj::byte>> {
    kj::Vector<kj::byte> all;
    auto chunk = kj::heapArray<kj::byte>(256 * 1024);
    while (true) {
      size_t n = co_await s.tryRead(chunk.begin(), 1, chunk.size());
      if (n == 0) break;
      all.addAll(chunk.slice(0, n));
    }
    co_return all.releaseAsArray();
  };
  auto draining = drainAll(*pair.server);
  pair.client->write("marker"_kjb).wait(ws);
  pair.client->shutdownWrite();
  auto drained = draining.wait(ws);
  KJ_ASSERT(drained.size() >= 6);
  KJ_EXPECT(drained.slice(drained.size() - 6, drained.size()) == "marker"_kjb);
  // Every byte before the marker is a prefix of the cancelled write, in order.
  auto prefix = drained.slice(0, drained.size() - 6);
  auto expected = makePatternedData(prefix.size(), 7);
  KJ_EXPECT(prefix == expected.asPtr());
#endif
}

#if !_WIN32
KJ_TEST("whenWriteDisconnected resolves on peer reset, not on half-close") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto pair = makeTcpPair(io);

  auto disconnected = pair.client->whenWriteDisconnected();
  KJ_EXPECT(!disconnected.poll(ws));

  // A peer half-close (FIN) must NOT count as write-disconnect: the client can still write.
  pair.server->shutdownWrite();
  kj::byte buffer[16];
  KJ_EXPECT(pair.client->tryRead(buffer, 1, sizeof(buffer)).wait(ws) == 0);  // observe EOF
  KJ_EXPECT(!disconnected.poll(ws));

  // Destroying the server end with SO_LINGER=0 sends an RST; now writes are doomed.
  struct linger lin;
  lin.l_onoff = 1;
  lin.l_linger = 0;
  setRawSockopt(*pair.server, SOL_SOCKET, SO_LINGER, &lin, sizeof(lin));
  pair.server = nullptr;

  disconnected.wait(ws);
}
#endif

KJ_TEST("acceptAuthenticated reports the TCP peer's NetworkPeerIdentity") {
  // workerd's HTTP listener builds the cf blob's clientIp (-> the CF-Connecting-IP header) from
  // this identity; UnknownPeerIdentity (kj's base-class default) silently yields an empty
  // client IP.
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  auto listener = parseNow(io, "127.0.0.1")->listen();
  auto acceptPromise = listener->acceptAuthenticated();
  auto client = parseNow(io, kj::str("127.0.0.1:", listener->getPort()))->connect().wait(ws);
  auto server = acceptPromise.wait(ws);

  auto &identity =
      KJ_ASSERT_NONNULL(kj::tryDowncast<kj::NetworkPeerIdentity>(*server.peerIdentity));
  // KJ's "ip:port" format, byte-identical to the native backend.
  auto text = identity.toString();
  KJ_EXPECT(text.startsWith("127.0.0.1:"), text);
}

#if !_WIN32
KJ_TEST("acceptAuthenticated reports LocalPeerIdentity credentials on unix sockets") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  // /tmp rather than TEST_TMPDIR: sun_path is limited to ~104 bytes.
  auto path = kj::str("/tmp/kj-rs-io-auth-test-", getpid(), ".sock");
  auto addr = parseNow(io, kj::str("unix:", path));

  auto listener = addr->listen();
  auto acceptPromise = listener->acceptAuthenticated();
  auto client = addr->connect().wait(ws);
  auto server = acceptPromise.wait(ws);

  auto &identity = KJ_ASSERT_NONNULL(kj::tryDowncast<kj::LocalPeerIdentity>(*server.peerIdentity));
  auto creds = identity.getCredentials();
  // The peer is this very process.
  KJ_EXPECT(KJ_ASSERT_NONNULL(creds.pid) == getpid());
  KJ_EXPECT(KJ_ASSERT_NONNULL(creds.uid) == getuid());

  unlink(path.cStr());
}
#endif

KJ_TEST("sockname/peername/raw handle passthrough") {
  auto io = setupTokioAsyncIo();
  auto pair = makeTcpPair(io);

  // The raw handle is populated and is the connected socket: its getsockname() agrees with the
  // stream's.
#if _WIN32
  auto rawHandle = reinterpret_cast<SOCKET>(KJ_ASSERT_NONNULL(pair.client->getWin32Handle()));
  KJ_EXPECT(rawHandle != INVALID_SOCKET);
#else
  int rawHandle = KJ_ASSERT_NONNULL(pair.client->getFd());
  KJ_EXPECT(rawHandle >= 0);
#endif
  {
    struct sockaddr_in viaHandle, viaStream;
#if _WIN32
    int viaHandleLen = sizeof(viaHandle);
#else
    socklen_t viaHandleLen = sizeof(viaHandle);
#endif
    kj::uint viaStreamLen = sizeof(viaStream);
    KJ_EXPECT(getsockname(
                  rawHandle, reinterpret_cast<struct sockaddr *>(&viaHandle), &viaHandleLen) == 0);
    pair.client->getsockname(reinterpret_cast<struct sockaddr *>(&viaStream), &viaStreamLen);
    KJ_EXPECT(viaHandle.sin_port == viaStream.sin_port);
  }

  // The client's peer is the server's local socket.
  struct sockaddr_in peer, local;
  kj::uint peerLen = sizeof(peer), localLen = sizeof(local);
  pair.client->getpeername(reinterpret_cast<struct sockaddr *>(&peer), &peerLen);
  pair.server->getsockname(reinterpret_cast<struct sockaddr *>(&local), &localLen);
  KJ_EXPECT(peer.sin_port == local.sin_port);
  KJ_EXPECT(peer.sin_addr.s_addr == local.sin_addr.s_addr);

  // KJ parity: both the connect and the accept path set TCP_NODELAY.
  KJ_EXPECT(getRawSockoptInt(*pair.client, IPPROTO_TCP, TCP_NODELAY) != 0);
  KJ_EXPECT(getRawSockoptInt(*pair.server, IPPROTO_TCP, TCP_NODELAY) != 0);
}

// =======================================================================================
// Network / addresses

KJ_TEST("parseAddress handles IP literals, port hints, and toString round-trips") {
  auto io = setupTokioAsyncIo();

  KJ_EXPECT(parseNow(io, "1.2.3.4:80")->toString() == "1.2.3.4:80");
  KJ_EXPECT(parseNow(io, "1.2.3.4", 99)->toString() == "1.2.3.4:99");
  KJ_EXPECT(parseNow(io, "[1234:5678::abcd]:80")->toString() == "[1234:5678::abcd]:80");
  KJ_EXPECT(parseNow(io, "1234:5678::abcd", 80)->toString() == "[1234:5678::abcd]:80");
  KJ_EXPECT(parseNow(io, "*:80")->toString() == "*:80");
  KJ_EXPECT(parseNow(io, "*")->toString() == "*:0");

  // clone() produces an equivalent address.
  auto addr = parseNow(io, "127.0.0.1:1234");
  KJ_EXPECT(addr->clone()->toString() == addr->toString());
}

KJ_TEST("wildcard listen binds dual-stack and reports its port; port 0 picks a "
        "free port") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  auto listener = parseNow(io, "*:0")->listen();
  kj::uint port = listener->getPort();
  KJ_EXPECT(port != 0);

  // Reachable over both IPv4 and IPv6 loopback (IPV6_V6ONLY off, like KJ).
  kj::String addrTexts[] = {kj::str("127.0.0.1:", port), kj::str("[::1]:", port)};
  for (auto &addrText: addrTexts) {
    auto acceptPromise = listener->accept();
    auto client = parseNow(io, addrText)->connect().wait(ws);
    auto server = acceptPromise.wait(ws);
    client->write("ping"_kjb).wait(ws);
    kj::byte buffer[4];
    KJ_EXPECT(server->tryRead(buffer, 4, sizeof(buffer)).wait(ws) == 4);
  }
}

KJ_TEST("datagram ports send and receive") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  auto sender = parseNow(io, "127.0.0.1:0")->bindDatagramPort();
  auto receiver = parseNow(io, "127.0.0.1:0")->bindDatagramPort();
  auto destination = parseNow(io, kj::str("127.0.0.1:", receiver->getPort()));
  auto source = kj::str("127.0.0.1:", sender->getPort());
  auto incoming = receiver->makeReceiver({.content = 5});

  auto receive = incoming->receive();
  KJ_EXPECT(sender->send("hello"_kjb, *destination).wait(ws) == 5);
  receive.wait(ws);
  auto content = incoming->getContent();
  KJ_EXPECT(content.value == "hello"_kjb);
  KJ_EXPECT(!content.isTruncated);
  KJ_EXPECT(incoming->getSource().toString() == source);

  receive = incoming->receive();
  KJ_EXPECT(sender->send(kj::ArrayPtr<const kj::byte>(nullptr), *destination).wait(ws) == 0);
  receive.wait(ws);
  content = incoming->getContent();
  KJ_EXPECT(content.value.size() == 0);
  KJ_EXPECT(!content.isTruncated);

  auto fullIncoming = receiver->makeReceiver({.content = 4});
  receive = fullIncoming->receive();
  const kj::ArrayPtr<const kj::byte> pieces[] = {"ab"_kjb, "cd"_kjb};
  KJ_EXPECT(sender->send(kj::arrayPtr(pieces), *destination).wait(ws) == 4);
  receive.wait(ws);
  KJ_EXPECT(fullIncoming->getContent().value == "abcd"_kjb);
}

#if !_WIN32
KJ_TEST("datagram ports report truncation") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  auto sender = parseNow(io, "127.0.0.1:0")->bindDatagramPort();
  auto receiver = parseNow(io, "127.0.0.1:0")->bindDatagramPort();
  auto destination = parseNow(io, kj::str("127.0.0.1:", receiver->getPort()));
  auto incoming = receiver->makeReceiver({.content = 3});

  auto receive = incoming->receive();
  KJ_EXPECT(sender->send("hello"_kjb, *destination).wait(ws) == 5);
  receive.wait(ws);
  auto content = incoming->getContent();
  KJ_EXPECT(content.value == "hel"_kjb);
  KJ_EXPECT(content.isTruncated);
}
#endif

KJ_TEST("parseAddress resolves hostnames via DNS") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  // Listen on IPv4 loopback only. "localhost" resolves to 127.0.0.1 (and, on most systems, ::1
  // as well, which connect()'s per-address fallback -- tested deterministically below -- skips).
  auto listener = parseNow(io, "127.0.0.1")->listen();
  auto addr = parseNow(io, kj::str("localhost:", listener->getPort()));

  auto acceptPromise = listener->accept();
  auto client = addr->connect().wait(ws);
  auto server = acceptPromise.wait(ws);
  client->write("dns!"_kjb).wait(ws);
  kj::byte buffer[4];
  KJ_EXPECT(server->tryRead(buffer, 4, sizeof(buffer)).wait(ws) == 4);
  KJ_EXPECT(kj::ArrayPtr<kj::byte>(buffer, 4) == "dns!"_kjb);
}

KJ_TEST("connect() tries each resolved address in order, falling through refused ones") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  // A port that is certainly closed: bind, note the port, close.
  kj::uint closedPort = parseNow(io, "127.0.0.1")->listen()->getPort();
  auto listener = parseNow(io, "127.0.0.1")->listen();

  // Two addresses, the first refused: a deterministic multi-result "DNS" answer.
  uint16_t ports[] = {
    static_cast<uint16_t>(closedPort), static_cast<uint16_t>(listener->getPort())};
  auto addr = kj::heap<kj_rs_io::TokioNetworkAddress>(
      address_from_loopback_ports(::rust::Slice<const uint16_t>(ports, kj::size(ports))),
      kj::arc<kj_rs_io::PeerFilter>());
  auto acceptPromise = listener->accept();
  auto client = addr->connect().wait(ws);
  auto server = acceptPromise.wait(ws);
  client->write("2nd"_kjb).wait(ws);
  kj::byte buffer[3];
  KJ_EXPECT(server->tryRead(buffer, 3, 3).wait(ws) == 3);

  // All refused: the LAST address's exception propagates (KJ parity).
  kj::uint closedPort2 = parseNow(io, "127.0.0.1")->listen()->getPort();
  uint16_t closedPorts[] = {static_cast<uint16_t>(closedPort), static_cast<uint16_t>(closedPort2)};
  auto allClosed = kj::heap<kj_rs_io::TokioNetworkAddress>(
      address_from_loopback_ports(
          ::rust::Slice<const uint16_t>(closedPorts, kj::size(closedPorts))),
      kj::arc<kj_rs_io::PeerFilter>());
  auto exception = expectConnectFailure(io, allClosed->connect());
  KJ_EXPECT(exception.getType() == kj::Exception::Type::DISCONNECTED, exception);
  KJ_EXPECT(exception.getDescription().contains("connect()"), exception.getDescription());
}

KJ_TEST("connect to a closed port surfaces a DISCONNECTED kj::Exception "
        "mentioning the refusal") {
  auto io = setupTokioAsyncIo();

  // Find a port that is certainly closed: bind one, note it, close it.
  kj::uint port;
  {
    auto listener = parseNow(io, "127.0.0.1")->listen();
    port = listener->getPort();
  }

  auto addr = parseNow(io, kj::str("127.0.0.1:", port));
  auto exception = expectConnectFailure(io, addr->connect());
  // Exact text (recorded): "connect(): Connection refused (os error 61)" on macOS /
  // "... (os error 111)" on Linux. KJ's native text would be "connect(): Connection refused".
  KJ_EXPECT(exception.getDescription().contains("refused"), exception.getDescription());
  KJ_EXPECT(exception.getType() == kj::Exception::Type::DISCONNECTED);
}

KJ_TEST("the address may be dropped while connect() is pending (KJ lifetime "
        "contract)") {
  // Upstream KJ heap-copies the resolved address list into the connect promise
  // (NetworkAddressImpl::connect() in kj/async-io-unix.c++), so callers may legally drop
  // the kj::NetworkAddress right after calling connect(). Verify this port honors the same
  // contract, on both the success path and the error/retry path.
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  // Success path: drop the address immediately, then complete the connect.
  {
    auto listener = parseNow(io, "127.0.0.1")->listen();
    auto acceptPromise = listener->accept();

    kj::Promise<kj::Own<kj::AsyncIoStream>> connectPromise = nullptr;
    {
      auto addr = parseNow(io, kj::str("127.0.0.1:", listener->getPort()));
      connectPromise = addr->connect();
      // `addr` is destroyed here, while the connect is still in flight.
    }

    auto client = connectPromise.wait(ws);
    auto server = acceptPromise.wait(ws);
    client->write("hello"_kjb).wait(ws);
    kj::byte buffer[5] = {};
    KJ_EXPECT(server->tryRead(buffer, 5, 5).wait(ws) == 5);
    KJ_EXPECT(kj::arrayPtr(buffer, 5) == "hello"_kjb);
  }

  // Error path: connect to a certainly-closed port with the address already dropped; the
  // failure continuation (which re-reads the address list) must still be safe and surface
  // the normal exception.
  {
    kj::uint port;
    {
      auto listener = parseNow(io, "127.0.0.1")->listen();
      port = listener->getPort();
    }

    kj::Promise<kj::Own<kj::AsyncIoStream>> connectPromise = nullptr;
    {
      auto addr = parseNow(io, kj::str("127.0.0.1:", port));
      connectPromise = addr->connect();
    }

    auto exception = expectConnectFailure(io, kj::mv(connectPromise));
    KJ_EXPECT(exception.getDescription().contains("refused"), exception.getDescription());
    KJ_EXPECT(exception.getType() == kj::Exception::Type::DISCONNECTED);
  }
}

KJ_TEST("connecting to a wildcard address is an error") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto addr = parseNow(io, "*:1234");
  KJ_EXPECT_THROW_MESSAGE("wildcard", addr->connect().wait(ws));
}

#if !_WIN32
KJ_TEST("unix domain sockets: parse, listen, connect, toString") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  // Note: /tmp rather than TEST_TMPDIR because sun_path is limited to ~104 bytes.
  auto path = kj::str("/tmp/kj-rs-io-test-", getpid(), ".sock");
  auto addrText = kj::str("unix:", path);

  auto addr = parseNow(io, addrText);
  KJ_EXPECT(addr->toString() == addrText);

  auto listener = addr->listen();
  KJ_EXPECT(listener->getPort() == 0);  // KJ reports 0 for non-IP listeners.
  auto acceptPromise = listener->accept();
  auto client = addr->connect().wait(ws);
  auto server = acceptPromise.wait(ws);

  client->write("via unix"_kjb).wait(ws);
  client->shutdownWrite();
  kj::byte buffer[16];
  KJ_EXPECT(server->tryRead(buffer, 16, sizeof(buffer)).wait(ws) == 8);
  KJ_EXPECT(kj::ArrayPtr<kj::byte>(buffer, 8) == "via unix"_kjb);

  unlink(path.cStr());
}

KJ_TEST("getSockaddr builds a connectable address from a raw struct sockaddr") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  auto listener = parseNow(io, "127.0.0.1")->listen();

  struct sockaddr_in sin;
  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_port = htons(static_cast<uint16_t>(listener->getPort()));
  sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  auto addr = io.getNetwork().getSockaddr(&sin, sizeof(sin));
  KJ_EXPECT(addr->toString() == kj::str("127.0.0.1:", listener->getPort()));

  auto acceptPromise = listener->accept();
  auto client = addr->connect().wait(ws);
  auto server = acceptPromise.wait(ws);
  client->write("hi"_kjb).wait(ws);
  kj::byte buffer[2];
  KJ_EXPECT(server->tryRead(buffer, 2, sizeof(buffer)).wait(ws) == 2);
}
#endif

KJ_TEST("newPipeThread is a documented stub") {
  auto io = setupTokioAsyncIo();
  KJ_EXPECT_THROW_MESSAGE("newPipeThread",
      io.getProvider().newPipeThread(
          [](kj::AsyncIoProvider &, kj::AsyncIoStream &, kj::WaitScope &) {}));
}

// =======================================================================================
// Provider odds and ends

KJ_TEST("provider pipes and timer work under the tokio loop") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  auto pipe = io.getProvider().newTwoWayPipe();
  auto writePromise = pipe.ends[0]->write("pipe data"_kjb).eagerlyEvaluate(nullptr);
  kj::byte buffer[16];
  KJ_EXPECT(pipe.ends[1]->tryRead(buffer, 9, sizeof(buffer)).wait(ws) == 9);
  writePromise.wait(ws);

  auto &timer = io.getProvider().getTimer();
  auto before = timer.now();
  timer.afterDelay(5 * kj::MILLISECONDS).wait(ws);
  KJ_EXPECT(timer.now() - before >= 5 * kj::MILLISECONDS);
}

// =======================================================================================
// Fd wrapping (kj::LowLevelAsyncIoProvider)

#if !_WIN32
KJ_TEST("wrapListenSocketFd accepts connections on a pre-bound listener (the "
        "--socket-fd case)") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  // Rust binds a *blocking* std listener (like an fd inherited from a supervisor) and hands us
  // the raw fd; wrapListenSocketFd must take ownership and make it usable.
  auto prebound = create_prebound_listener_fd();
  auto receiver = io.getLowLevelProvider().wrapListenSocketFd(
      prebound.fd, kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP);
  KJ_EXPECT(receiver->getPort() == prebound.port);

  auto acceptPromise = receiver->accept();
  auto client = parseNow(io, kj::str("127.0.0.1:", prebound.port))->connect().wait(ws);
  auto server = acceptPromise.wait(ws);

  client->write("fd listen"_kjb).wait(ws);
  kj::byte buffer[16];
  KJ_EXPECT(server->tryRead(buffer, 9, sizeof(buffer)).wait(ws) == 9);
  KJ_EXPECT(kj::ArrayPtr<kj::byte>(buffer, 9) == "fd listen"_kjb);
}

KJ_TEST(
    "wrapListenSocketFd with a caller-owned NetworkFilter is an intentional UNIMPLEMENTED stub") {
  // KJ's three-argument overload lends the filter by reference for the receiver's lifetime.
  // workerd's only call is the two-argument (allow-all) overload, tested above; a caller-owned
  // filter is refused up front rather than borrowed on a lifetime promise (async-io.c++).
  auto io = setupTokioAsyncIo();
  struct DenyAll final: public kj::LowLevelAsyncIoProvider::NetworkFilter {
    bool shouldAllow(const struct sockaddr *, kj::uint) override {
      return false;
    }
  } filter;
  auto prebound = create_prebound_listener_fd();
  KJ_EXPECT_THROW_MESSAGE("caller-owned NetworkFilter",
      io.getLowLevelProvider().wrapListenSocketFd(prebound.fd, filter, 0));
  ::close(prebound.fd);
}

KJ_TEST("wrapSocketFd wraps both ends of a socketpair") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  int fds[2];
  KJ_SYSCALL(socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
  auto end0 =
      io.getLowLevelProvider().wrapSocketFd(fds[0], kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP);
  auto end1 =
      io.getLowLevelProvider().wrapSocketFd(fds[1], kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP);

  end0->write("socketpair"_kjb).wait(ws);
  kj::byte buffer[16];
  KJ_EXPECT(end1->tryRead(buffer, 10, sizeof(buffer)).wait(ws) == 10);
  KJ_EXPECT(kj::ArrayPtr<kj::byte>(buffer, 10) == "socketpair"_kjb);
}

KJ_TEST("wrapSocketFd normalizes a borrowed descriptor without consuming it") {
  auto io = setupTokioAsyncIo();

  int fds[2];
  KJ_SYSCALL(socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
  kj::OwnFd borrowed(fds[0]);
  kj::OwnFd peer(fds[1]);

  auto wrapped = io.getLowLevelProvider().wrapSocketFd(borrowed, 0);
  auto wrappedFd = KJ_ASSERT_NONNULL(wrapped->getFd());
  KJ_EXPECT(wrappedFd != borrowed.get());
  int descriptorFlags;
  KJ_SYSCALL(descriptorFlags = fcntl(wrappedFd, F_GETFD));
  KJ_EXPECT((descriptorFlags & FD_CLOEXEC) != 0);
  int statusFlags;
  KJ_SYSCALL(statusFlags = fcntl(wrappedFd, F_GETFL));
  KJ_EXPECT((statusFlags & O_NONBLOCK) != 0);
  KJ_SYSCALL(fcntl(borrowed, F_GETFD));
}

KJ_TEST("restrictPeers blocks disallowed connect() with KJ's error text") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  kj_rs_io::TokioNetwork network;
  auto restricted = network.restrictPeers({"public"_kj}, {});

  // Loopback is not "public": blocked before any connection attempt.
  auto blockedAddr = restricted->parseAddress("127.0.0.1:1").wait(ws);
  KJ_EXPECT_THROW_MESSAGE("connect() blocked by restrictPeers()", blockedAddr->connect().wait(ws));

  // getSockaddr() itself accepts the address; the filter applies when it is used (KJ rejects
  // eagerly there too, a difference only in when the same error surfaces).
  struct sockaddr_in sin;
  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_port = htons(1);
  sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  auto viaSockaddr = restricted->getSockaddr(&sin, sizeof(sin));
  KJ_EXPECT_THROW_MESSAGE("connect() blocked by restrictPeers()", viaSockaddr->connect().wait(ws));

  // An allowing restriction still connects.
  auto allowed = network.restrictPeers({"private"_kj}, {});
  auto listener = network.parseAddress("127.0.0.1", 0).wait(ws)->listen();
  auto acceptPromise = listener->accept();
  auto client = allowed->parseAddress(kj::str("127.0.0.1:", listener->getPort()))
                    .wait(ws)
                    ->connect()
                    .wait(ws);
  auto server = acceptPromise.wait(ws);
  client->write("ok"_kjb).wait(ws);
  kj::byte buffer[2];
  KJ_EXPECT(server->tryRead(buffer, 2, 2).wait(ws) == 2);
}

KJ_TEST("restrictPeers filters accepted peers (disallowed peers are dropped, "
        "accept keeps waiting)") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  kj_rs_io::TokioNetwork network;
  auto restricted = network.restrictPeers({"public"_kj}, {});

  auto listener = restricted->parseAddress("127.0.0.1", 0).wait(ws)->listen();
  auto acceptPromise = listener->accept();

  // Connect via the unrestricted network; the loopback peer is not "public", so the listener
  // silently drops it: accept() stays pending and the client observes EOF.
  auto client = network.parseAddress(kj::str("127.0.0.1:", listener->getPort()), 0)
                    .wait(ws)
                    ->connect()
                    .wait(ws);
  KJ_EXPECT(!acceptPromise.poll(ws));
  kj::byte buffer[1];
  KJ_EXPECT(client->tryRead(buffer, 1, 1).wait(ws) == 0);
}

KJ_TEST("onSignal is delivered even when another runtime's thread consumes the signal") {
  // Tokio's process-global signal registry broadcasts from whichever runtime consumes the
  // signal's wake byte. A second parked runtime (any other tokio runtime in the process with the
  // signal driver enabled) can therefore wake this loop's signal future from another thread.
  // ArcWaker must deliver that wake through its cross-thread fulfiller so this loop observes
  // the signal.
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  // A second, idle tokio-ported KJ loop parked on another thread for the duration of the test.
  // The shutdown promise must be created ON that thread's loop (kj promises are single-loop
  // objects); only its CrossThreadPromiseFulfiller half comes back to this thread.
  kj::MutexGuarded<kj::Maybe<kj::Own<const kj::CrossThreadPromiseFulfiller<void>>>> shutdown;
  kj::Thread otherLoop([&shutdown]() {
    auto io2 = setupTokioAsyncIo();
    auto paf = kj::newPromiseAndCrossThreadFulfiller<void>();
    *shutdown.lockExclusive() = kj::mv(paf.fulfiller);
    paf.promise.wait(io2.getWaitScope());
  });
  auto fulfillShutdown = [&]() {
    KJ_IF_SOME(fulfiller, *shutdown.lockExclusive()) {
      fulfiller->fulfill();
    }
  };
  KJ_DEFER(fulfillShutdown());
  // Wait until the other loop is up and parked (and the shutdown fulfiller exists) before
  // raising any signals, so its runtime genuinely participates in the wake-byte race.
  shutdown.when([](auto &maybe) { return maybe != kj::none; }, [](auto &) {});

  // Several rounds, giving each runtime chances to win the wake-byte race. Bounded so a lost
  // wake fails with a diagnosis instead of eating the binary's bazel timeout.
  for (int i = 0; i < 5; i++) {
    auto promise = kj_rs_io::onSignal(SIGUSR2);
    // The handler was installed inside onSignal(); the poll only proves the promise is pending.
    KJ_EXPECT(!promise.poll(ws));
    KJ_SYSCALL(kill(getpid(), SIGUSR2));
    promise
        .exclusiveJoin(io.getTimer().afterDelay(20 * kj::SECONDS).then([]() {
      KJ_FAIL_ASSERT("onSignal wake was lost (cross-thread waker regression)");
    })).wait(ws);
  }
}

KJ_TEST("onSignal resolves when the process receives the signal") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  auto promise = kj_rs_io::onSignal(SIGUSR2);
  // The handler was installed inside onSignal() (operation-start policy); a signal raised right
  // after the call is caught. The poll only proves the promise is pending.
  KJ_EXPECT(!promise.poll(ws));

  KJ_SYSCALL(kill(getpid(), SIGUSR2));
  promise.wait(ws);

  // A second watcher works too (the process-global registration is reusable).
  auto again = kj_rs_io::onSignal(SIGUSR2);
  KJ_EXPECT(!again.poll(ws));
  KJ_SYSCALL(kill(getpid(), SIGUSR2));
  again.wait(ws);
}
#endif

// =======================================================================================
// Filters, lifetimes and teardown

KJ_TEST("concurrent acceptAuthenticated on two tokio-ported loops does not race the filter") {
  // Regression guard for the per-identity allow-all filter: it must NOT be a process-wide
  // static kj::Rc (non-atomic refcount) shared across accept loops on different threads. Two
  // loops, each accepting a TCP connection and building a NetworkPeerIdentity (which creates
  // that filter), running concurrently. TSAN target; also ASAN-visible as a double-free if the
  // refcount ever races.
  constexpr kj::uint N = 40;
  auto runOne = []() noexcept {
    auto io = setupTokioAsyncIo();
    auto &ws = io.getWaitScope();
    for (kj::uint i = 0; i < N; i++) {
      auto listener = parseNow(io, "127.0.0.1")->listen();
      auto connectAddr = parseNow(io, kj::str("127.0.0.1:", listener->getPort()));
      auto acceptPromise = listener->acceptAuthenticated();
      auto client = connectAddr->connect().wait(ws);
      auto authed = acceptPromise.wait(ws);
      // Touch the identity so the allow-all filter is actually built and addRef'd; a TCP
      // peer's identity prints as its address.
      KJ_EXPECT(authed.peerIdentity->toString().startsWith("127.0.0.1:"),
          authed.peerIdentity->toString());
    }
  };
  kj::Thread other(runOne);
  runOne();
}

KJ_TEST("restrictPeers: a child network (and its addresses) outlive the parent network") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  // Build a restricted child, then DROP the parent network while keeping the child and an
  // address parsed from it. The refcounted PeerFilter chain must keep the parent's filter alive
  // through the child's Rc, so the child stays usable.
  kj::Own<kj::NetworkAddress> addr;
  kj::Own<kj::Network> child;
  {
    auto parent = io.getNetwork().restrictPeers({"public"_kj}, {});
    child = parent->restrictPeers({"private"_kj}, {});
    addr = child->parseAddress("127.0.0.1:1").wait(ws);
    // `parent` drops here.
  }
  // The child still works (its filter chain is intact): a private address connect is blocked
  // with KJ's error text, proving the (grand)parent rules still apply.
  auto blocked = kj::runCatchingExceptions([&]() { addr->connect().wait(ws); });
  KJ_EXPECT(blocked != kj::none);
  KJ_EXPECT(KJ_ASSERT_NONNULL(blocked).getDescription().contains("restrictPeers"));
}

KJ_TEST("loopback: addresses connect within the process once enabled") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto &network = io.getNetwork();

  // Off by default: "loopback:svc" is then a host "loopback" with service "svc".
  KJ_EXPECT_THROW_MESSAGE("getaddrinfo()", network.parseAddress("loopback:svc").wait(ws));

  kj::downcast<kj_rs_io::TokioNetwork>(network).enableLoopback();
  auto addr = network.parseAddress("loopback:svc").wait(ws);
  KJ_EXPECT(addr->toString() == "loopback:svc");
  auto receiver = addr->listen();
  KJ_EXPECT(receiver->getPort() == 0);

  // A restrictPeers() child shares the namespace, and the filter does not judge loopback
  // connections: this restriction would block any real address.
  auto restricted = network.restrictPeers({"1.2.3.4/32"_kj}, {});
  auto clientPromise = restricted->parseAddress("loopback:svc").wait(ws)->connect();
  auto server = receiver->accept().wait(ws);
  auto client = clientPromise.wait(ws);

  // Real sockets underneath: bytes flow both ways.
  client->write("ping"_kjb).wait(ws);
  kj::byte buffer[4];
  KJ_EXPECT(server->tryRead(buffer, 4, 4).wait(ws) == 4);
  KJ_EXPECT(kj::ArrayPtr<kj::byte>(buffer, 4) == "ping"_kjb);
  server->write("pong"_kjb).wait(ws);
  KJ_EXPECT(client->tryRead(buffer, 4, 4).wait(ws) == 4);
  KJ_EXPECT(kj::ArrayPtr<kj::byte>(buffer, 4) == "pong"_kjb);

  // Connections made before anyone accepts are queued, and different names are separate.
  auto other = network.parseAddress("loopback:other").wait(ws);
  auto queued = addr->connect().wait(ws);
  auto otherReceiver = other->listen();
  auto otherAccept = otherReceiver->accept();
  KJ_EXPECT(!otherAccept.poll(ws));
  auto accepted = receiver->accept().wait(ws);
  queued->write("!"_kjb).wait(ws);
  KJ_EXPECT(accepted->tryRead(buffer, 1, 1).wait(ws) == 1);
  KJ_EXPECT(!otherAccept.poll(ws));
}

KJ_TEST("loopback: a name belongs to the loop that first parsed it") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  kj::downcast<kj_rs_io::TokioNetwork>(io.getNetwork()).enableLoopback();
  auto addr = io.getNetwork().parseAddress("loopback:owned").wait(ws);
  auto receiver = addr->listen();

  // A clone of the address carried to another loop thread cannot connect: the queued end would
  // be a socket of that loop, unusable by the receiver here.
  kj::Maybe<kj::Exception> failure;
  {
    auto other = addr->clone();
    kj::Thread thread([&]() noexcept {
      auto otherIo = setupTokioAsyncIo();
      failure = kj::runCatchingExceptions([&]() { other->connect().wait(otherIo.getWaitScope()); });
    });
  }
  KJ_EXPECT(KJ_ASSERT_NONNULL(failure).getDescription().contains("different TokioEventPort"),
      KJ_ASSERT_NONNULL(failure).getDescription());

  // Nothing was queued: an accept here still waits.
  auto acceptPromise = receiver->accept();
  KJ_EXPECT(!acceptPromise.poll(ws));
}

KJ_TEST("dropping a just-started connect() then tearing down the context is clean") {
  // Start a connect(), kick the machinery with one poll, then drop the promise and destroy the
  // whole context -- exercising cancellation of the connect's readiness registration and the
  // teardown that follows (ASAN target). Whether the connect has settled by the poll is
  // environment-dependent and irrelevant: either way the drop + teardown must be clean.
  // 198.51.100.1 is TEST-NET-2 (RFC 5737), non-routable, so it usually stays pending.
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto addr = parseNow(io, "198.51.100.1:80");
  auto connectPromise = addr->connect();
  connectPromise.poll(ws);
  { auto dropped = kj::mv(connectPromise); }
  KJ_EXPECT(kj::evalLater([]() { return 1; }).wait(ws) == 1);
}

KJ_TEST("canceling a pending accept() then accepting again works") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto listener = parseNow(io, "127.0.0.1")->listen();
  auto connectAddr = parseNow(io, kj::str("127.0.0.1:", listener->getPort()));

  // Start an accept with no client, then drop it.
  {
    auto pending = listener->accept();
    KJ_EXPECT(!pending.poll(ws));
  }

  // The listener is still usable: a fresh accept completes against a new client.
  auto acceptPromise = listener->accept();
  auto client = connectAddr->connect().wait(ws);
  auto server = acceptPromise.wait(ws);
  KJ_EXPECT(server->write("x"_kjb).then([]() { return true; }).wait(ws));
}

KJ_TEST("context teardown while a DNS parseAddress() is in flight is clean") {
  // parseAddress() of a hostname spawns a runtime task (getaddrinfo on the blocking pool). Drop
  // the promise mid-lookup and tear the context down: the spawned task must be cancelled without
  // touching freed KJ state (ASAN target; the kj-rs-tokio teardown-order fix covers this).
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto parsePromise = io.getNetwork().parseAddress("example.invalid:80");
  // `.invalid` never resolves to success, but the lookup is in flight after one poll.
  parsePromise.poll(ws);
  { auto dropped = kj::mv(parsePromise); }
  KJ_EXPECT(kj::evalLater([]() { return 2; }).wait(ws) == 2);
}

KJ_TEST("zero-length write() is a no-op that succeeds") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto pair = makeTcpPair(io);
  pair.client->write(kj::ArrayPtr<const kj::byte>()).wait(ws);
  // The stream is still fully usable afterwards.
  pair.client->write("hi"_kjb).wait(ws);
  kj::byte buf[2];
  KJ_EXPECT(pair.server->tryRead(buf, 2, 2).wait(ws) == 2);
}

#if !_WIN32
KJ_TEST("write() to a reset peer surfaces a DISCONNECTED exception") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto pair = makeTcpPair(io);

  // RST the server end (SO_LINGER=0 close), then write from the client until the RST is
  // observed. The first write may still succeed into the local send buffer, so loop; the
  // failure, when it comes, must be DISCONNECTED (EPIPE/ECONNRESET), not FAILED.
  struct linger lin;
  lin.l_onoff = 1;
  lin.l_linger = 0;
  setRawSockopt(*pair.server, SOL_SOCKET, SO_LINGER, &lin, sizeof(lin));
  pair.server = nullptr;

  auto chunk = kj::heapArray<kj::byte>(64 * 1024);
  memset(chunk.begin(), 0, chunk.size());
  kj::Maybe<kj::Exception> maybeException;
  for (int i = 0; i < 100 && maybeException == kj::none; i++) {
    maybeException = kj::runCatchingExceptions([&]() { pair.client->write(chunk).wait(ws); });
  }
  auto &exception = KJ_ASSERT_NONNULL(maybeException, "write to a reset peer should fail");
  KJ_EXPECT(exception.getType() == kj::Exception::Type::DISCONNECTED, exception.getDescription());
}

KJ_TEST("write_all applies backpressure: it stays pending against a peer that never reads") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto pair = makeTcpPair(io);

  // Write far more than the combined send+recv socket buffers to a peer that never reads: the
  // write must NOT complete (write_all's try_write hits WouldBlock and awaits WRITABLE).
  auto payload = kj::heapArray<kj::byte>(16 * 1024 * 1024);
  memset(payload.begin(), 0x5a, payload.size());
  auto write = pair.client->write(payload);
  // Give the loop real turns; a correct write stays pending under backpressure.
  for (int i = 0; i < 5; i++) {
    io.getTimer().afterDelay(5 * kj::MILLISECONDS).wait(ws);
  }
  KJ_EXPECT(!write.poll(ws), "write_all must not complete while the peer never reads");

  // Now drain on the peer; the write completes.
  auto drain = [](kj::AsyncIoStream &s, size_t total) -> kj::Promise<void> {
    auto buf = kj::heapArray<kj::byte>(256 * 1024);
    size_t got = 0;
    while (got < total) {
      size_t n = co_await s.tryRead(buf.begin(), 1, buf.size());
      if (n == 0) break;
      got += n;
    }
  }(*pair.server, payload.size());
  write.exclusiveJoin(kj::mv(drain)).wait(ws);
}

KJ_TEST("getSockaddr builds a connectable IPv6 address from a raw sockaddr_in6") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  auto listener = parseNow(io, "[::1]")->listen();

  struct sockaddr_in6 sin6;
  memset(&sin6, 0, sizeof(sin6));
  sin6.sin6_family = AF_INET6;
  sin6.sin6_port = htons(static_cast<uint16_t>(listener->getPort()));
  sin6.sin6_addr = in6addr_loopback;
  auto addr = io.getNetwork().getSockaddr(&sin6, sizeof(sin6));
  KJ_EXPECT(addr->toString() == kj::str("[::1]:", listener->getPort()));

  auto acceptPromise = listener->accept();
  auto client = addr->connect().wait(ws);
  auto server = acceptPromise.wait(ws);
  client->write("v6"_kjb).wait(ws);
  kj::byte buf[2];
  KJ_EXPECT(server->tryRead(buf, 2, 2).wait(ws) == 2);
}

KJ_TEST("multiple concurrent onSignal for the same signum all fire") {
  // tokio broadcasts a signal to every live stream for that signum, so two concurrent
  // onSignal(SIGUSR2) must both resolve on a single delivery.
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto a = kj_rs_io::onSignal(SIGUSR2);
  auto b = kj_rs_io::onSignal(SIGUSR2);
  KJ_EXPECT(!a.poll(ws));  // both handlers installed before we raise
  KJ_EXPECT(!b.poll(ws));
  KJ_SYSCALL(kill(getpid(), SIGUSR2));
  a.wait(ws);
  b.wait(ws);
}

KJ_TEST("onSignal isolates different signums") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto usr1 = kj_rs_io::onSignal(SIGUSR1);
  auto usr2 = kj_rs_io::onSignal(SIGUSR2);
  KJ_EXPECT(!usr1.poll(ws));
  KJ_EXPECT(!usr2.poll(ws));
  KJ_SYSCALL(kill(getpid(), SIGUSR2));
  usr2.wait(ws);
  // Only SIGUSR2 was raised; the SIGUSR1 watcher stays pending.
  KJ_EXPECT(!usr1.poll(ws));
}

KJ_TEST("dropping a pending onSignal does not break later watches") {
  // Cancel a registered-but-unfired signal watch, then confirm a fresh watch still delivers --
  // the dropped tokio signal stream must not disturb the process-global registration. ASAN-
  // relevant (the drop cancels the stream).
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  {
    auto dropped = kj_rs_io::onSignal(SIGUSR2);
    KJ_EXPECT(!dropped.poll(ws));
  }
  auto again = kj_rs_io::onSignal(SIGUSR2);
  KJ_EXPECT(!again.poll(ws));
  KJ_SYSCALL(kill(getpid(), SIGUSR2));
  again.wait(ws);
}

KJ_TEST("onSignal for an unwatchable signum errors instead of aborting") {
  // SIGKILL/SIGSTOP cannot have handlers; tokio's signal() rejects them, which must surface as a
  // catchable kj::Exception (a rejected promise), never a crash. No signal is raised.
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  KJ_EXPECT_THROW_MESSAGE("signal", kj_rs_io::onSignal(SIGKILL).wait(ws));
}
#endif  // !_WIN32

// =======================================================================================
// Added coverage: vectored writes under backpressure, DNS failure, unix bind collisions,
// acceptAuthenticated through a restricted listener.

KJ_TEST("multi-piece write() larger than the socket buffer arrives intact and in order "
        "(partial writev + backpressure)") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto pair = makeTcpPair(io);

  // Several MiB across pieces of very different sizes, with empty pieces mixed in: the kernel
  // will accept only part of the iovec array per writev, so the Rust side must resume from the
  // exact byte the previous writev stopped at, across pieces.
  auto a = makePatternedData(3 * 1024 * 1024, 1);
  auto b = makePatternedData(7, 2);
  auto c = makePatternedData(2 * 1024 * 1024, 3);
  kj::ArrayPtr<const kj::byte> empty;
  kj::ArrayPtr<const kj::byte> pieces[] = {empty, a, b, empty, c, empty};

  auto expected = kj::heapArray<kj::byte>(a.size() + b.size() + c.size());
  memcpy(expected.begin(), a.begin(), a.size());
  memcpy(expected.begin() + a.size(), b.begin(), b.size());
  memcpy(expected.begin() + a.size() + b.size(), c.begin(), c.size());

  auto write = pair.client->write(kj::arrayPtr(pieces, kj::size(pieces)));
  // The reader runs concurrently: nothing drains the socket otherwise, so the write must hit
  // WouldBlock partway through the array and pick up where it left off.
  kj::joinPromisesFailFast(kj::arr(kj::mv(write), readExact(*pair.server, expected))).wait(ws);
}

KJ_TEST("multi-piece write() of only empty pieces succeeds without touching the socket") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto pair = makeTcpPair(io);
  kj::ArrayPtr<const kj::byte> pieces[] = {{}, {}};
  pair.client->write(kj::arrayPtr(pieces, 2)).wait(ws);
  // Nothing was written: a short read times out rather than returning bytes.
  kj::byte buffer[1];
  auto read = pair.server->tryRead(buffer, 1, 1);
  KJ_EXPECT(!read.poll(ws));
}

KJ_TEST("parseAddress of an unresolvable host fails with a getaddrinfo exception") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  // RFC 6761 reserves .invalid: resolvers must answer NXDOMAIN without asking upstream.
  auto exception = KJ_ASSERT_NONNULL(kj::runCatchingExceptions([&]() {
    boundedBy(io, io.getNetwork().parseAddress("nonexistent.invalid:80"), 30 * kj::SECONDS,
        "DNS failure to be reported")
        .wait(ws);
  }),
      "nonexistent.invalid unexpectedly resolved");
  KJ_EXPECT(exception.getDescription().contains("getaddrinfo"), exception.getDescription());
}

#if !_WIN32
KJ_TEST("listen() on a unix socket path that already exists fails (no unlink, like KJ)") {
  auto io = setupTokioAsyncIo();
  auto path = freshUnixSocketPath("bind-twice");
  KJ_DEFER(::unlink(path.cStr()));
  auto addr = parseNow(io, kj::str("unix:", path));
  auto first = addr->listen();
  KJ_EXPECT_THROW_MESSAGE("bind()", addr->listen());
  // The first listener still works.
  KJ_EXPECT(first->getPort() == 0);
}
#endif  // !_WIN32

KJ_TEST("acceptAuthenticated() through a restricted listener drops disallowed peers too") {
  // Mirrors the accept() test above through the other entry point: both share acceptImpl, and
  // the filter must run before any peer identity is minted.
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  kj_rs_io::TokioNetwork network;
  auto restricted = network.restrictPeers({"public"_kj}, {});

  auto listener = restricted->parseAddress("127.0.0.1", 0).wait(ws)->listen();
  auto acceptPromise = listener->acceptAuthenticated();

  auto client = network.parseAddress(kj::str("127.0.0.1:", listener->getPort()), 0)
                    .wait(ws)
                    ->connect()
                    .wait(ws);
  kj::byte buffer[1];
  // The listener drops the loopback (non-"public") peer: the client sees EOF, and the accept is
  // still pending afterwards (the EOF proves the drop happened, so this poll is meaningful).
  KJ_EXPECT(client->tryRead(buffer, 1, 1).wait(ws) == 0);
  KJ_EXPECT(!acceptPromise.poll(ws));
}

// =======================================================================================
// Vectored writes and operation start

KJ_TEST("multi-piece write(): more empty pieces than IOV_MAX before the data still delivers it") {
  // writev(2) only looks at the first IOV_MAX (1024) entries. A prefix of empty pieces longer
  // than that would make the kernel write zero bytes -- indistinguishable from a closed
  // connection -- while the real data sat further down the array; empty pieces are dropped
  // before the syscall.
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto pair = makeTcpPair(io);

  auto pieces = kj::heapArray<kj::ArrayPtr<const kj::byte>>(2049);
  for (auto &piece: pieces) piece = nullptr;
  pieces[2048] = "x"_kjb;

  pair.client->write(pieces.asPtr()).wait(ws);
  kj::byte buffer[1];
  KJ_EXPECT(pair.server->tryRead(buffer, 1, 1).wait(ws) == 1);
  KJ_EXPECT(buffer[0] == 'x');
}

KJ_TEST("every stream operation starts inside the call: an unawaited write goes out") {
  // kj-http queues header writes without awaiting them. started() (async-io.c++) puts the
  // operation under way inside the call, so a kept-but-unawaited write completes as the loop
  // turns. (When the syscall happens is tokio's
  // decision, see stream.rs; only a write *dropped* on the spot on a never-yet-ready descriptor
  // is not covered, and no test pins that.)
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto pair = makeTcpPair(io);

  auto unawaited = pair.client->write("started"_kjb);  // retained, never awaited before reading
  kj::byte buffer[7];
  KJ_EXPECT(pair.server->tryRead(buffer, 7, 7).wait(ws) == 7);
  KJ_EXPECT(kj::ArrayPtr<kj::byte>(buffer, 7) == "started"_kjb);
  unawaited.wait(ws);
}

KJ_TEST("listen() on a hostname binds every resolved address and accepts on each") {
  // KJ's NetworkAddressImpl::listen() creates one socket per resolved address and combines
  // them (newAggregateConnectionReceiver); binding only the first would make a `localhost`
  // listener reachable over one IP family only, depending on resolver order.
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  // A port free on IPv4 loopback; the IPv6 bind below shares it (like KJ, each socket binds
  // independently, so an explicit port is what makes them agree).
  kj::uint port;
  {
    auto probe = parseNow(io, "127.0.0.1")->listen();
    port = probe->getPort();
  }
  auto addr = parseNow(io, "localhost", port);
  auto text = addr->toString();
  bool hasV4 = text.contains("127.0.0.1");
  bool hasV6 = text.contains("::1");
  KJ_EXPECT(hasV4 || hasV6, text);
  auto listener = addr->listen();
  KJ_EXPECT(listener->getPort() == port);

  auto acceptOne = [&](kj::StringPtr connectTo) {
    auto acceptPromise = listener->accept();
    auto client = parseNow(io, connectTo)->connect().wait(ws);
    auto server = boundedBy(io, kj::mv(acceptPromise), 10 * kj::SECONDS, connectTo).wait(ws);
    client->write("hi"_kjb).wait(ws);
    kj::byte buffer[2];
    KJ_EXPECT(server->tryRead(buffer, 2, 2).wait(ws) == 2);
  };
  if (hasV4) acceptOne(kj::str("127.0.0.1:", port));
  if (hasV6) acceptOne(kj::str("[::1]:", port));
  // (When the resolver returns both families, both connects above were accepted by ONE
  // receiver; when it returns one, this degrades to the single-socket case.)
}

KJ_TEST(
    "restrictPeers applies at connect(), not at parse (KJ's parse-time literal check is not kept)") {
  // KJ rejects a filtered *literal* already in parseAddress(); here every address parses and the
  // same rejection happens on connect(), with KJ's connect-time text. Nothing in workerd tells
  // the two moments apart, and it keeps parsing free of filter policy.
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  kj_rs_io::TokioNetwork network;

#if !_WIN32
  auto ipOnly = network.restrictPeers({"private"_kj}, {});
  auto unixAddr = ipOnly->parseAddress("unix:/tmp/kj-rs-io-never-bound.sock", 0).wait(ws);
  KJ_EXPECT_THROW_MESSAGE("connect() blocked by restrictPeers()", unixAddr->connect().wait(ws));
#endif

  auto unixOnly = network.restrictPeers({"unix"_kj}, {});
  auto ipAddr = unixOnly->parseAddress("127.0.0.1:1", 0).wait(ws);
  KJ_EXPECT_THROW_MESSAGE("connect() blocked by restrictPeers()", ipAddr->connect().wait(ws));

  auto publicOnly = network.restrictPeers({"public"_kj}, {});
  auto loopback = publicOnly->parseAddress("127.0.0.1:1", 0).wait(ws);
  KJ_EXPECT_THROW_MESSAGE("connect() blocked by restrictPeers()", loopback->connect().wait(ws));
}

// The SIGPIPE test re-executes this test binary as a child so the check starts from the
// process default disposition (SIG_DFL), which every earlier test in this process has already
// replaced by setting up a context. The child is selected with KJ's `--filter FILE:LINE`.
#if !_WIN32
kj::String currentExecutablePath() {
#if __APPLE__
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);  // reports the required size
  auto buffer = kj::heapArray<char>(size + 1);
  KJ_REQUIRE(_NSGetExecutablePath(buffer.begin(), &size) == 0);
  return kj::heapString(buffer.begin());
#else
  char buffer[PATH_MAX];
  ssize_t n;
  KJ_SYSCALL(n = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1));
  buffer[n] = '\0';
  return kj::heapString(buffer);
#endif
}

constexpr const char *SIGPIPE_CHILD_ENV = "KJ_RS_IO_SIGPIPE_CHILD";
// The child exits with this code only after every check below passed: a child that ran no test
// (a filter mismatch: the KJ runner exits 0 then) or failed one (exit 1) cannot pass the parent.
constexpr int SIGPIPE_CHILD_OK = 42;
// KJ records a multi-line KJ_TEST(...) at its closing line; keep the declaration on one line so
// `__LINE__ + 1` is exactly what `--filter FILE:LINE` must name.
static const int SIGPIPE_TEST_LINE = __LINE__ + 1;
KJ_TEST("a context ignores SIGPIPE: writes to a vanished peer fail, the process survives") {
  if (getenv(SIGPIPE_CHILD_ENV) != nullptr) {
    // --- Child: start from the default disposition, then rely on setupTokioAsyncIo() alone.
    // KJ_REQUIRE rather than KJ_EXPECT throughout: a failure must change the exit status.
    KJ_SYSCALL(signal(SIGPIPE, SIG_DFL) == SIG_ERR ? -1 : 0);
    auto io = setupTokioAsyncIo();
    auto &ws = io.getWaitScope();

    // A stream socket whose peer is closed: the second write (after the RST) raises SIGPIPE
    // unless it is ignored.
    int sv[2];
    KJ_SYSCALL(socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    auto stream =
        io.getLowLevelProvider().wrapSocketFd(sv[0], kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP);
    KJ_SYSCALL(close(sv[1]));
    auto socketException = KJ_ASSERT_NONNULL(kj::runCatchingExceptions([&]() {
      stream->write("x"_kjb).wait(ws);
      stream->write("y"_kjb).wait(ws);
    }),
        "write to a closed unix socket peer unexpectedly succeeded");
    KJ_REQUIRE(socketException.getType() == kj::Exception::Type::DISCONNECTED,
        socketException.getDescription());
    _exit(SIGPIPE_CHILD_OK);
  }

  // --- Parent: run the child with the default SIGPIPE disposition and check how it died.
  auto exe = currentExecutablePath();
  auto filter = kj::str(__FILE__, ":", SIGPIPE_TEST_LINE);
  pid_t pid;
  KJ_SYSCALL(pid = fork());
  if (pid == 0) {
    // Only exec after fork in this multi-threaded process; the disposition is inherited as
    // SIG_IGN from the parent's earlier contexts, which the child resets itself.
    setenv(SIGPIPE_CHILD_ENV, "1", 1);
    const char *argv[] = {exe.cStr(), "--filter", filter.cStr(), nullptr};
    execv(exe.cStr(), const_cast<char *const *>(argv));
    _exit(127);
  }
  int status;
  KJ_SYSCALL(waitpid(pid, &status, 0));
  if (WIFSIGNALED(status)) {
    KJ_FAIL_EXPECT("the child was killed by a signal", WTERMSIG(status),
        WTERMSIG(status) == SIGPIPE ? "(SIGPIPE: the disposition was not set)" : "");
  } else {
    KJ_EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == SIGPIPE_CHILD_OK, WEXITSTATUS(status),
        "(0: the child ran no test -- filter mismatch; 1: a child check failed)");
  }
}
#endif  // !_WIN32

KJ_TEST("abortRead() ends a pending read with EOF (shutdown(SHUT_RD), like KJ)") {
  // kj-http's WebSocket abort and CONNECT error handling call abortRead() to terminate reads;
  // the default no-op would leave them blocked.
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto pair = makeTcpPair(io);

  kj::byte buffer[8];
  auto pendingRead = pair.server->tryRead(buffer, 1, sizeof(buffer));
  KJ_EXPECT(!pendingRead.poll(ws));
  pair.server->abortRead();
  KJ_EXPECT(
      boundedBy(io, kj::mv(pendingRead), 10 * kj::SECONDS, "EOF after abortRead").wait(ws) == 0);
  // Reads issued afterwards observe EOF too.
  KJ_EXPECT(pair.server->tryRead(buffer, 1, sizeof(buffer)).wait(ws) == 0);
  // The write side is unaffected.
  pair.server->write("still"_kjb).wait(ws);
  KJ_EXPECT(pair.client->tryRead(buffer, 5, 5).wait(ws) == 5);
}

KJ_TEST("an unawaited multi-piece write() goes out too (coroutine start)") {
  // writePieces is the one stream method whose eager start relies on the coroutine polling its
  // bridged future synchronously rather than on started().
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto pair = makeTcpPair(io);

  kj::ArrayPtr<const kj::byte> pieces[] = {"multi"_kjb, "-"_kjb, "piece"_kjb};
  auto unawaited = pair.client->write(kj::arrayPtr(pieces, kj::size(pieces)));
  kj::byte buffer[11];
  KJ_EXPECT(pair.server->tryRead(buffer, 11, 11).wait(ws) == 11);
  KJ_EXPECT(kj::ArrayPtr<kj::byte>(buffer, 11) == "multi-piece"_kjb);
  unawaited.wait(ws);
}

KJ_TEST("tryRead with minBytes == 0 still waits for data (KJ parity: EAGAIN always waits)") {
  // KJ's AsyncStreamFd::tryReadInternal waits for readability on EAGAIN regardless of
  // minBytes; minBytes only decides when to stop once bytes have arrived. So a zero-minimum
  // read on an idle socket parks, and returns as soon as the first bytes land.
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto pair = makeTcpPair(io);

  kj::byte buffer[8];
  auto read = pair.server->tryRead(buffer, 0, sizeof(buffer));
  io.getTimer().afterDelay(20 * kj::MILLISECONDS).wait(ws);
  KJ_EXPECT(!read.poll(ws), "minBytes == 0 must not return 0 from an idle socket");
  pair.client->write("ab"_kjb).wait(ws);
  KJ_EXPECT(boundedBy(io, kj::mv(read), 5 * kj::SECONDS, "the zero-minimum read").wait(ws) == 2);
  KJ_EXPECT(kj::ArrayPtr<kj::byte>(buffer, 2) == "ab"_kjb);
  // Buffered bytes: a zero-minimum read returns what is there without waiting for more.
  pair.client->write("cd"_kjb).wait(ws);
  KJ_EXPECT(boundedBy(io, pair.server->tryRead(buffer, 0, sizeof(buffer)), 5 * kj::SECONDS,
                "the second zero-minimum read")
                .wait(ws) == 2);
}

KJ_TEST("an address with no socket addresses fails connect() and listen() with an error") {
  // The empty-list edge of connect()'s try-each-address loop and listen()'s bind-every-address
  // loop: neither may silently do nothing.
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto empty = kj::heap<kj_rs_io::TokioNetworkAddress>(
      address_from_loopback_ports(::rust::Slice<const uint16_t>()),
      kj::arc<kj_rs_io::PeerFilter>());
  auto exception = expectConnectFailure(io, empty->connect());
  KJ_EXPECT(exception.getDescription().contains("no addresses to connect to"),
      exception.getDescription());
  auto listenFailure = KJ_ASSERT_NONNULL(kj::runCatchingExceptions([&]() { empty->listen(); }));
  KJ_EXPECT(listenFailure.getDescription().contains("no addresses to bind"),
      listenFailure.getDescription());
  (void)ws;
}

#if !_WIN32
KJ_TEST("getSockaddr keeps a zero-initialized sockaddr_un usable and prints it like KJ") {
  // The conventional way to build a sockaddr_un: memset to zero, strcpy the path, pass
  // sizeof(sockaddr_un). The trailing NULs are part of the address KJ stores verbatim and reads
  // up to the first NUL (safeUnixPath); listen(), connect() and toString() must all work.
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto path = freshUnixSocketPath("raw-un");
  struct sockaddr_un sun;
  memset(&sun, 0, sizeof(sun));
  sun.sun_family = AF_UNIX;
  KJ_REQUIRE(path.size() < sizeof(sun.sun_path));
  memcpy(sun.sun_path, path.begin(), path.size());
  auto addr = io.getNetwork().getSockaddr(&sun, sizeof(sun));
  KJ_EXPECT(addr->toString() == kj::str("unix:", path), addr->toString());

  auto listener = addr->listen();
  auto acceptPromise = listener->accept();
  auto client = addr->connect().wait(ws);
  auto server = acceptPromise.wait(ws);
  client->write("raw"_kjb).wait(ws);
  kj::byte buffer[3];
  KJ_EXPECT(server->tryRead(buffer, 3, 3).wait(ws) == 3);
  ::unlink(path.cStr());
}
#endif

KJ_TEST("provider pipes are socket pairs: a write completes before anyone reads") {
  // kj's own provider hands out real pipes/socketpairs, and callers rely on their kernel
  // buffering: a small write completes without a reader waiting (an in-memory kj pipe would
  // leave it pending). workerd's loopback transport also asks the provider for real sockets.
  // Both pipe kinds are socket pairs here (async-io.c++ newOneWayPipe explains).
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  auto oneWay = io.getProvider().newOneWayPipe();
  oneWay.out->write("one"_kjb).wait(ws);  // completes with no read outstanding
  kj::byte buffer[3];
  KJ_EXPECT(oneWay.in->tryRead(buffer, 3, 3).wait(ws) == 3);
  KJ_EXPECT(kj::ArrayPtr<kj::byte>(buffer, 3) == "one"_kjb);
  oneWay.out = nullptr;
  KJ_EXPECT(oneWay.in->tryRead(buffer, 1, 1).wait(ws) == 0);  // EOF on close

  auto twoWay = io.getProvider().newTwoWayPipe();
  twoWay.ends[0]->write("two"_kjb).wait(ws);
  KJ_EXPECT(twoWay.ends[1]->tryRead(buffer, 3, 3).wait(ws) == 3);
  twoWay.ends[1]->write("owt"_kjb).wait(ws);
  KJ_EXPECT(twoWay.ends[0]->tryRead(buffer, 3, 3).wait(ws) == 3);
  KJ_EXPECT(kj::ArrayPtr<kj::byte>(buffer, 3) == "owt"_kjb);
#if !_WIN32
  KJ_EXPECT(twoWay.ends[0]->getFd() != kj::none, "a real socket, not an in-memory pipe");
  KJ_EXPECT(oneWay.in.get() != nullptr);
#endif
  twoWay.ends[0]->shutdownWrite();
  KJ_EXPECT(twoWay.ends[1]->tryRead(buffer, 1, 1).wait(ws) == 0);
}

#if !_WIN32
KJ_TEST(
    "multi-piece write() with more than IOV_MAX non-empty pieces arrives intact (std clamps the "
    "iovec count), on sockets and provider pipes") {
  // writev(2) rejects more than IOV_MAX entries (EINVAL; sendmsg EMSGSIZE). KJ's writeInternal
  // batches by iovMax(); so does write_all_pieces (stream.rs IOV_MAX, 1024 on every platform).
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  size_t count = 1024 + 1;
  auto data = makePatternedData(count, 3);
  auto pieces = kj::heapArray<kj::ArrayPtr<const kj::byte>>(count);
  for (size_t i = 0; i < count; i++) pieces[i] = data.slice(i, i + 1);

  auto pair = makeTcpPair(io);
  auto socketWrite = pair.client->write(pieces.asPtr());
  readExact(*pair.server, data).wait(ws);
  socketWrite.wait(ws);

  auto pipe = io.getProvider().newOneWayPipe();
  auto pipeWrite = pipe.out->write(pieces.asPtr());
  readExact(*pipe.in, data).wait(ws);
  pipeWrite.wait(ws);
}
#endif

KJ_TEST("connectAuthenticated reports the connected address as a NetworkPeerIdentity") {
  // KJ's NetworkAddressImpl::connectAuthenticated(): the identity is the address that connected
  // (kj::NetworkAddress's own default would wrap a clone of the multi-address parent instead).
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  auto listener = parseNow(io, "127.0.0.1")->listen();
  auto acceptPromise = listener->accept();
  auto client =
      parseNow(io, kj::str("127.0.0.1:", listener->getPort()))->connectAuthenticated().wait(ws);
  auto server = acceptPromise.wait(ws);

  auto &identity =
      KJ_ASSERT_NONNULL(kj::tryDowncast<kj::NetworkPeerIdentity>(*client.peerIdentity));
  KJ_EXPECT(identity.toString() == kj::str("127.0.0.1:", listener->getPort()));
  // The identity's address is connectable in its own right (it carries this network's filter,
  // as KJ's does).
  auto acceptAgain = listener->accept();
  auto second = identity.getAddress().connect().wait(ws);
  acceptAgain.wait(ws);
  client.stream->write("auth"_kjb).wait(ws);
  kj::byte buffer[4];
  KJ_EXPECT(server->tryRead(buffer, 4, sizeof(buffer)).wait(ws) == 4);
}

KJ_TEST("connectAuthenticated through a restricted network keeps the identity restricted") {
  // The identity's NetworkAddress carries the connecting network's filter chain, as under KJ:
  // a connect() back through it is judged by the same restrictPeers().
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  auto listener = parseNow(io, "127.0.0.1")->listen();
  kj::StringPtr deny[] = {"private"};
  // Allow the loopback connect itself, so the identity comes back; then narrow it.
  auto restricted = io.getNetwork().restrictPeers({"public"_kj, "local"_kj}, deny);
  auto acceptPromise = listener->accept();
  auto address = restricted->parseAddress(kj::str("127.0.0.1:", listener->getPort())).wait(ws);
  auto client = address->connectAuthenticated().wait(ws);
  acceptPromise.wait(ws);
  auto &identity =
      KJ_ASSERT_NONNULL(kj::tryDowncast<kj::NetworkPeerIdentity>(*client.peerIdentity));
  // Same chain: the identity address's toString() and a connect() through it both work the
  // way this network's own would (loopback is allowed here), which is what a caller relying on
  // KJ's behavior gets.
  auto acceptAgain = listener->accept();
  auto second = identity.getAddress().connect().wait(ws);
  acceptAgain.wait(ws);
}

#if !_WIN32
KJ_TEST("connectAuthenticated reports LocalPeerIdentity credentials on unix sockets") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto path = freshUnixSocketPath("connect-auth");
  auto addr = parseNow(io, kj::str("unix:", path));

  auto listener = addr->listen();
  auto acceptPromise = listener->accept();
  auto client = addr->connectAuthenticated().wait(ws);
  acceptPromise.wait(ws);

  auto &identity = KJ_ASSERT_NONNULL(kj::tryDowncast<kj::LocalPeerIdentity>(*client.peerIdentity));
  auto creds = identity.getCredentials();
  KJ_EXPECT(KJ_ASSERT_NONNULL(creds.pid) == getpid());
  KJ_EXPECT(KJ_ASSERT_NONNULL(creds.uid) == getuid());
  ::unlink(path.cStr());
}
#endif

KJ_TEST("getSockaddr re-encodes a sockaddr from its fields: garbage past the family's struct and "
        "in its padding is not part of the address") {
  // KJ's interface only promises the family's *fields* are set. A caller may hand over a whole
  // sockaddr_storage (addrlen = sizeof(storage)) with whatever the stack held past sin_addr, or
  // a sockaddr_in whose sin_zero was never written. Both must produce the same address as a
  // clean sockaddr_in -- byte for byte, since the Rust side compares and orders addresses by
  // their bytes.
  auto io = setupTokioAsyncIo();

  struct sockaddr_in clean;
  memset(&clean, 0, sizeof(clean));
  clean.sin_family = AF_INET;
  clean.sin_port = htons(8080);
  clean.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  auto expected = io.getNetwork().getSockaddr(&clean, sizeof(clean))->toString();
  KJ_EXPECT(expected == "127.0.0.1:8080", expected);

  struct sockaddr_storage dirty;
  memset(&dirty, 0xAB, sizeof(dirty));
  memcpy(&dirty, &clean, offsetof(struct sockaddr_in, sin_zero));  // fields only; sin_zero dirty
  KJ_EXPECT(io.getNetwork().getSockaddr(&dirty, sizeof(dirty))->toString() == expected);
  KJ_EXPECT(io.getNetwork().getSockaddr(&dirty, sizeof(clean))->toString() == expected);

  struct sockaddr_in6 clean6;
  memset(&clean6, 0, sizeof(clean6));
  clean6.sin6_family = AF_INET6;
  clean6.sin6_port = htons(8080);
  clean6.sin6_addr.s6_addr[15] = 1;
  auto expected6 = io.getNetwork().getSockaddr(&clean6, sizeof(clean6))->toString();
  KJ_EXPECT(expected6 == "[::1]:8080", expected6);
  memset(&dirty, 0xCD, sizeof(dirty));
  memcpy(&dirty, &clean6, sizeof(clean6));
  KJ_EXPECT(io.getNetwork().getSockaddr(&dirty, sizeof(dirty))->toString() == expected6);

  // A length shorter than the family's struct, or a family this backend does not speak, is
  // rejected up front rather than read past or handed to Rust half-initialized.
  KJ_EXPECT_THROW_MESSAGE("too short", io.getNetwork().getSockaddr(&clean, sizeof(clean) - 1));
  struct sockaddr unspec;
  memset(&unspec, 0, sizeof(unspec));
  unspec.sa_family = AF_UNSPEC;
  KJ_EXPECT_THROW_MESSAGE(
      "unsupported socket address family", io.getNetwork().getSockaddr(&unspec, sizeof(unspec)));
}

#if !_WIN32
KJ_TEST("getSockaddr re-encodes a sockaddr_un by its pathname, however long the caller's length") {
  // sizeof(sockaddr_un) with trailing zeros, or the exact offsetof + strlen + 1, or the exact
  // length without the NUL: the same path, so the same address.
  auto io = setupTokioAsyncIo();
  auto path = freshUnixSocketPath("re-encode");
  struct sockaddr_un sun;
  memset(&sun, 0, sizeof(sun));
  sun.sun_family = AF_UNIX;
  KJ_REQUIRE(path.size() < sizeof(sun.sun_path));
  memcpy(sun.sun_path, path.begin(), path.size());
  auto expected = kj::str("unix:", path);
  KJ_EXPECT(io.getNetwork().getSockaddr(&sun, sizeof(sun))->toString() == expected);
  auto exact = offsetof(struct sockaddr_un, sun_path) + path.size();
  KJ_EXPECT(io.getNetwork().getSockaddr(&sun, exact + 1)->toString() == expected);
  KJ_EXPECT(io.getNetwork().getSockaddr(&sun, exact)->toString() == expected);
  // Garbage after the NUL is not part of a pathname address either.
  memset(sun.sun_path + path.size() + 1, 0xEE, sizeof(sun.sun_path) - path.size() - 1);
  KJ_EXPECT(io.getNetwork().getSockaddr(&sun, sizeof(sun))->toString() == expected);
}

#endif

KJ_TEST("port text is a decimal port or a service name; KJ's octal/hex grammar is not kept") {
  // KJ parses the port with strtoul(..., 0), so "010" is 8 and "0x50" is 80. The tokio backend
  // reads decimal only and hands anything else to getaddrinfo as a service name -- an
  // intentional simplification (net.rs explains; no configuration relies on octal ports).
  auto io = setupTokioAsyncIo();
  KJ_EXPECT(parseNow(io, "127.0.0.1:80")->toString() == "127.0.0.1:80");
  KJ_EXPECT(parseNow(io, "127.0.0.1:010")->toString() == "127.0.0.1:10");
  KJ_EXPECT_THROW_MESSAGE("Port number too large", parseNow(io, "127.0.0.1:70000"));
  KJ_EXPECT_THROW_MESSAGE("Port number too large", parseNow(io, "127.0.0.1:99999999999999999999"));
  // "http" is a service name resolvable on every platform's services database.
  // A service name resolves through the system services database (/etc/services), which a
  // minimal sandbox (the internal RBE executors) may not have; the grammar is what is under
  // test, not the database, so only check the lookup where the database can answer it.
  if (getservbyname("http", "tcp") != nullptr) {
    KJ_EXPECT(parseNow(io, "127.0.0.1:http")->toString() == "127.0.0.1:80");
  } else {
    KJ_EXPECT_THROW_MESSAGE("DNS lookup failed", parseNow(io, "127.0.0.1:http"));
  }
}

#if !_WIN32
#if __linux__
KJ_TEST("unix-abstract: addresses (workerd.capnp's Socket/ExternalServer grammar) listen, connect, "
        "print and identify") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto name = kj::str("kj-rs-io-test-", getpid());
  auto addr = parseNow(io, kj::str("unix-abstract:", name));
  KJ_EXPECT(addr->toString() == kj::str("unix-abstract:", name), addr->toString());

  auto listener = addr->listen();
  auto acceptPromise = listener->acceptAuthenticated();
  auto client = addr->connect().wait(ws);
  auto server = acceptPromise.wait(ws);
  KJ_EXPECT(kj::tryDowncast<kj::LocalPeerIdentity>(*server.peerIdentity) != kj::none,
      "an abstract-socket peer is a local peer");
  client->write("abs"_kjb).wait(ws);
  kj::byte buffer[3];
  KJ_EXPECT(server.stream->tryRead(buffer, 3, 3).wait(ws) == 3);

  // The address round-trips through the raw sockaddr KJ's interfaces expose: getpeername() on
  // the client yields the listener's abstract address, and getSockaddr() rebuilds it.
  struct sockaddr_un peer;
  kj::uint len = sizeof(peer);
  client->getpeername(reinterpret_cast<struct sockaddr *>(&peer), &len);
  KJ_EXPECT(len == offsetof(struct sockaddr_un, sun_path) + 1 + name.size(), len);
  KJ_EXPECT(peer.sun_path[0] == '\0');
  KJ_EXPECT(io.getNetwork().getSockaddr(&peer, len)->toString() == addr->toString());

  // restrictPeers grammar: "unix-abstract" is its own class, as in KJ.
  auto pathOnly = io.getNetwork().restrictPeers({"unix"_kj}, {});
  KJ_EXPECT_THROW_MESSAGE("connect() blocked by restrictPeers()",
      pathOnly->parseAddress(kj::str("unix-abstract:", name), 0).wait(ws)->connect().wait(ws));
}
#else
KJ_TEST("unix-abstract: addresses are refused off Linux with KJ's message") {
  auto io = setupTokioAsyncIo();
  KJ_EXPECT_THROW_MESSAGE("only supported on Linux", parseNow(io, "unix-abstract:kj-rs-io-test"));
}
#endif

KJ_TEST("wrapConnectingSocketFd is an intentional UNIMPLEMENTED stub") {
  auto io = setupTokioAsyncIo();
  struct sockaddr_in sin;
  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;
  KJ_EXPECT_THROW_MESSAGE("wrapConnectingSocketFd is not implemented",
      io.getLowLevelProvider().wrapConnectingSocketFd(
          -1, reinterpret_cast<struct sockaddr *>(&sin), sizeof(sin), 0));
}

KJ_TEST("unsupported connecting-socket wrapping closes a transferred socket") {
  auto io = setupTokioAsyncIo();
  auto pair = makeTcpPair(io);
  int duplicate;
  KJ_SYSCALL(duplicate = fcntl(rawSocketOf(*pair.client), F_DUPFD_CLOEXEC, 0));
  kj::OwnFd transferred(duplicate);
  pair.client = nullptr;

  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(pair.listener->getPort());
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  KJ_EXPECT_THROW_MESSAGE("wrapConnectingSocketFd is not implemented",
      io.getLowLevelProvider().wrapConnectingSocketFd(
          kj::mv(transferred), reinterpret_cast<struct sockaddr *>(&address), sizeof(address)));

  kj::byte byte;
  auto read = pair.server->tryRead(&byte, 1, 1);
  KJ_EXPECT(io.getTimer().timeoutAfter(5 * kj::SECONDS, kj::mv(read)).wait(io.getWaitScope()) == 0);
}

KJ_TEST("unsupported listen filter closes a transferred listening socket") {
  auto io = setupTokioAsyncIo();
  auto prebound = create_prebound_listener_fd();
  kj::OwnFd transferred(prebound.fd);
  auto address = parseNow(io, kj::str("127.0.0.1:", prebound.port));
  auto client = address->connect().wait(io.getWaitScope());

  class RejectAll final: public kj::LowLevelAsyncIoProvider::NetworkFilter {
   public:
    bool shouldAllow(const struct sockaddr *, kj::uint) override {
      return false;
    }
  } filter;
  KJ_EXPECT_THROW_MESSAGE("wrapListenSocketFd with a caller-owned NetworkFilter is not implemented",
      io.getLowLevelProvider().wrapListenSocketFd(kj::mv(transferred), filter));

  kj::byte byte;
  auto closed = client->tryRead(&byte, 1, 1).then([](size_t count) {
    KJ_EXPECT(count == 0);
  }, [](kj::Exception &&exception) {
    KJ_EXPECT(exception.getType() == kj::Exception::Type::DISCONNECTED);
  });
  io.getTimer().timeoutAfter(5 * kj::SECONDS, kj::mv(closed)).wait(io.getWaitScope());
}

KJ_TEST("wrapInputFd/wrapOutputFd take sockets, not pipes (kj's win32 definition, on every "
        "platform)") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  int sv[2];
  KJ_SYSCALL(socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
  auto in =
      io.getLowLevelProvider().wrapInputFd(sv[0], kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP);
  auto out =
      io.getLowLevelProvider().wrapOutputFd(sv[1], kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP);
  out->write("sock"_kjb).wait(ws);
  kj::byte buffer[4];
  KJ_EXPECT(in->tryRead(buffer, 4, 4).wait(ws) == 4);
  KJ_EXPECT(kj::ArrayPtr<kj::byte>(buffer, 4) == "sock"_kjb);

  int fds[2];
  KJ_SYSCALL(pipe(fds));
  KJ_EXPECT_THROW_MESSAGE("getsockname",
      io.getLowLevelProvider().wrapInputFd(fds[0], kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP));
  close(fds[1]);
}
#endif

#if !_WIN32
KJ_TEST("getSockaddr keeps a pathname that fills sun_path with no terminating NUL whole") {
  // KJ allows the unterminated form (the address's length is what ends the path). It must print
  // whole; std refuses to bind or connect it (no room for its NUL), which is where that surfaces.
  auto io = setupTokioAsyncIo();
  struct sockaddr_un sun;
  memset(&sun, 0, sizeof(sun));
  sun.sun_family = AF_UNIX;
  memset(sun.sun_path, 'x', sizeof(sun.sun_path));
  sun.sun_path[0] = '/';
  auto shown = io.getNetwork().getSockaddr(&sun, sizeof(sun))->toString();
  KJ_EXPECT(shown.size() == 5 + sizeof(sun.sun_path), shown.size());
  KJ_EXPECT(shown.startsWith("unix:/xxx"), shown);
}
#endif

KJ_TEST("addresses cloned concurrently on two threads share their network's filter safely") {
  // KJ permits cloning a kj::NetworkAddress off its loop (its state is immutable); each clone
  // takes a share of the network's filter, so that share count must be atomic (kj::Arc). Run
  // under a TSAN config to check the property, not only to pass.
  kj::EventLoop loop;
  kj::WaitScope ws(loop);
  kj_rs_io::TokioNetwork network;
  struct sockaddr_in sin;
  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_port = htons(80);
  sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  auto first = network.getSockaddr(&sin, sizeof(sin));
  auto second = first->clone();
  auto cloneMany = [](kj::Own<kj::NetworkAddress> address) {
    for (int i = 0; i < 100000; i++) {
      auto clone = address->clone();
      KJ_EXPECT(clone->toString() == "127.0.0.1:80");
    }
  };
  kj::Thread other([address = kj::mv(second), cloneMany]() mutable {
    kj::EventLoop threadLoop;
    kj::WaitScope threadWs(threadLoop);
    cloneMany(kj::mv(address));
  });
  cloneMany(kj::mv(first));
}

KJ_TEST("accept() on a thread without a TokioEventPort fails instead of waiting forever") {
  // A receiver carried to another thread is memory-safe (lib.rs, "Threads") but has no driver
  // there: like every bridged operation, accept() reports that as a kj::Exception.
  auto io = setupTokioAsyncIo();
  auto listener = parseNow(io, "127.0.0.1")->listen();
  kj::Thread other([receiver = kj::mv(listener)]() mutable {
    kj::EventLoop loop;
    kj::WaitScope ws(loop);
    auto exception = kj::runCatchingExceptions([&] { receiver->accept().wait(ws); });
    KJ_EXPECT(KJ_ASSERT_NONNULL(exception).getDescription().contains("no TokioEventPort"));
    receiver = nullptr;
  });
}

}  // namespace
}  // namespace kj_rs_io_test
