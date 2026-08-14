// Tests for kj_rs_io::serve_kj_stream (serve.rs): the native-serve entry
// point Rust servers use to drive a kj::AsyncIoStream's connection natively. Following kj-rs
// conventions, C++ KJ_TESTs drive; Rust helpers (tests/serve_helpers.rs) run the echo server
// side over whichever transport path the entry point picks.

#include "kj-rs-io-test/lib.rs.h"
#include "kj-rs-io/async-io.h"

#include <kj/array.h>
#include <kj/async-io.h>
#include <kj/async.h>
#include <kj/debug.h>
#include <kj/test.h>

#include <cstring>

#if !_WIN32
#include <unistd.h>  // getpid()/unlink() for the unix-socket pair helper
#endif

namespace kj_rs_io_test {
namespace {

using kj_rs_io::setupTokioAsyncIo;
using kj_rs_io::TokioAsyncIoContext;

struct ConnectedPair {
  kj::Own<kj::ConnectionReceiver> listener;
  kj::Own<kj::AsyncIoStream> client;
  kj::Own<kj::AsyncIoStream> server;
};

ConnectedPair makeTcpPair(TokioAsyncIoContext &io) {
  auto &ws = io.getWaitScope();
  auto listener = io.getNetwork().parseAddress("127.0.0.1").wait(ws)->listen();
  auto connectAddr =
      io.getNetwork().parseAddress(kj::str("127.0.0.1:", listener->getPort())).wait(ws);
  auto acceptPromise = listener->accept();
  auto client = connectAddr->connect().wait(ws);
  auto server = acceptPromise.wait(ws);
  return ConnectedPair{kj::mv(listener), kj::mv(client), kj::mv(server)};
}

#if !_WIN32
// A connected AF_UNIX stream-socket pair, from the kj-rs-io network's `unix:` support. Used to
// prove take_kj_socket serves a non-TCP fd. Binds a short /tmp path (unlinked before bind to
// clear stale sockets, and
// again once connected) unique per process + call, so parallel/repeated runs never collide.
ConnectedPair makeUnixPair(TokioAsyncIoContext &io) {
  auto &ws = io.getWaitScope();
  static uint counter = 0;
  auto path = kj::str("/tmp/kj-rs-io-serve-", ::getpid(), "-", counter++, ".sock");
  ::unlink(path.cStr());
  auto addr = kj::str("unix:", path);
  auto listener = io.getNetwork().parseAddress(addr).wait(ws)->listen();
  auto connectAddr = io.getNetwork().parseAddress(addr).wait(ws);
  auto acceptPromise = listener->accept();
  auto client = connectAddr->connect().wait(ws);
  auto server = acceptPromise.wait(ws);
  // The bound path is no longer needed once both ends are connected.
  ::unlink(path.cStr());
  return ConnectedPair{kj::mv(listener), kj::mv(client), kj::mv(server)};
}
#endif  // !_WIN32

kj::Array<kj::byte> makePatternedData(size_t size, kj::byte seed) {
  auto data = kj::heapArray<kj::byte>(size);
  for (size_t i = 0; i < size; i++) {
    data[i] = static_cast<kj::byte>((i * 31 + seed) & 0xff);
  }
  return data;
}

// The client side of an echo round trip: write `data` (in chunks) and concurrently read the
// echo back and verify it (concurrent, so bounded transports -- the pump duplex, socket
// buffers -- never deadlock on payloads larger than their buffering); then half-close and
// expect EOF.
kj::Promise<void> echoRoundTrip(
    kj::AsyncIoStream &clientStream, kj::ArrayPtr<const kj::byte> data) {
  static auto constexpr readBack = [](kj::AsyncIoStream &s,
                                       kj::ArrayPtr<const kj::byte> expected) -> kj::Promise<void> {
    auto buffer = kj::heapArray<kj::byte>(expected.size());
    size_t total = co_await s.tryRead(buffer.begin(), buffer.size(), buffer.size());
    KJ_ASSERT(total == expected.size(), total, expected.size());
    KJ_ASSERT(memcmp(buffer.begin(), expected.begin(), expected.size()) == 0);
  };

  static auto constexpr writeAll = [](kj::AsyncIoStream &s,
                                       kj::ArrayPtr<const kj::byte> data) -> kj::Promise<void> {
    constexpr size_t CHUNK = 64 * 1024;
    size_t offset = 0;
    while (offset < data.size()) {
      size_t n = kj::min(CHUNK, data.size() - offset);
      co_await s.write(data.slice(offset, offset + n));
      offset += n;
    }
    s.shutdownWrite();  // half-close: the echo server sees EOF and finishes flushing
  };

  co_await kj::joinPromisesFailFast(
      kj::arr(writeAll(clientStream, data), readBack(clientStream, data)));
  kj::byte extra;
  KJ_EXPECT(co_await clientStream.tryRead(&extra, 1, 1) == 0);  // EOF after echo completes
}

// =======================================================================================
// Unwrap fast path

KJ_TEST("serve_kj_stream takes the native path for kj-rs-io TCP streams and "
        "echoes") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto pair = makeTcpPair(io);

  // Native path: the connection now belongs to the Rust side (the hollow wrapper was
  // destroyed by serve_kj_stream).
  auto session = start_serve_echo(kj::mv(pair.server));
  KJ_EXPECT(session->is_native());

  auto data = makePatternedData(256 * 1024, 7);
  auto drive = session->drive();
  auto client = echoRoundTrip(*pair.client, data);
  kj::joinPromisesFailFast(kj::arr(kj::mv(drive), kj::mv(client))).wait(ws);
}

// =======================================================================================
// Duplex pump fallback (foreign streams)

KJ_TEST("serve_kj_stream pumps foreign streams: bidirectional echo + half-close") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  // An in-memory kj pipe is the canonical foreign stream: not kj-rs-io-originated.
  auto pipe = kj::newTwoWayPipe();

  auto session = start_serve_echo(kj::mv(pipe.ends[0]));
  KJ_EXPECT(!session->is_native());

  auto data = makePatternedData(512 * 1024, 3);
  auto drive = session->drive();
  auto client = echoRoundTrip(*pipe.ends[1], data);
  // The pump owns `pipe.ends[0]` now; only the client end stays with the test.
  kj::joinPromisesFailFast(kj::arr(kj::mv(drive), kj::mv(client))).wait(ws);
}

KJ_TEST("serve_kj_stream pump: dropping the pump aborts the bridge (drop-abort)") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto pipe = kj::newTwoWayPipe();

  auto session = start_serve_echo(kj::mv(pipe.ends[0]));
  KJ_EXPECT(!session->is_native());

  {
    // Prove the bridge is live: one small round trip, driving the pump only while the
    // client operation runs, then *drop* the drive promise mid-connection.
    auto drive = session->drive();
    auto oneRoundTrip = [](kj::AsyncIoStream &s) -> kj::Promise<void> {
      co_await s.write("ping"_kjb);
      kj::byte buffer[4];
      size_t n = co_await s.tryRead(buffer, 4, 4);
      KJ_ASSERT(n == 4);
      KJ_ASSERT(memcmp(buffer, "ping", 4) == 0);
    }(*pipe.ends[1]);
    // exclusiveJoin: when the round trip finishes, `drive` is cancelled (dropped).
    oneRoundTrip.exclusiveJoin(kj::mv(drive)).wait(ws);
  }

  // Dropping the pump dropped the kj-side duplex end: the echo consumer reads EOF and
  // finishes...
  session->wait_echo_done().wait(ws);

  // ...and the pump destroyed the kj stream it owned: the peer observes teardown (a rejected
  // write), not a zombie half-open pipe.
  auto orphanWrite = kj::evalNow([&]() { return pipe.ends[1]->write("anyone there?"_kjb); });
  KJ_EXPECT(orphanWrite.poll(ws));
  orphanWrite
      .then([]() { KJ_FAIL_EXPECT("write to a torn-down pipe unexpectedly succeeded"); },
          [](kj::Exception &&) {
  }).wait(ws);
}

// =======================================================================================
// take_kj_socket (native-only: unwrap tier, else fd-dup tier)

KJ_TEST("take_kj_socket unwraps kj-rs-io TCP streams (tier 1) and echoes") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto pair = makeTcpPair(io);

  // Native socket taken; the (hollow) kj stream was destroyed inside take_kj_socket.
  auto session = start_take_socket_echo(kj::mv(pair.server));
  KJ_EXPECT(session->is_native());

  auto data = makePatternedData(256 * 1024, 5);
  auto drive = session->drive();
  auto client = echoRoundTrip(*pair.client, data);
  kj::joinPromisesFailFast(kj::arr(kj::mv(drive), kj::mv(client))).wait(ws);
}

#if !_WIN32
// A foreign kj::AsyncIoStream that exposes only its OS fd: unwrap must fail (it is not a
// kj-rs-io wrapper) and any per-read FFI would abort the test -- proving take_kj_socket's fd
// tier does all its I/O on the dup'd socket, never through the kj stream.
class FdOnlyStream final: public kj::AsyncIoStream {
 public:
  explicit FdOnlyStream(kj::Own<kj::AsyncIoStream> inner): inner(kj::mv(inner)) {}

  kj::Maybe<int> getFd() const override {
    return inner->getFd();
  }

  kj::Promise<size_t> tryRead(void *, size_t, size_t) override {
    KJ_UNIMPLEMENTED("FdOnlyStream must not be read through the FFI");
  }
  kj::Promise<void> write(kj::ArrayPtr<const kj::byte>) override {
    KJ_UNIMPLEMENTED("FdOnlyStream must not be written through the FFI");
  }
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>>) override {
    KJ_UNIMPLEMENTED("FdOnlyStream must not be written through the FFI");
  }
  kj::Promise<void> whenWriteDisconnected() override {
    KJ_UNIMPLEMENTED("FdOnlyStream must not be observed through the FFI");
  }
  void shutdownWrite() override {
    KJ_UNIMPLEMENTED("FdOnlyStream must not be shut down through the FFI");
  }

 private:
  kj::Own<kj::AsyncIoStream> inner;
};

KJ_TEST("take_kj_socket dups the fd of foreign fd-backed streams (tier 2); the "
        "original stream may be dropped") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto pair = makeTcpPair(io);

  // Hide the kj-rs-io origin behind a foreign wrapper: only getFd() is reachable.
  auto foreign = kj::heap<FdOnlyStream>(kj::mv(pair.server));

  // Fd tier: the dup is independent; take_kj_socket destroys the wrapper (and with it the
  // original socket's owner) before returning, which must not tear the served connection down.
  auto session = start_take_socket_echo(kj::mv(foreign));
  KJ_EXPECT(session->is_native());

  auto data = makePatternedData(256 * 1024, 9);
  auto drive = session->drive();
  auto client = echoRoundTrip(*pair.client, data);
  kj::joinPromisesFailFast(kj::arr(kj::mv(drive), kj::mv(client))).wait(ws);
}

// A unix-domain (AF_UNIX) socket must be served natively too: take_kj_socket's fd tier dups the
// fd and detects the family (getsockname), producing a tokio UnixStream (ServeIo::Unix).
KJ_TEST("take_kj_socket serves a unix-domain (AF_UNIX) socket via its fd tier and echoes") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto pair = makeUnixPair(io);

  // Hide the kj-rs-io origin behind a foreign wrapper so take_kj_socket must use its fd tier
  // (dup + family detection), not the unwrap fast path -- proving the AF_UNIX -> UnixStream
  // branch of serve_io_from_owned_fd.
  auto foreign = kj::heap<FdOnlyStream>(kj::mv(pair.server));

  auto session = start_take_socket_echo(kj::mv(foreign));
  KJ_EXPECT(session->is_native());  // ServeIo::Unix is a native path (no pump)

  auto data = makePatternedData(256 * 1024, 11);
  auto drive = session->drive();
  auto client = echoRoundTrip(*pair.client, data);
  kj::joinPromisesFailFast(kj::arr(kj::mv(drive), kj::mv(client))).wait(ws);
}
#endif  // !_WIN32

KJ_TEST("take_kj_socket refuses fd-less foreign streams (no pump tier)") {
  auto io = setupTokioAsyncIo();
  auto pipe = kj::newTwoWayPipe();

  KJ_EXPECT_THROW_MESSAGE(
      "cannot take the stream's socket natively", start_take_socket_echo(kj::mv(pipe.ends[0])));
}

}  // namespace
}  // namespace kj_rs_io_test
