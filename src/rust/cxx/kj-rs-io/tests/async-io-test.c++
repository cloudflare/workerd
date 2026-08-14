// Tests for kj-rs-io: tokio-backed implementations of KJ's async I/O interfaces, driven by a
// kj::EventLoop on a TokioEventPort. Following kj-rs conventions, C++ KJ_TESTs drive; Rust
// helpers (tests/lib.rs) provide the native-side behaviors (unwrap fast path, pre-bound fds).

#include "kj-rs-io-test/lib.rs.h"
#include "kj-rs-io/async-io.h"

#include <kj/array.h>
#include <kj/async.h>
#include <kj/debug.h>
#include <kj/test.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <thread>

#if _WIN32
#include <windows.h>  // GetProcessTimes, for the wedge watchdog below.

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
// Helpers

struct ConnectedPair {
  kj::Own<kj::ConnectionReceiver> listener;
  kj::Own<kj::AsyncIoStream> client;
  kj::Own<kj::AsyncIoStream> server;
};

kj::Own<kj::NetworkAddress> parseNow(
    TokioAsyncIoContext &io, kj::StringPtr addr, kj::uint portHint = 0) {
  return io.getNetwork().parseAddress(addr, portHint).wait(io.getWaitScope());
}

ConnectedPair makeTcpPair(TokioAsyncIoContext &io) {
  auto &ws = io.getWaitScope();
  auto listener = parseNow(io, "127.0.0.1")->listen();
  auto connectAddr = parseNow(io, kj::str("127.0.0.1:", listener->getPort()));
  auto acceptPromise = listener->accept();
  auto client = connectAddr->connect().wait(ws);
  auto server = acceptPromise.wait(ws);
  return ConnectedPair{kj::mv(listener), kj::mv(client), kj::mv(server)};
}

kj::Array<kj::byte> makePatternedData(size_t size, kj::byte seed) {
  auto data = kj::heapArray<kj::byte>(size);
  for (size_t i = 0; i < size; i++) {
    data[i] = static_cast<kj::byte>((i * 31 + seed) & 0xff);
  }
  return data;
}

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

// Writes `data` to `out` in chunks (exercising write-all + backpressure).
kj::Promise<void> pumpOut(kj::AsyncIoStream &out, kj::ArrayPtr<const kj::byte> data) {
  constexpr size_t CHUNK = 64 * 1024;
  size_t offset = 0;
  while (offset < data.size()) {
    size_t n = kj::min(CHUNK, data.size() - offset);
    co_await out.write(data.slice(offset, offset + n));
    offset += n;
  }
}

// Reads exactly `expected.size()` bytes from `in` and verifies they match `expected`.
kj::Promise<void> drainAndCheck(kj::AsyncIoStream &in, kj::ArrayPtr<const kj::byte> expected) {
  auto buffer = kj::heapArray<kj::byte>(expected.size());
  size_t total = co_await in.tryRead(buffer.begin(), buffer.size(), buffer.size());
  KJ_ASSERT(total == expected.size(), total, expected.size());
  KJ_ASSERT(memcmp(buffer.begin(), expected.begin(), expected.size()) == 0);
}

// CPU seconds consumed by this process so far. Used by the wedge watchdog to distinguish a
// spinning event loop from one that is parked and never woken.
double processCpuSeconds() {
#if _WIN32
  // clock() is WALL time on Windows (CRT quirk), so use GetProcessTimes.
  FILETIME creationTime, exitTime, kernelTime, userTime;
  GetProcessTimes(GetCurrentProcess(), &creationTime, &exitTime, &kernelTime, &userTime);
  auto toSeconds = [](const FILETIME &ft) {
    return static_cast<double>(
               (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) *
        1e-7;
  };
  return toSeconds(kernelTime) + toSeconds(userTime);
#else
  return static_cast<double>(clock()) / CLOCKS_PER_SEC;
#endif
}

// Waits for `connectPromise` (a connect to a certainly-closed port) to fail and returns the
// exception. Instrumented for a wedge observed (flakily) on Windows CI, where such a connect
// neither succeeded nor failed and a bare wait() silently ate the whole binary's bazel timeout:
//
// - A 30 s KJ timer bounds the wait, so a lost connect-readiness wake fails the test with a
//   message instead. (A pending timer also makes the event port park with a timeout rather than
//   indefinitely, so if the wedge is a lost wake on an indefinite park, the timer tick itself
//   recovers it -- the run then passes, which is a data point too: the one CI run carrying this
//   bound passed while both runs with a bare wait() timed out.)
// - A watchdog thread aborts after 60 s in case the loop stops servicing even timers, reporting
//   process CPU use to distinguish a spinning loop (high) from one parked without wakeups (~0).
kj::Exception expectConnectFailure(
    TokioAsyncIoContext &io, kj::Promise<kj::Own<kj::AsyncIoStream>> connectPromise) {
  std::atomic<bool> done{false};
  std::thread watchdog([&done]() {
    double cpuBefore = processCpuSeconds();
    for (int i = 0; i < 600; i++) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (done.load()) return;
    }
    double cpuUsed = processCpuSeconds() - cpuBefore;
    fprintf(stderr,
        "expectConnectFailure watchdog: the event loop serviced neither the connect nor the 30s "
        "timer for 60s; the process used %.1f CPU-seconds meanwhile (high = loop spinning, ~0 = "
        "parked and never woken). Aborting rather than eating the bazel timeout.\n",
        cpuUsed);
    fflush(stderr);
    abort();
  });
  KJ_DEFER({
    done.store(true);
    watchdog.join();
  });

  auto timeout =
      io.getTimer().afterDelay(30 * kj::SECONDS).then([]() -> kj::Own<kj::AsyncIoStream> {
    KJ_FAIL_ASSERT("connect() to a closed port neither succeeded nor failed within 30s; "
                   "the connect-failure readiness wake was likely lost");
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
  builder.add(pumpOut(*pair.client, dataA));
  builder.add(drainAndCheck(*pair.server, dataA));
  builder.add(pumpOut(*pair.server, dataB));
  builder.add(drainAndCheck(*pair.client, dataB));
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

KJ_TEST("parseAddress resolves hostnames via DNS and connect tries addresses in "
        "order") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  // Listen on IPv4 loopback only. "localhost" typically resolves to both ::1 and 127.0.0.1;
  // connect() must try each in order until one succeeds.
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

}  // namespace
}  // namespace kj_rs_io_test
