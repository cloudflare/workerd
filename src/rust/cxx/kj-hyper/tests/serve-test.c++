// Tests for kj_hyper::serve (serve.rs): serve_kj_stream / take_kj_socket, the native-serve
// entry points hyper's server and stream-tier client use to drive a kj::AsyncIoStream's
// connection natively. Following kj-rs conventions, C++ KJ_TESTs drive; Rust helpers
// (tests/serve_helpers.rs) run the echo server side over whichever transport path the entry
// point picks.

#include "io-test-helpers.h"
#include "kj-hyper-test/lib.rs.h"
#include "kj-rs-io/async-io.h"

#include <kj/array.h>
#include <kj/async-io.h>
#include <kj/async.h>
#include <kj/debug.h>
#include <kj/test.h>

#include <cstring>

namespace kj_hyper_test {
namespace {

using kj_rs_io::setupTokioAsyncIo;

// The client side of an echo round trip: write `data` (in chunks) and concurrently read the
// echo back and verify it (concurrent, so bounded transports -- kj pipes, socket
// buffers -- never deadlock on payloads larger than their buffering); then half-close and
// expect EOF.
kj::Promise<void> echoRoundTrip(
    kj::AsyncIoStream &clientStream, kj::ArrayPtr<const kj::byte> data) {
  static auto constexpr writeThenHalfClose =
      [](kj::AsyncIoStream &s, kj::ArrayPtr<const kj::byte> data) -> kj::Promise<void> {
    co_await writeChunked(s, data);
    s.shutdownWrite();  // half-close: the echo server sees EOF and finishes flushing
  };

  co_await kj::joinPromisesFailFast(
      kj::arr(writeThenHalfClose(clientStream, data), readExact(clientStream, data)));
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

  // Native path: the connection now belongs to the Rust side (the wrapper was consumed by
  // serve_kj_stream).
  auto session = start_serve_echo(kj::mv(pair.server));
  KJ_EXPECT(session->is_native());

  auto data = makePatternedData(256 * 1024, 7);
  auto drive = session->drive();
  auto client = echoRoundTrip(*pair.client, data);
  kj::joinPromisesFailFast(kj::arr(kj::mv(drive), kj::mv(client))).wait(ws);
}

KJ_TEST("take_kj_socket preserves a native stream when extraction is blocked by in-flight I/O") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto pair = makeTcpPair(io);

  kj::Own<kj::AsyncIoStream> recovered;
  {
    kj::byte pendingByte;
    auto pendingRead = pair.server->tryRead(&pendingByte, 1, 1);
    KJ_EXPECT(!pendingRead.poll(ws));

    auto failure = expect_take_socket_failure(kj::mv(pair.server));
    KJ_EXPECT(failure->is_in_flight());
    recovered = failure->take_stream();
  }

  pair.client->write("ok"_kjb).wait(ws);
  kj::byte buffer[2];
  KJ_EXPECT(recovered->tryRead(buffer, 2, 2).wait(ws) == 2);
  KJ_EXPECT(kj::arrayPtr(buffer) == "ok"_kjb);
}

KJ_TEST("serve_kj_stream preserves a native stream when extraction is blocked by in-flight I/O") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto pair = makeTcpPair(io);

  kj::Own<kj::AsyncIoStream> recovered;
  {
    kj::byte pendingByte;
    auto pendingRead = pair.server->tryRead(&pendingByte, 1, 1);
    KJ_EXPECT(!pendingRead.poll(ws));

    auto failure = expect_serve_stream_failure(kj::mv(pair.server));
    KJ_EXPECT(failure->is_in_flight());
    recovered = failure->take_stream();
  }

  pair.client->write("ok"_kjb).wait(ws);
  kj::byte buffer[2];
  KJ_EXPECT(recovered->tryRead(buffer, 2, 2).wait(ws) == 2);
  KJ_EXPECT(kj::arrayPtr(buffer) == "ok"_kjb);
}

// =======================================================================================
// Foreign streams, driven directly

KJ_TEST("serve_kj_stream drives foreign streams: bidirectional echo + half-close") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  // An in-memory kj pipe is the canonical foreign stream: not kj-rs-io-originated.
  auto pipe = kj::newTwoWayPipe();

  auto session = start_serve_echo(kj::mv(pipe.ends[0]));
  KJ_EXPECT(!session->is_native());

  auto data = makePatternedData(512 * 1024, 3);
  auto drive = session->drive();
  auto client = echoRoundTrip(*pipe.ends[1], data);
  // The served stream owns `pipe.ends[0]` now; only the client end stays with the test.
  kj::joinPromisesFailFast(kj::arr(kj::mv(drive), kj::mv(client))).wait(ws);
}

KJ_TEST(
    "serve_kj_stream: dropping the consumer mid-connection destroys the kj stream (drop-abort)") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto pipe = kj::newTwoWayPipe();

  auto session = start_serve_echo(kj::mv(pipe.ends[0]));
  KJ_EXPECT(!session->is_native());

  {
    // Prove the connection is live: one small round trip, then *drop* the drive promise
    // mid-connection, aborting the consumer.
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

  // The aborted consumer has finished...
  session->wait_echo_done().wait(ws);

  // ...and dropped the stream it served, destroying the kj stream: the peer observes teardown
  // (a rejected write), not a zombie half-open pipe.
  auto orphanWrite = kj::evalNow([&]() { return pipe.ends[1]->write("anyone there?"_kjb); });
  KJ_EXPECT(orphanWrite.poll(ws));
  orphanWrite
      .then([]() { KJ_FAIL_EXPECT("write to a torn-down pipe unexpectedly succeeded"); },
          [](kj::Exception &&) {
  }).wait(ws);
}

KJ_TEST("serve_kj_stream: a kj stream polled off its event loop's thread fails, untouched") {
  // A served kj stream is Send (hyper's upgrade path requires it) but belongs to its loop's
  // thread: a consumer on another thread and runtime gets an error on its first operation, and
  // nothing kj runs off-thread.
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto pipe = kj::newTwoWayPipe();

  auto session = start_serve_echo_foreign_thread(kj::mv(pipe.ends[0]));
  KJ_EXPECT(!session->is_native());

  KJ_EXPECT_THROW_MESSAGE("off the thread owning its event loop", session->drive().wait(ws));
}

// A foreign stream over one end of a kj two-way pipe whose read side reports a DISCONNECTED
// failure once the pipe's data is gone (instead of a clean EOF), and whose write side may be
// made to fail DISCONNECTED as well: the shape of an abruptly-reset TCP peer, as seen through a
// non-kj-rs-io kj stream.
class DisconnectingStream final: public kj::AsyncIoStream {
 public:
  // `readFailure` is the exception type the read side fails with at EOF: DISCONNECTED (the
  // reset-peer shape) by default; anything else models a genuine stream error.
  DisconnectingStream(kj::Own<kj::AsyncIoStream> inner,
      bool writesDisconnected,
      kj::Exception::Type readFailure = kj::Exception::Type::DISCONNECTED)
      : inner(kj::mv(inner)),
        writesDisconnected(writesDisconnected),
        readFailure(readFailure) {}

  kj::Promise<size_t> tryRead(void *buffer, size_t minBytes, size_t maxBytes) override {
    size_t n = co_await inner->tryRead(buffer, minBytes, maxBytes);
    if (n < minBytes) {
      // Where a well-behaved peer would half-close, this one failed.
      kj::throwFatalException(kj::Exception(readFailure, __FILE__, __LINE__,
          readFailure == kj::Exception::Type::DISCONNECTED
              ? kj::str("peer reset the connection")
              : kj::str("read side broke: not a disconnect")));
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
  kj::Exception::Type readFailure;
};

KJ_TEST("serve_kj_stream: a DISCONNECTED read is treated as EOF, not as an error") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto pipe = kj::newTwoWayPipe();

  auto session = start_serve_echo(kj::heap<DisconnectingStream>(kj::mv(pipe.ends[0]), false));
  KJ_EXPECT(!session->is_native());

  auto drive = session->drive().eagerlyEvaluate(nullptr);
  // One message goes through, then the client goes away abruptly: the kj read fails
  // DISCONNECTED. The echo consumer must see EOF (and echo back what it got), and finish Ok --
  // abrupt client disconnects are normal load, not failures.
  pipe.ends[1]->write("ping"_kjb).wait(ws);
  kj::byte buffer[4];
  KJ_EXPECT(pipe.ends[1]->tryRead(buffer, 4, 4).wait(ws) == 4);
  pipe.ends[1]->shutdownWrite();  // the wrapper turns this EOF into DISCONNECTED
  KJ_EXPECT(pipe.ends[1]->tryRead(buffer, 1, 1).wait(ws) == 0);  // consumer shut down -> EOF back
  boundedBy(io, kj::mv(drive), 10 * kj::SECONDS, "the consumer to finish").wait(ws);
  session->wait_echo_done().wait(ws);
}

KJ_TEST("serve_kj_stream: a non-DISCONNECTED read failure fails the consumer's read") {
  // Only peer-teardown-shaped failures are EOF; a genuine stream error must reach the consumer
  // (and so drive()), not be swallowed as a clean end of input.
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto pipe = kj::newTwoWayPipe();

  auto session = start_serve_echo(
      kj::heap<DisconnectingStream>(kj::mv(pipe.ends[0]), false, kj::Exception::Type::FAILED));
  auto drive = session->drive().eagerlyEvaluate(nullptr);
  pipe.ends[1]->write("ping"_kjb).wait(ws);
  kj::byte buffer[4];
  KJ_EXPECT(pipe.ends[1]->tryRead(buffer, 4, 4).wait(ws) == 4);
  pipe.ends[1]->shutdownWrite();  // the wrapper turns this EOF into a FAILED exception
  auto failure = KJ_ASSERT_NONNULL(kj::runCatchingExceptions(
      [&]() { boundedBy(io, kj::mv(drive), 10 * kj::SECONDS, "the consumer to fail").wait(ws); }));
  KJ_EXPECT(failure.getDescription().contains("read side broke"), failure.getDescription());
  session->wait_echo_done().wait(ws);
}

KJ_TEST("serve_kj_stream: a DISCONNECTED write ends the direction without an error") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto pipe = kj::newTwoWayPipe();

  auto session = start_serve_echo(kj::heap<DisconnectingStream>(kj::mv(pipe.ends[0]), true));
  auto drive = session->drive().eagerlyEvaluate(nullptr);
  // The consumer's echo of "ping" is written to a peer that already reset: that is a broken
  // pipe, not a failure. Half-close so the read direction finishes normally too.
  pipe.ends[1]->write("ping"_kjb).wait(ws);
  pipe.ends[1]->shutdownWrite();
  boundedBy(io, kj::mv(drive), 10 * kj::SECONDS, "the consumer to finish").wait(ws);
  session->wait_echo_done().wait(ws);
}

KJ_TEST("serve_kj_stream: the consumer dropping its end (no shutdown) closes the kj side") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto pipe = kj::newTwoWayPipe();

  // A consumer that reads one message and then simply drops its ServeIo: no shutdown() call.
  auto session = start_serve_drop_consumer(kj::mv(pipe.ends[0]));
  KJ_EXPECT(!session->is_native());
  auto drive = session->drive().eagerlyEvaluate(nullptr);

  pipe.ends[1]->write("ping"_kjb).wait(ws);
  // The drop destroys the kj stream: the client reads EOF.
  kj::byte buffer[1];
  KJ_EXPECT(boundedBy(io, pipe.ends[1]->tryRead(buffer, 1, 1), 10 * kj::SECONDS,
                "EOF from the destroyed stream")
                .wait(ws) == 0);
  // The peer stays idle and open; the consumer has finished regardless.
  boundedBy(io, kj::mv(drive), 10 * kj::SECONDS, "the consumer to finish with an idle peer")
      .wait(ws);
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

KJ_TEST("serve_kj_stream drives a byte-transforming wrapper (TLS-shaped) correctly, never its "
        "fd") {
  // The wrapper exposes its transport's fd, so an fd-tier shortcut would serve the transformed
  // bytes. serve_kj_stream must drive the wrapper and echo the plaintext the client sees.
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto pair = makeTcpPair(io);

  auto session = start_serve_echo(kj::heap<XorStream>(kj::mv(pair.server)));
  KJ_EXPECT(!session->is_native());

  // The client speaks through its own XorStream, so plaintext round-trips only if the server
  // side went through the wrapper (and not off the raw socket).
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

  // Native socket taken; the kj wrapper was consumed inside take_kj_socket.
  auto session = start_take_socket_echo(kj::mv(pair.server));
  KJ_EXPECT(session->is_native());

  auto data = makePatternedData(256 * 1024, 5);
  auto drive = session->drive();
  auto client = echoRoundTrip(*pair.client, data);
  kj::joinPromisesFailFast(kj::arr(kj::mv(drive), kj::mv(client))).wait(ws);
}

KJ_TEST("take_kj_socket refuses foreign streams") {
  auto io = setupTokioAsyncIo();
  auto pipe = kj::newTwoWayPipe();

  KJ_EXPECT_THROW_MESSAGE(
      "cannot take the stream's socket natively", start_take_socket_echo(kj::mv(pipe.ends[0])));
}

KJ_TEST("dropping a failed extraction's stream with a read in flight is memory-safe") {
  // The realistic error path: a caller propagates the "in flight" TakeSocketError as a plain
  // KjError, which destroys the handed-back stream while a read is still pending on it. The
  // pending read owns its share of the socket, so cancelling it afterwards must touch no freed
  // memory (Rust-instrumented ASan target: --config=asan with -Zsanitizer=address).
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto pair = makeTcpPair(io);

  kj::byte pendingByte;
  auto pendingRead = pair.server->tryRead(&pendingByte, 1, 1);
  KJ_EXPECT(!pendingRead.poll(ws));

  auto description =
      kj::heapString(take_socket_failure_dropping_stream(kj::mv(pair.server)).c_str());
  KJ_EXPECT(description.contains("in flight"), description);
  // The wrapper is gone; the read still owns the socket, so the peer sees no EOF yet...
  kj::byte probe;
  auto peerRead = pair.client->tryRead(&probe, 1, 1);
  KJ_EXPECT(!peerRead.poll(ws));
  // ...and cancelling the read is clean, after which the socket closes and the peer sees EOF.
  pendingRead = nullptr;
  KJ_EXPECT(
      boundedBy(io, kj::mv(peerRead), 10 * kj::SECONDS, "EOF after the last share").wait(ws) == 0);
}

KJ_TEST("an outstanding whenWriteDisconnected() blocks the native serve path") {
  // kj-http holds whenWriteDisconnected() on every served connection, so this -- not a pending
  // read -- is how the "in flight" refusal is met in production.
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto pair = makeTcpPair(io);

  auto disconnectWatch = pair.server->whenWriteDisconnected();
  KJ_EXPECT(!disconnectWatch.poll(ws));
  auto failure = expect_serve_stream_failure(kj::mv(pair.server));
  KJ_EXPECT(failure->is_in_flight());
  auto recovered = failure->take_stream();

  // Cancel the watch: extraction now succeeds and the connection is served natively.
  disconnectWatch = nullptr;
  auto session = start_serve_echo(kj::mv(recovered));
  KJ_EXPECT(session->is_native());
  auto data = makePatternedData(4096, 3);
  auto drive = session->drive();
  auto client = echoRoundTrip(*pair.client, data);
  kj::joinPromisesFailFast(kj::arr(kj::mv(drive), kj::mv(client))).wait(ws);
}

KJ_TEST("serve_kj_stream: a consumer that drops while its write is blocked cancels the write, "
        "as destroying a kj stream does") {
  // The kj peer never reads, so the consumer's write is stuck in the kj stream, and the consumer
  // drops its end mid-write without shutdown(). kj semantics: destroying a kj stream cancels its
  // in-flight write, so the stream is destroyed with the payload never delivered -- the peer
  // observes EOF, not the payload.
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  auto pipe = kj::newTwoWayPipe();

  auto session = start_serve_write_then_drop(kj::mv(pipe.ends[0]));
  KJ_EXPECT(!session->is_native());
  auto drive = session->drive().eagerlyEvaluate(nullptr);
  session->wait_echo_done().wait(ws);  // the consumer has dropped its end
  boundedBy(io, kj::mv(drive), 10 * kj::SECONDS, "the consumer to finish once it dropped").wait(ws);
  // The stream is gone: the peer reads EOF, never the discarded payload.
  kj::byte probe;
  KJ_EXPECT(boundedBy(io, pipe.ends[1]->tryRead(&probe, 1, 1), 10 * kj::SECONDS,
                "EOF from the destroyed stream")
                .wait(ws) == 0);
}

}  // namespace
}  // namespace kj_hyper_test
