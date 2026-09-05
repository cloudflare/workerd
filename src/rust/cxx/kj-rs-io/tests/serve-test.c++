// Tests for kj_rs_io::serve_kj_stream (serve.rs): the native-serve entry
// point Rust servers use to drive a kj::AsyncIoStream's connection natively. Following kj-rs
// conventions, C++ KJ_TESTs drive; Rust helpers (tests/serve_helpers.rs) run the echo server
// side over whichever transport path the entry point picks.

#include "io-test-helpers.h"
#include "kj-rs-io-test/lib.rs.h"
#include "kj-rs-io/async-io.h"

#include <kj/array.h>
#include <kj/async-io.h>
#include <kj/async.h>
#include <kj/debug.h>
#include <kj/test.h>

#include <cstring>

namespace kj_rs_io_test {
namespace {

using kj_rs_io::setupTokioAsyncIo;
using kj_rs_io::TokioAsyncIoContext;

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

KJ_TEST("serve_kj_stream pump: the Duplex consumer may be driven from another thread") {
  // ServedKjStream::io's docs promise the Duplex variant is safe to drive off the KJ event-loop
  // thread since the waker bridge became thread-safe. Here the echo consumer runs on its own OS
  // thread and runtime, so every read/write/drop on the Duplex wakes the pump (parked on this
  // KJ loop) cross-thread through the FutureWakerCell. TSAN target.
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto pipe = kj::newTwoWayPipe();

  auto session = start_serve_echo_foreign_thread(kj::mv(pipe.ends[0]));
  KJ_EXPECT(!session->is_native());

  auto data = makePatternedData(512 * 1024, 5);
  auto drive = session->drive();  // drives the pump here; the consumer echoes on its own thread
  auto client = echoRoundTrip(*pipe.ends[1], data);
  kj::joinPromisesFailFast(kj::arr(kj::mv(drive), kj::mv(client))).wait(ws);
}

// A foreign stream over one end of a kj two-way pipe whose read side reports a DISCONNECTED
// failure once the pipe's data is gone (instead of a clean EOF), and whose write side may be
// made to fail DISCONNECTED as well: the shape of an abruptly-reset TCP peer, as seen through a
// non-kj-rs-io kj stream.
class DisconnectingStream final: public kj::AsyncIoStream {
 public:
  DisconnectingStream(kj::Own<kj::AsyncIoStream> inner, bool writesDisconnected)
      : inner(kj::mv(inner)),
        writesDisconnected(writesDisconnected) {}

  kj::Promise<size_t> tryRead(void *buffer, size_t minBytes, size_t maxBytes) override {
    size_t n = co_await inner->tryRead(buffer, minBytes, maxBytes);
    if (n < minBytes) {
      // Where a well-behaved peer would half-close, this one was reset.
      kj::throwFatalException(KJ_EXCEPTION(DISCONNECTED, "peer reset the connection"));
    }
    co_return n;
  }
  kj::Promise<void> write(kj::ArrayPtr<const kj::byte> buffer) override {
    if (writesDisconnected) return KJ_EXCEPTION(DISCONNECTED, "peer reset the connection");
    return inner->write(buffer);
  }
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    if (writesDisconnected) return KJ_EXCEPTION(DISCONNECTED, "peer reset the connection");
    return inner->write(pieces);
  }
  kj::Promise<void> whenWriteDisconnected() override {
    return inner->whenWriteDisconnected();
  }
  void shutdownWrite() override {
    if (writesDisconnected) {
      kj::throwFatalException(KJ_EXCEPTION(DISCONNECTED, "peer reset the connection"));
    }
    inner->shutdownWrite();
  }

 private:
  kj::Own<kj::AsyncIoStream> inner;
  bool writesDisconnected;
};

KJ_TEST("serve_kj_stream pump: a DISCONNECTED read is treated as EOF, not as an error") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto pipe = kj::newTwoWayPipe();

  auto session = start_serve_echo(kj::heap<DisconnectingStream>(kj::mv(pipe.ends[0]), false));
  KJ_EXPECT(!session->is_native());

  auto drive = session->drive();
  // One message goes through, then the client goes away abruptly: the pump's read fails
  // DISCONNECTED. The echo consumer must see EOF (and echo back what it got), and the pump must
  // settle Ok -- abrupt client disconnects are normal load, not failures.
  pipe.ends[1]->write("ping"_kjb).wait(ws);
  kj::byte buffer[4];
  KJ_EXPECT(pipe.ends[1]->tryRead(buffer, 4, 4).wait(ws) == 4);
  pipe.ends[1]->shutdownWrite();  // the wrapper turns this EOF into DISCONNECTED
  KJ_EXPECT(pipe.ends[1]->tryRead(buffer, 1, 1).wait(ws) == 0);  // consumer shut down -> EOF back
  boundedBy(io, kj::mv(drive), 10 * kj::SECONDS, "the pump to settle").wait(ws);
  session->wait_echo_done().wait(ws);
}

KJ_TEST("serve_kj_stream pump: a DISCONNECTED write ends the direction without an error") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto pipe = kj::newTwoWayPipe();

  auto session = start_serve_echo(kj::heap<DisconnectingStream>(kj::mv(pipe.ends[0]), true));
  auto drive = session->drive();
  // The consumer's echo of "ping" is written to a peer that already reset: the pump must not
  // fail. Half-close so the read direction finishes normally too.
  pipe.ends[1]->write("ping"_kjb).wait(ws);
  pipe.ends[1]->shutdownWrite();
  boundedBy(io, kj::mv(drive), 10 * kj::SECONDS, "the pump to settle").wait(ws);
  session->wait_echo_done().wait(ws);
}

KJ_TEST("serve_kj_stream pump: the consumer dropping its end (no shutdown) half-closes the kj "
        "side") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto pipe = kj::newTwoWayPipe();

  // A consumer that reads one message and then simply drops its ServeIo: no shutdown() call.
  auto session = start_serve_drop_consumer(kj::mv(pipe.ends[0]));
  KJ_EXPECT(!session->is_native());
  auto drive = session->drive();

  pipe.ends[1]->write("ping"_kjb).wait(ws);
  // The drop must surface as shutdownWrite() on the kj stream: the client reads EOF.
  kj::byte buffer[1];
  KJ_EXPECT(boundedBy(io, pipe.ends[1]->tryRead(buffer, 1, 1), 10 * kj::SECONDS,
                "EOF from the pump's shutdownWrite")
                .wait(ws) == 0);
  // The kj->consumer direction is still waiting on the client; closing the client end lets the
  // pump finish both directions.
  pipe.ends[1] = nullptr;
  boundedBy(io, kj::mv(drive), 10 * kj::SECONDS, "the pump to settle").wait(ws);
}

// A byte-transforming wrapper over a kj-rs-io TCP stream: XORs everything in both directions.
// The shape of kj::TlsConnection as far as serve_kj_stream is concerned -- it forwards getFd()
// to its transport socket, whose bytes are NOT the stream's bytes.
class XorStream final: public kj::AsyncIoStream {
 public:
  explicit XorStream(kj::Own<kj::AsyncIoStream> inner): inner(kj::mv(inner)) {}

  kj::Promise<size_t> tryRead(void *buffer, size_t minBytes, size_t maxBytes) override {
    size_t n = co_await inner->tryRead(buffer, minBytes, maxBytes);
    auto bytes = reinterpret_cast<kj::byte *>(buffer);
    for (size_t i = 0; i < n; i++) bytes[i] ^= KEY;
    co_return n;
  }
  kj::Promise<void> write(kj::ArrayPtr<const kj::byte> buffer) override {
    auto copy = kj::heapArray<kj::byte>(buffer.size());
    for (size_t i = 0; i < buffer.size(); i++) copy[i] = buffer[i] ^ KEY;
    co_await inner->write(copy);
  }
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    for (auto piece: pieces) co_await write(piece);
  }
  kj::Promise<void> whenWriteDisconnected() override {
    return inner->whenWriteDisconnected();
  }
  void shutdownWrite() override {
    inner->shutdownWrite();
  }
  kj::Maybe<int> getFd() const override {
    return inner->getFd();  // the transport's fd: ciphertext, not this stream's bytes
  }

 private:
  static constexpr kj::byte KEY = 0x5a;
  kj::Own<kj::AsyncIoStream> inner;
};

KJ_TEST("serve_kj_stream pumps a byte-transforming wrapper (TLS-shaped) correctly, never its "
        "fd") {
  // The wrapper exposes its transport's fd, so an fd-tier shortcut would serve the transformed
  // bytes. serve_kj_stream must take the pump path and echo the plaintext the client sees.
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto pair = makeTcpPair(io);

  auto session = start_serve_echo(kj::heap<XorStream>(kj::mv(pair.server)));
  KJ_EXPECT(!session->is_native());

  // The client speaks through its own XorStream, so plaintext round-trips only if the server
  // side was pumped through the wrapper (and not read off the raw socket).
  XorStream client(kj::mv(pair.client));
  auto data = makePatternedData(128 * 1024, 13);
  auto drive = session->drive();
  auto echo = echoRoundTrip(client, data);
  kj::joinPromisesFailFast(kj::arr(kj::mv(drive), kj::mv(echo))).wait(ws);
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

KJ_TEST("take_kj_socket on an already-unwrapped (hollow) kj-rs-io stream is refused") {
  // Unwrap the stream first (leaving the C++ wrapper hollow), then take_kj_socket it: tier 1
  // (unwrap) fails because it is hollow, and tier 2 (fd dup) finds no fd (getFd -> none on a
  // hollow wrapper), so it is refused rather than crashing.
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto pair = makeTcpPair(io);

  // Unwrap + drop the native stream; pair.server is now a hollow wrapper.
  native_write_via_kj_unwrap(*pair.server, ::rust::Slice<const uint8_t>()).wait(ws);

  KJ_EXPECT_THROW_MESSAGE(
      "cannot take the stream's socket natively", start_take_socket_echo(kj::mv(pair.server)));
}

KJ_TEST("take_kj_socket refuses fd-less foreign streams (no pump tier)") {
  auto io = setupTokioAsyncIo();
  auto pipe = kj::newTwoWayPipe();

  KJ_EXPECT_THROW_MESSAGE(
      "cannot take the stream's socket natively", start_take_socket_echo(kj::mv(pipe.ends[0])));
}

}  // namespace
}  // namespace kj_rs_io_test
