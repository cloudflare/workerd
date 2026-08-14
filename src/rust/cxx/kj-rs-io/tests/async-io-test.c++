// Tests for kj-rs-io: tokio-backed implementations of KJ's async I/O interfaces, driven by a
// kj::EventLoop on a TokioEventPort. Following kj-rs conventions, C++ KJ_TESTs drive; Rust
// helpers (tests/lib.rs) provide the native-side behaviors (unwrap fast path, pre-bound fds).

#include "io-test-helpers.h"
#include "kj-rs-io-test/lib.rs.h"
#include "kj-rs-io/async-io.h"

#include <kj/array.h>
#include <kj/async.h>
#include <kj/debug.h>
#include <kj/test.h>
#include <kj/thread.h>

#include <cstring>

#if _WIN32
#include <windows.h>  // Win32 APIs used by the Windows-only test arms below.

// After windows.h: un-breaks macros it leaks over KJ's, notably ERROR (which otherwise breaks
// the KJ_LOG(ERROR, ...) inside KJ_FAIL_* expansions).
#include <kj/windows-sanity.h>
#else
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace kj_rs_io_test {
namespace {

using kj_rs_io::setupTokioAsyncIo;
using kj_rs_io::TokioAsyncIoContext;

// =======================================================================================
// Helpers (shared ones: io-test-helpers.h)

::rust::Slice<const uint8_t> toRust(kj::ArrayPtr<const kj::byte> data) {
  return ::rust::Slice<const uint8_t>(data.begin(), data.size());
}

::rust::Vec<uint8_t> toRustVec(kj::ArrayPtr<const kj::byte> data) {
  ::rust::Vec<uint8_t> vec;
  vec.reserve(data.size());
  for (auto b: data) {
    vec.push_back(b);
  }
  return vec;
}

// Waits for `connectPromise` (a connect to a certainly-closed port) to fail and returns the
// exception, bounded by a KJ timer so a never-settling connect fails the test with a message
// instead of eating the binary's bazel timeout. (An earlier version of this helper carried a
// watchdog thread and CPU accounting to diagnose a Windows CI wedge: a lost connect-readiness
// wake caused by the then single-threaded waker bridge. That bridge is thread-safe now and the
// wedge is gone with it; the timer bound is kept as a plain test hygiene measure.)
kj::Exception expectConnectFailure(
    TokioAsyncIoContext &io, kj::Promise<kj::Own<kj::AsyncIoStream>> connectPromise) {
  auto timeout =
      io.getTimer().afterDelay(30 * kj::SECONDS).then([]() -> kj::Own<kj::AsyncIoStream> {
    KJ_FAIL_ASSERT("connect() to a closed port neither succeeded nor failed within 30s");
  });
  return KJ_ASSERT_NONNULL(kj::runCatchingExceptions([&]() {
    connectPromise.exclusiveJoin(kj::mv(timeout)).wait(io.getWaitScope());
  }),
      "connect() to a closed port unexpectedly succeeded");
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

  // Canceling mid-large-write also leaves the process sane (bytes may be lost, like KJ).
  {
    auto data = makePatternedData(16 * 1024 * 1024, 7);
    auto bigWrite = pair.client->write(data);
    if (bigWrite.poll(ws)) {
      bigWrite.wait(ws);
    }
  }
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
  pair.server->setsockopt(SOL_SOCKET, SO_LINGER, &lin, sizeof(lin));
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

KJ_TEST("sockname/peername/sockopt/getFd passthrough") {
#if _WIN32
  return;
#else
  auto io = setupTokioAsyncIo();
  auto pair = makeTcpPair(io);

  // getFd is populated.
  KJ_EXPECT(KJ_ASSERT_NONNULL(pair.client->getFd()) >= 0);

  // The client's peer is the server's local socket.
  struct sockaddr_in peer, local;
  kj::uint peerLen = sizeof(peer), localLen = sizeof(local);
  pair.client->getpeername(reinterpret_cast<struct sockaddr *>(&peer), &peerLen);
  pair.server->getsockname(reinterpret_cast<struct sockaddr *>(&local), &localLen);
  KJ_EXPECT(peer.sin_port == local.sin_port);
  KJ_EXPECT(peer.sin_addr.s_addr == local.sin_addr.s_addr);

  // setsockopt/getsockopt round-trip (this is also how setNoDelay-style options are applied).
  int on = 1;
  pair.client->setsockopt(IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
  int result = 0;
  kj::uint resultLen = sizeof(result);
  pair.client->getsockopt(IPPROTO_TCP, TCP_NODELAY, &result, &resultLen);
  KJ_EXPECT(result != 0);
#endif
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
      kj::rc<kj_rs_io::PeerFilter>());
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
      kj::rc<kj_rs_io::PeerFilter>());
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
  KJ_EXPECT(
      strstr(exception.getDescription().cStr(), "refused") != nullptr, exception.getDescription());
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
    KJ_EXPECT(strstr(exception.getDescription().cStr(), "refused") != nullptr,
        exception.getDescription());
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

KJ_TEST("provider pipes (in-memory) and timer work under the tokio loop") {
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
// Unwrap fast path

KJ_TEST("unwrap fast path: recover the native tokio stream and write from Rust") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto pair = makeTcpPair(io);

  // Recover the native tokio TcpStream out of the kj wrapper (free-function form, as a Rust
  // server would after being handed a kj::AsyncIoStream&)...
  auto native = kj_rs_io::unwrapTokioStream(*pair.server);

  // ...the hollow wrapper now refuses I/O...
  kj::byte buffer[32];
  KJ_EXPECT_THROW_MESSAGE("unwrapped", pair.server->tryRead(buffer, 1, sizeof(buffer)).wait(ws));

  // ...and Rust can drive the connection natively: it writes via the tokio readiness API and
  // closes; C++ reads the bytes plus EOF through the (still wrapped) client end.
  auto writeDone = native_write_via_unwrap(kj::mv(native), toRustVec("native write"_kjb));
  KJ_EXPECT(pair.client->tryRead(buffer, 12, sizeof(buffer)).wait(ws) == 12);
  KJ_EXPECT(kj::ArrayPtr<kj::byte>(buffer, 12) == "native write"_kjb);
  writeDone.wait(ws);
  KJ_EXPECT(pair.client->tryRead(buffer, 1, sizeof(buffer)).wait(ws) == 0);  // EOF
}

KJ_TEST("unwrap fast path: Rust-side unwrap_kj_stream() from a "
        "kj::AsyncIoStream&") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto pair = makeTcpPair(io);

  // Rust receives only a kj::AsyncIoStream& and performs the unwrap + native write itself.
  auto writeDone = native_write_via_kj_unwrap(*pair.server, toRust("rust unwrap"_kjb));
  kj::byte buffer[32];
  KJ_EXPECT(pair.client->tryRead(buffer, 11, sizeof(buffer)).wait(ws) == 11);
  KJ_EXPECT(kj::ArrayPtr<kj::byte>(buffer, 11) == "rust unwrap"_kjb);
  writeDone.wait(ws);

  // Unwrapping a foreign (non-kj-rs-io) stream fails cleanly.
  auto pipe = io.getProvider().newTwoWayPipe();
  KJ_EXPECT_THROW_MESSAGE("cannot unwrap", kj_rs_io::unwrapTokioStream(*pipe.ends[0]));
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

KJ_TEST("wrapConnectingSocketFd completes a nonblocking connect") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  auto listener = parseNow(io, "127.0.0.1")->listen();

  int fd;
  KJ_SYSCALL(fd = socket(AF_INET, SOCK_STREAM, 0));
  struct sockaddr_in sin;
  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_port = htons(static_cast<uint16_t>(listener->getPort()));
  sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  auto acceptPromise = listener->accept();
  auto client = io.getLowLevelProvider()
                    .wrapConnectingSocketFd(fd, reinterpret_cast<struct sockaddr *>(&sin),
                        sizeof(sin), kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP)
                    .wait(ws);
  auto server = acceptPromise.wait(ws);

  client->write("connected"_kjb).wait(ws);
  kj::byte buffer[16];
  KJ_EXPECT(server->tryRead(buffer, 9, sizeof(buffer)).wait(ws) == 9);
}

KJ_TEST("wrapInputFd/wrapOutputFd move bytes through an OS pipe and observe EOF") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  int fds[2];
  KJ_SYSCALL(pipe(fds));
  auto input =
      io.getLowLevelProvider().wrapInputFd(fds[0], kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP);
  auto output =
      io.getLowLevelProvider().wrapOutputFd(fds[1], kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP);

  // Blocked read completes once data is written ("pipe fd io" is 10 bytes; cap the first read
  // at 9 so one byte remains).
  kj::byte buffer[16];
  auto readPromise = input->tryRead(buffer, 9, 9);
  KJ_EXPECT(!readPromise.poll(ws));
  auto writePromise = output->write("pipe fd io"_kjb);
  KJ_EXPECT(readPromise.wait(ws) == 9);
  writePromise.wait(ws);
  KJ_EXPECT(input->tryRead(buffer, 1, sizeof(buffer)).wait(ws) == 1);  // "o"

  // Dropping the output stream closes the write end -> EOF.
  output = nullptr;
  KJ_EXPECT(input->tryRead(buffer, 1, sizeof(buffer)).wait(ws) == 0);
}

KJ_TEST("restrictPeers blocks disallowed connect() with KJ's error text") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  kj_rs_io::TokioNetwork network;
  auto restricted = network.restrictPeers({"public"_kj}, {});

  // Loopback is not "public": blocked before any connection attempt.
  auto blockedAddr = restricted->parseAddress("127.0.0.1:1").wait(ws);
  KJ_EXPECT_THROW_MESSAGE("connect() blocked by restrictPeers()", blockedAddr->connect().wait(ws));

  // getSockaddr is rejected eagerly, like KJ.
  struct sockaddr_in sin;
  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_port = htons(1);
  sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  KJ_EXPECT_THROW_MESSAGE(
      "address blocked by restrictPeers()", restricted->getSockaddr(&sin, sizeof(sin)));

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
  // Regression test for a SIGTERM hang observed in workerd: tokio's signal registry is
  // process-global, and whichever runtime's driver consumes the signal's wake byte performs the
  // broadcast. With a second tokio runtime parked on another thread (workerd's inspector thread,
  // in the wild), the broadcast often runs on THAT thread — a cross-thread wake of this loop's
  // waker. Before the kj-rs waker bridge was thread-safe, that wake was lost and workerd ignored
  // SIGTERM until killed; now it must always be delivered (kj-rs/waker.h's cross-thread path).
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
  KJ_DEFER({
    KJ_IF_SOME(fulfiller, *shutdown.lockExclusive()) {
      fulfiller->fulfill();
    }
  });
  // Wait until the other loop is up and parked (and the shutdown fulfiller exists) before
  // raising any signals, so its runtime genuinely participates in the wake-byte race.
  shutdown.when([](auto &maybe) { return maybe != kj::none; }, [](auto &) {});

  // Several rounds, giving each runtime chances to win the wake-byte race. Bounded so a lost
  // wake fails with a diagnosis instead of eating the binary's bazel timeout.
  for (int i = 0; i < 5; i++) {
    auto promise = kj_rs_io::onSignal(SIGUSR2);
    // Pump the loop so the handler is installed before we raise (see the test below).
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
  // Pump the loop so the (cold, first-poll-registered) tokio signal handler is installed
  // before we raise; raising first would take SIGUSR2's default disposition (terminate).
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
// Review-driven hardening tests (re-review of Part2 against the prep-review lens).

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
      // Touch the identity so the allow-all filter is actually built and addRef'd.
      KJ_EXPECT(authed.peerIdentity->toString() != nullptr);
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
  pair.server->setsockopt(SOL_SOCKET, SO_LINGER, &lin, sizeof(lin));
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

#if !_WIN32
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

}  // namespace
}  // namespace kj_rs_io_test
