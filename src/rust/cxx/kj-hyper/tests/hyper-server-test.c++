// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// End-to-end tests for the hyper-backed inbound HTTP server, in workerd's production shape: a
// C++-owned accept loop hands each accepted connection to a HyperHttpConnection, which
// dispatches every request to a C++ kj::HttpService running on the test's KJ event loop.
// Requests are made over real loopback TCP with both a plain kj::HttpClient and (cross-test)
// the hyper-backed outbound client, asserting the same behavior through both stacks.

#include "kj-hyper/tests/test-harness.h"

#include <kj-hyper/hyper-http.h>
#include <kj-rs-io/async-io.h>

#include <kj/async-io.h>
#include <kj/compat/http.h>
#include <kj/debug.h>
#include <kj/test.h>
#include <kj/time.h>
#include <kj/vector.h>

#include <cstring>

// The whole suite serves connections through newHyperHttpConnection(), which takes the accepted
// socket natively; like the single-connection tests that preceded this shape, it is not
// exercised on Windows.
#if !_WIN32

namespace {

using namespace kj_hyper_test;

void ensureTokioInitialized() {
  static bool initialized = []() { return true; }();
  (void)initialized;
}

// Tolerates (but does not require) ERROR logs mentioning "disconnected" while in scope. When the
// server aborts a connection mid-body, the kj client may log a recoverable DISCONNECTED
// exception raised during unwind, depending on how the RST races the reads; kj-test would treat
// that log as a failure.
class TolerateDisconnectErrorLogs: public kj::ExceptionCallback {
 public:
  void logMessage(kj::LogSeverity severity,
      const char* file,
      int line,
      int contextDepth,
      kj::String&& text) override {
    if (severity == kj::LogSeverity::ERROR && strstr(text.cStr(), "disconnected") != nullptr) {
      return;
    }
    ExceptionCallback::logMessage(severity, file, line, contextDepth, kj::mv(text));
  }
};

// The service under test: a C++ kj::HttpService with path-dispatched, controllable behaviors.

class TestHttpService final: public kj::HttpService {
 public:
  TestHttpService(kj::HttpHeaderTable& table,
      kj::HttpHeaderId xEcho,
      kj::HttpHeaderId xBinary,
      kj::HttpHeaderId xEchoBack,
      kj::HttpHeaderId xBinaryResp,
      kj::HttpHeaderId xUrl)
      : table(table),
        xEcho(xEcho),
        xBinary(xBinary),
        xEchoBack(xEchoBack),
        xBinaryResp(xBinaryResp),
        xUrl(xUrl) {}

  uint requestCount = 0;
  uint canceledCount = 0;
  bool inHang = false;
  // When set, /gate requests wait on this before responding.
  kj::Maybe<kj::Promise<void>> gate;
  // The exact Sec-WebSocket-Extensions value the last WebSocket request carried, if any.
  kj::Maybe<kj::String> lastWsExtensions;
  // Set by /ws-abort-watch when whenAborted() resolves / receive() fails DISCONNECTED.
  bool wsAborted = false;
  kj::Maybe<kj::Exception> lastWsError;

  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      Response& response) override {
    ++requestCount;

    if (headers.isWebSocket()) {
      lastWsExtensions = kj::none;
      KJ_IF_SOME(ext, headers.get(kj::HttpHeaderId::SEC_WEBSOCKET_EXTENSIONS)) {
        lastWsExtensions = kj::str(ext);
      }
      if (url == "/ws-echo") {
        kj::HttpHeaders respHeaders(table);
        respHeaders.addPtr("X-Ws-Accepted", kj::str("yes"));
        auto ws = response.acceptWebSocket(respHeaders);
        co_await wsEcho(*ws);
        co_return;
      } else if (url == "/ws-echo-compress") {
        // Mirror workerd's api/http.c++ MANUAL_COMPRESSION pattern: the application passes the
        // request's extension offer into acceptWebSocket()'s headers, and the (hyper) server
        // computes the agreement from it like kj does.
        kj::HttpHeaders respHeaders(table);
        KJ_IF_SOME(ext, headers.get(kj::HttpHeaderId::SEC_WEBSOCKET_EXTENSIONS)) {
          respHeaders.set(kj::HttpHeaderId::SEC_WEBSOCKET_EXTENSIONS, kj::str(ext));
        }
        auto ws = response.acceptWebSocket(respHeaders);
        co_await wsEcho(*ws);
        co_return;
      } else if (url == "/ws-limit") {
        // Accept, then receive with a tiny maxSize; the client's oversized message must fail
        // with kj's 1009 semantics.
        kj::HttpHeaders respHeaders(table);
        auto ws = response.acceptWebSocket(respHeaders);
        lastWsError = kj::none;
        co_await ws->receive(1024).then([](kj::WebSocket::Message&&) {
          KJ_FAIL_ASSERT("expected the oversized receive to fail");
        }, [this](kj::Exception&& e) { lastWsError = kj::mv(e); });
        co_return;
      } else if (url == "/ws-abort-watch") {
        // Accept, then wait for the peer to abort: both whenAborted() and the pending receive
        // must observe it.
        kj::HttpHeaders respHeaders(table);
        auto ws = response.acceptWebSocket(respHeaders);
        auto aborted = ws->whenAborted();
        lastWsError = kj::none;
        co_await ws->receive().then([](kj::WebSocket::Message&&) {
          KJ_FAIL_ASSERT("expected receive() to fail after the peer aborts");
        }, [this](kj::Exception&& e) { lastWsError = kj::mv(e); });
        co_await aborted;
        wsAborted = true;
        co_return;
      }
      // Fall through: treat as a regular request.
    }

    if (url.startsWith("/hello")) {
      kj::HttpHeaders respHeaders(table);
      respHeaders.set(kj::HttpHeaderId::CONTENT_TYPE, kj::str("text/plain"));
      respHeaders.set(xUrl, kj::str(url));
      respHeaders.addPtr("X-Method", kj::str(method));
      auto body = "Hello from KJ service!"_kj;
      auto out = response.send(200, "Everything Is Fine", respHeaders, body.size());
      co_await out->write(body.asBytes());
    } else if (url == "/echo") {
      // Capture the advertised length before reading: entity-body streams report the
      // *remaining* length, which drops as the body is consumed.
      auto maybeLen = requestBody.tryGetLength();
      auto body = co_await requestBody.readAllBytes();
      kj::HttpHeaders respHeaders(table);
      KJ_IF_SOME(len, maybeLen) {
        respHeaders.addPtr("X-Request-Length", kj::str(len));
      } else {
        respHeaders.add("X-Request-Length", "unknown");
      }
      // No expected size: the response goes out chunked.
      auto out = response.send(200, "OK", respHeaders, kj::none);
      co_await out->write(body);
    } else if (url == "/headers") {
      kj::HttpHeaders respHeaders(table);
      KJ_IF_SOME(echo, headers.get(xEcho)) {
        respHeaders.set(xEchoBack, kj::str(echo));
      }
      KJ_IF_SOME(bin, headers.get(xBinary)) {
        // Echo the (possibly non-UTF-8) bytes back verbatim.
        respHeaders.set(xBinaryResp, kj::str(bin));
      }
      KJ_IF_SOME(host, headers.get(kj::HttpHeaderId::HOST)) {
        respHeaders.addPtr("X-Host-Back", kj::str(host));
      }
      kj::Vector<kj::String> multi;
      headers.forEach([&](kj::StringPtr name, kj::StringPtr value) {
        if (asciiEqualsIgnoreCase(name, "x-multi")) multi.add(kj::str(value));
      });
      respHeaders.addPtr("X-Multi-Back", kj::strArray(multi, ","));
      respHeaders.add("Set-Cookie", "a=1");
      respHeaders.add("Set-Cookie", "b=2");
      kj::byte binary[] = {'v', 0x80, 0xFF, 0xFE, 'z'};
      respHeaders.addPtr(
          "X-Binary-Value", kj::heapString(kj::arrayPtr(binary, sizeof(binary)).asChars()));
      auto out = response.send(200, "OK", respHeaders, size_t(2));
      co_await out->write("ok"_kjb);
    } else if (url == "/large") {
      kj::HttpHeaders respHeaders(table);
      auto out = response.send(200, "OK", respHeaders, LARGE_BODY_SIZE);
      auto buf = kj::heapArray<kj::byte>(CHUNK_SIZE);
      for (uint64_t offset = 0; offset < LARGE_BODY_SIZE; offset += CHUNK_SIZE) {
        for (size_t i = 0; i < CHUNK_SIZE; i++) {
          buf[i] = patternByte(offset + i);
        }
        co_await out->write(buf);
      }
    } else if (url == "/drain-body") {
      // Stream-consume the request body without buffering it.
      auto buf = kj::heapArray<kj::byte>(CHUNK_SIZE);
      uint64_t total = 0;
      for (;;) {
        auto n = co_await requestBody.tryRead(buf.begin(), 1, buf.size());
        if (n == 0) break;
        for (size_t i = 0; i < n; i++) {
          KJ_ASSERT(buf[i] == patternByte(total + i), "request body corruption", total + i);
        }
        total += n;
      }
      kj::HttpHeaders respHeaders(table);
      auto body = kj::str(total);
      auto out = response.send(200, "OK", respHeaders, body.size());
      co_await out->write(body.asBytes());
    } else if (url == "/no-content") {
      kj::HttpHeaders respHeaders(table);
      response.send(204, "No Content", respHeaders, kj::none);
    } else if (url == "/hang") {
      // Send headers and a little data, then hang until canceled. Observably reports
      // cancellation (the client disconnecting must cancel this promise).
      kj::HttpHeaders respHeaders(table);
      auto out = response.send(200, "OK", respHeaders, kj::none);
      co_await out->write("partial"_kjb);
      inHang = true;
      KJ_DEFER({
        inHang = false;
        ++canceledCount;
      });
      co_await kj::Promise<void>(kj::NEVER_DONE);
    } else if (url == "/hang-before-send") {
      // Hang before sending any response, until canceled.
      inHang = true;
      KJ_DEFER({
        inHang = false;
        ++canceledCount;
      });
      co_await kj::Promise<void>(kj::NEVER_DONE);
    } else if (url == "/gate") {
      // Wait for the test to open the gate, then respond.
      KJ_IF_SOME(g, gate) {
        auto promise = kj::mv(g);
        gate = kj::none;
        co_await promise;
      }
      kj::HttpHeaders respHeaders(table);
      auto body = "gated response"_kj;
      auto out = response.send(200, "OK", respHeaders, body.size());
      co_await out->write(body.asBytes());
    } else if (url == "/throw") {
      KJ_FAIL_ASSERT("the test service deliberately failed");
    } else if (url == "/throw-unimplemented") {
      KJ_UNIMPLEMENTED("the test service does not implement this");
    } else if (url == "/throw-disconnected") {
      kj::throwFatalException(KJ_EXCEPTION(DISCONNECTED, "the test service disconnected"));
    } else if (url == "/throw-mid-body") {
      kj::HttpHeaders respHeaders(table);
      auto out = response.send(200, "OK", respHeaders, kj::none);
      co_await out->write("partial"_kjb);
      KJ_FAIL_ASSERT("the test service deliberately failed mid-body");
    } else if (url == "/no-response") {
      // Return without calling send().
      co_return;
    } else if (url == "/accept-websocket") {
      kj::HttpHeaders respHeaders(table);
      auto ws = response.acceptWebSocket(respHeaders);
      KJ_FAIL_ASSERT("acceptWebSocket unexpectedly succeeded");
    } else {
      kj::HttpHeaders respHeaders(table);
      auto out = response.send(404, "Not Found", respHeaders, size_t(0));
    }
    co_return;
  }

  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::HttpConnectSettings settings) override {
    // The hyper server passes kj::HttpServer's default settings.
    KJ_EXPECT(!settings.useTls);
    if (host == "echo-host:1234") {
      kj::HttpHeaders respHeaders(table);
      respHeaders.addPtr("X-Tunnel", kj::str("ok"));
      response.accept(200, "OK", respHeaders);
      auto buf = kj::heapArray<kj::byte>(4096);
      for (;;) {
        auto n = co_await connection.tryRead(buf.begin(), 1, buf.size());
        if (n == 0) break;
        co_await connection.write(buf.first(n));
      }
      connection.shutdownWrite();
    } else if (host == "reject-host:99") {
      kj::HttpHeaders respHeaders(table);
      auto body = "tunnel rejected"_kj;
      auto out = response.reject(403, "Forbidden", respHeaders, uint64_t(body.size()));
      co_await out->write(body.asBytes());
    } else {
      KJ_UNIMPLEMENTED("no tunnel for this host");
    }
  }

 private:
  kj::HttpHeaderTable& table;
  kj::HttpHeaderId xEcho;
  kj::HttpHeaderId xBinary;
  kj::HttpHeaderId xEchoBack;
  kj::HttpHeaderId xBinaryResp;
  kj::HttpHeaderId xUrl;
};

// Deterministic entropy for the kj client's Sec-WebSocket-Key / frame masks.
class FakeEntropySource final: public kj::EntropySource {
 public:
  void generate(kj::ArrayPtr<kj::byte> buffer) override {
    for (auto& b: buffer) {
      b = counter++;
      if (b == 0) b = counter++;  // avoid the all-zero mask, which means "unmasked" to kj
    }
  }

 private:
  kj::byte counter = 17;
};

// The production serving shape: a C++-owned accept loop (as in workerd's Server::HttpListener)
// handing each accepted connection to its own HyperHttpConnection. Provides the
// listenHttp()/drain() surface the fixtures drive, with kj::HttpServer-like drain semantics:
// drain() stops accepting, finishes in-flight requests (Connection: close), then resolves.
class TestServer {
 public:
  TestServer(TokioTestIo& io, const kj::HttpHeaderTable& table, kj::HttpService& service)
      : table(table),
        service(service),
        listener(
            io.provider->getNetwork().parseAddress("127.0.0.1", 0).wait(io.waitScope)->listen()),
        drainPaf(kj::newPromiseAndFulfiller<void>()),
        drainRequested(drainPaf.promise.fork()),
        donePaf(kj::newPromiseAndFulfiller<void>()),
        done(donePaf.promise.fork()) {}

  uint getPort() {
    return KJ_ASSERT_NONNULL(listener)->getPort();
  }

  // Accept and serve connections until drain(). Resolves once draining finished, including all
  // in-flight serves.
  kj::Promise<void> listenHttp() {
    listening = true;
    for (;;) {
      auto accepted =
          co_await KJ_ASSERT_NONNULL(listener)
              ->accept()
              .then([](kj::Own<kj::AsyncIoStream> s) -> kj::Maybe<kj::Own<kj::AsyncIoStream>> {
        return kj::mv(s);
      })
              .exclusiveJoin(drainRequested.addBranch().then(
                  []() -> kj::Maybe<kj::Own<kj::AsyncIoStream>> { return kj::none; }));
      KJ_IF_SOME(stream, accepted) {
        // The stream's socket is taken natively; the handoff consumes (and destroys) the kj
        // stream object, mirroring workerd's HttpListener.
        auto conn = workerd::rust::kj_hyper::newHyperHttpConnection(table, service, kj::mv(stream));
        auto serveTask = conn->serve().eagerlyEvaluate([](kj::Exception&& e) { KJ_LOG(ERROR, e); });
        connections.add(Entry{kj::mv(conn), kj::mv(serveTask)});
      } else {
        break;  // draining
      }
    }
    // Refuse new connections, then let every connection finish its in-flight request.
    listener = kj::none;
    auto conns = kj::mv(connections);
    for (auto& c: conns) c.handle->shutdown();
    for (auto& c: conns) co_await c.serveTask;
    donePaf.fulfiller->fulfill();
  }

  // Stop accepting, let in-flight requests finish, then resolve. Resolves immediately if
  // listenHttp() was never called.
  kj::Promise<void> drain() {
    if (!draining) {
      draining = true;
      drainPaf.fulfiller->fulfill();
    }
    if (!listening) co_return;
    co_await done.addBranch();
  }

 private:
  struct Entry {
    kj::Own<workerd::rust::kj_hyper::HyperHttpConnection> handle;
    kj::Promise<void> serveTask;
  };

  const kj::HttpHeaderTable& table;
  kj::HttpService& service;
  kj::Maybe<kj::Own<kj::ConnectionReceiver>> listener;
  kj::PromiseFulfillerPair<void> drainPaf;
  kj::ForkedPromise<void> drainRequested;
  kj::PromiseFulfillerPair<void> donePaf;
  kj::ForkedPromise<void> done;
  bool listening = false;
  bool draining = false;
  kj::Vector<Entry> connections;
};

struct TestFixture {
  TestFixture() {
    ensureTokioInitialized();

    service = kj::heap<TestHttpService>(
        *ids.table, ids.xEcho, ids.xBinary, ids.xEchoBack, ids.xBinaryResp, ids.xUrl);

    server = kj::heap<TestServer>(io, *ids.table, *service);
    listenTask = server->listenHttp().eagerlyEvaluate([](kj::Exception&& e) { KJ_LOG(ERROR, e); });

    address =
        io.provider->getNetwork().parseAddress("127.0.0.1", server->getPort()).wait(io.waitScope);
    // Mirror workerd's outbound client settings: entropy for WebSocket keys/masks, and
    // MANUAL_COMPRESSION (the application negotiates Sec-WebSocket-Extensions itself).
    client = kj::newHttpClient(io.provider->getTimer(), *ids.table, *address,
        {.entropySource = entropy,
          .webSocketCompressionMode = kj::HttpClientSettings::MANUAL_COMPRESSION});
  }

  ~TestFixture() noexcept(false) {
    // Quiesce before tearing down the KJ event loop: drain() waits until every connection task
    // has exited and the dispatchers have finished, so no cross-thread wakes from the tokio
    // side race the EventLoop destructor.
    client = nullptr;
    server->drain().wait(io.waitScope);
  }

  kj::HttpHeaders makeHeaders() {
    kj::HttpHeaders headers(*ids.table);
    headers.set(kj::HttpHeaderId::HOST, kj::str("test-host"));
    return headers;
  }

  kj::String get(kj::StringPtr url, uint expectedStatus = 200) {
    auto req = client->request(kj::HttpMethod::GET, url, makeHeaders(), uint64_t(0));
    auto resp = req.response.wait(io.waitScope);
    KJ_EXPECT(resp.statusCode == expectedStatus, resp.statusCode, url);
    return resp.body->readAllText().wait(io.waitScope);
  }

  // Run the KJ loop until `condition` is true (with a rough time budget). Cross-thread wakes
  // from the tokio side land as events on this loop.
  void spinUntil(kj::Function<bool()> condition, uint maxMs = 5000) {
    for (uint elapsed = 0; elapsed < maxMs; elapsed++) {
      if (condition()) return;
      io.provider->getTimer().afterDelay(1 * kj::MILLISECONDS).wait(io.waitScope);
    }
    KJ_FAIL_ASSERT("condition not reached in time");
  }

  TokioTestIo io;
  HeaderIds ids;
  FakeEntropySource entropy;
  kj::Own<TestHttpService> service;
  kj::Own<TestServer> server;
  kj::Promise<void> listenTask = nullptr;
  kj::Own<kj::NetworkAddress> address;
  kj::Own<kj::HttpClient> client;
};

KJ_TEST("hyper server: getPort assigns an ephemeral port") {
  TestFixture f;
  KJ_EXPECT(f.server->getPort() != 0);
}

KJ_TEST("hyper server: GET round trip") {
  TestFixture f;

  auto req = f.client->request(kj::HttpMethod::GET, "/hello?foo=bar", f.makeHeaders(), uint64_t(0));
  auto resp = req.response.wait(f.io.waitScope);

  KJ_EXPECT(resp.statusCode == 200);
  // Non-canonical status text passes through to the wire.
  KJ_EXPECT(resp.statusText == "Everything Is Fine", resp.statusText);
  // Builtin indexed response header.
  KJ_EXPECT(KJ_ASSERT_NONNULL(resp.headers->get(kj::HttpHeaderId::CONTENT_TYPE)) == "text/plain");
  // The URL (path + query) reached the service verbatim.
  KJ_EXPECT(KJ_ASSERT_NONNULL(resp.headers->get(f.ids.xUrl)) == "/hello?foo=bar");
  KJ_EXPECT(KJ_ASSERT_NONNULL(getHeader(*resp.headers, "X-Method")) == "GET");
  // expectedBodySize became Content-Length.
  KJ_EXPECT(KJ_ASSERT_NONNULL(resp.body->tryGetLength()) == 22);
  auto body = resp.body->readAllText().wait(f.io.waitScope);
  KJ_EXPECT(body == "Hello from KJ service!", body);
}

KJ_TEST("hyper server: HEAD request advertises Content-Length with empty body") {
  TestFixture f;

  auto req = f.client->request(kj::HttpMethod::HEAD, "/hello", f.makeHeaders(), uint64_t(0));
  auto resp = req.response.wait(f.io.waitScope);

  KJ_EXPECT(resp.statusCode == 200);
  KJ_EXPECT(KJ_ASSERT_NONNULL(getHeader(*resp.headers, "X-Method")) == "HEAD");
  KJ_EXPECT(KJ_ASSERT_NONNULL(resp.body->tryGetLength()) == 22);
  auto body = resp.body->readAllText().wait(f.io.waitScope);
  KJ_EXPECT(body == "", body);
}

KJ_TEST("hyper server: 204 has no body and no framing headers") {
  TestFixture f;

  auto req = f.client->request(kj::HttpMethod::GET, "/no-content", f.makeHeaders(), uint64_t(0));
  auto resp = req.response.wait(f.io.waitScope);

  KJ_EXPECT(resp.statusCode == 204);
  KJ_EXPECT(getHeader(*resp.headers, "Content-Length") == kj::none);
  KJ_EXPECT(getHeader(*resp.headers, "Transfer-Encoding") == kj::none);
  auto body = resp.body->readAllText().wait(f.io.waitScope);
  KJ_EXPECT(body == "", body);
}

KJ_TEST("hyper server: POST with streaming chunked request body, chunked response") {
  TestFixture f;

  // No expectedBodySize: the client sends the request body chunked; the service sees an
  // unknown length. The response also has no expected size, so it goes out chunked.
  auto req = f.client->request(kj::HttpMethod::POST, "/echo", f.makeHeaders());
  req.body->write("Hello, "_kjb).wait(f.io.waitScope);
  req.body->write("streaming "_kjb).wait(f.io.waitScope);
  req.body->write("world!"_kjb).wait(f.io.waitScope);
  req.body = nullptr;  // EOF

  auto resp = req.response.wait(f.io.waitScope);
  KJ_EXPECT(resp.statusCode == 200);
  auto reqLen = KJ_ASSERT_NONNULL(getHeader(*resp.headers, "X-Request-Length"));
  KJ_EXPECT(reqLen == "unknown", reqLen);
  // Chunked response: no Content-Length visible.
  KJ_EXPECT(resp.body->tryGetLength() == kj::none);
  auto body = resp.body->readAllText().wait(f.io.waitScope);
  KJ_EXPECT(body == "Hello, streaming world!", body);
}

KJ_TEST("hyper server: POST with Content-Length is visible to the service") {
  TestFixture f;

  auto content = "exact-length body"_kj;
  auto req =
      f.client->request(kj::HttpMethod::POST, "/echo", f.makeHeaders(), uint64_t(content.size()));
  req.body->write(content.asBytes()).wait(f.io.waitScope);
  req.body = nullptr;

  auto resp = req.response.wait(f.io.waitScope);
  KJ_EXPECT(resp.statusCode == 200);
  // The service observed the exact length through kj::AsyncInputStream::tryGetLength().
  auto reqLen = KJ_ASSERT_NONNULL(getHeader(*resp.headers, "X-Request-Length"));
  KJ_EXPECT(reqLen == kj::str(content.size()), reqLen);
  auto body = resp.body->readAllText().wait(f.io.waitScope);
  KJ_EXPECT(body == content, body);
}

KJ_TEST("hyper server: header fidelity (multi-value, non-UTF-8 bytes, both directions)") {
  TestFixture f;

  const kj::byte binary[] = {'a', 0x80, 0xC3, 0x28, 0xFF, 'z'};  // deliberately invalid UTF-8

  auto headers = f.makeHeaders();
  headers.set(f.ids.xEcho, kj::str("echo-me"));
  headers.set(f.ids.xBinary, kj::heapString(kj::arrayPtr(binary, sizeof(binary)).asChars()));
  headers.add("X-Multi", "one");
  headers.add("X-Multi", "two");

  auto req = f.client->request(kj::HttpMethod::GET, "/headers", kj::mv(headers), uint64_t(0));
  auto resp = req.response.wait(f.io.waitScope);
  KJ_EXPECT(resp.statusCode == 200);

  // Request headers arrived at the service intact (indexed via the shared header table)...
  KJ_EXPECT(KJ_ASSERT_NONNULL(resp.headers->get(f.ids.xEchoBack)) == "echo-me");
  KJ_EXPECT(KJ_ASSERT_NONNULL(getHeader(*resp.headers, "X-Host-Back")) == "test-host");
  // ... including multi-value order ...
  KJ_EXPECT(KJ_ASSERT_NONNULL(getHeader(*resp.headers, "X-Multi-Back")) == "one,two");
  // ... and non-UTF-8 bytes, echoed back through the response path.
  auto echoedBinary = KJ_ASSERT_NONNULL(resp.headers->get(f.ids.xBinaryResp));
  KJ_EXPECT(echoedBinary.asBytes() == kj::arrayPtr(binary, sizeof(binary)));

  // Response-originated multi-value header preserved in order.
  auto cookies = collectHeader(*resp.headers, "Set-Cookie");
  KJ_ASSERT(cookies.size() == 2);
  KJ_EXPECT(cookies[0] == "a=1", cookies[0]);
  KJ_EXPECT(cookies[1] == "b=2", cookies[1]);

  // Response-originated non-UTF-8 header value passes through byte-for-byte.
  const kj::byte expectedBinary[] = {'v', 0x80, 0xFF, 0xFE, 'z'};
  auto binaryValue = KJ_ASSERT_NONNULL(getHeader(*resp.headers, "X-Binary-Value"));
  KJ_EXPECT(binaryValue.asBytes() == kj::arrayPtr(expectedBinary, sizeof(expectedBinary)));

  auto body = resp.body->readAllText().wait(f.io.waitScope);
  KJ_EXPECT(body == "ok", body);
}

KJ_TEST("hyper server: large streamed response body with backpressure") {
  TestFixture f;

  auto req = f.client->request(kj::HttpMethod::GET, "/large", f.makeHeaders(), uint64_t(0));
  auto resp = req.response.wait(f.io.waitScope);
  KJ_EXPECT(resp.statusCode == 200);
  KJ_EXPECT(KJ_ASSERT_NONNULL(resp.body->tryGetLength()) == LARGE_BODY_SIZE);

  auto buf = kj::heapArray<kj::byte>(CHUNK_SIZE);
  uint64_t offset = 0;
  for (;;) {
    auto n = resp.body->tryRead(buf.begin(), 1, buf.size()).wait(f.io.waitScope);
    if (n == 0) break;
    for (size_t i = 0; i < n; i++) {
      KJ_ASSERT(buf[i] == patternByte(offset + i), "response body corruption", offset + i);
    }
    offset += n;
  }
  KJ_EXPECT(offset == LARGE_BODY_SIZE, offset);
}

KJ_TEST("hyper server: large streamed request body with backpressure") {
  TestFixture f;

  auto req = f.client->request(
      kj::HttpMethod::POST, "/drain-body", f.makeHeaders(), uint64_t(LARGE_UPLOAD_SIZE));
  auto buf = kj::heapArray<kj::byte>(CHUNK_SIZE);
  for (uint64_t offset = 0; offset < LARGE_UPLOAD_SIZE; offset += CHUNK_SIZE) {
    for (size_t i = 0; i < CHUNK_SIZE; i++) {
      buf[i] = patternByte(offset + i);
    }
    req.body->write(buf).wait(f.io.waitScope);
  }
  req.body = nullptr;

  auto resp = req.response.wait(f.io.waitScope);
  KJ_EXPECT(resp.statusCode == 200);
  auto body = resp.body->readAllText().wait(f.io.waitScope);
  KJ_EXPECT(body == kj::str(LARGE_UPLOAD_SIZE), body);
}

KJ_TEST("hyper server: keep-alive serves multiple requests on one connection") {
  TestFixture f;

  // A single raw TCP connection with a stream-bound kj HTTP client: all requests ride the same
  // connection, proving server-side keep-alive.
  auto stream = f.address->connect().wait(f.io.waitScope);
  auto streamClient = kj::newHttpClient(*f.ids.table, *stream);

  for (int i = 0; i < 3; i++) {
    auto req = streamClient->request(kj::HttpMethod::GET, "/hello", f.makeHeaders(), uint64_t(0));
    auto resp = req.response.wait(f.io.waitScope);
    KJ_EXPECT(resp.statusCode == 200);
    auto body = resp.body->readAllText().wait(f.io.waitScope);
    KJ_EXPECT(body == "Hello from KJ service!");
  }
  KJ_EXPECT(f.service->requestCount == 3);
}

KJ_TEST("hyper server: concurrent connections") {
  TestFixture f;

  auto makeRequest = [&]() -> kj::Promise<kj::String> {
    auto req = f.client->request(kj::HttpMethod::GET, "/hello", f.makeHeaders(), uint64_t(0));
    return req.response.then([](kj::HttpClient::Response&& resp) {
      KJ_EXPECT(resp.statusCode == 200);
      auto promise = resp.body->readAllText();
      return promise.attach(kj::mv(resp.body));
    });
  };
  kj::Vector<kj::Promise<kj::String>> promises;
  for (int i = 0; i < 4; i++) {
    promises.add(makeRequest());
  }
  auto results = kj::joinPromisesFailFast(promises.releaseAsArray()).wait(f.io.waitScope);
  for (auto& result: results) {
    KJ_EXPECT(result == "Hello from KJ service!");
  }
  KJ_EXPECT(f.service->requestCount == 4);
}

KJ_TEST("hyper server: many requests are simultaneously in-flight on the one shared service") {
  // Regression guard for the F1b aliasing-soundness fix. The inbound server dispatches every
  // request (across all connections) to a SINGLE kj::HttpService, and several service calls run
  // concurrently on it. The Rust side must borrow that service as a shared `&HttpService`
  // (ServicePtr::get), never an exclusive `Pin<&mut>` — otherwise N simultaneously-live calls
  // would alias N `&mut` to one object (UB). Here N requests park in the handler at once, so N
  // shared borrows of the one service coexist; then all clients disconnect and every parked call
  // is canceled.
  TestFixture f;
  constexpr uint N = 8;

  {
    // A separate raw connection per request, so the calls genuinely overlap rather than
    // serializing on one keep-alive connection.
    kj::Vector<kj::Own<kj::AsyncIoStream>> streams;
    kj::Vector<kj::Own<kj::HttpClient>> clients;
    kj::Vector<kj::Promise<kj::HttpClient::Response>> responses;
    for (uint i = 0; i < N; i++) {
      auto stream = f.address->connect().wait(f.io.waitScope);
      auto client = kj::newHttpClient(*f.ids.table, *stream);
      auto req =
          client->request(kj::HttpMethod::GET, "/hang-before-send", f.makeHeaders(), uint64_t(0));
      responses.add(kj::mv(req.response));
      clients.add(kj::mv(client));
      streams.add(kj::mv(stream));
    }
    // All N requests reached the handler and parked (NEVER_DONE) without any completing: N
    // service calls are live on the one shared service at the same time.
    f.spinUntil([&]() { return f.service->requestCount == N; });
    KJ_EXPECT(f.service->canceledCount == 0);
    // Leaving this scope destroys the clients/streams, disconnecting all N connections.
  }

  // Every parked call is canceled as its connection dies (the shared borrows drop together).
  f.spinUntil([&]() { return f.service->canceledCount == N; });
  KJ_EXPECT(!f.service->inHang);
}

KJ_TEST("hyper server: service exception maps to a kj-style 500") {
  TestFixture f;

  auto req = f.client->request(kj::HttpMethod::GET, "/throw", f.makeHeaders(), uint64_t(0));
  auto resp = req.response.wait(f.io.waitScope);
  KJ_EXPECT(resp.statusCode == 500);
  KJ_EXPECT(resp.statusText == "Internal Server Error", resp.statusText);
  KJ_EXPECT(KJ_ASSERT_NONNULL(resp.headers->get(kj::HttpHeaderId::CONTENT_TYPE)) == "text/plain");
  auto body = resp.body->readAllText().wait(f.io.waitScope);
  KJ_EXPECT(body.startsWith("ERROR: The server threw an exception. Details:\n\n"), body);
  KJ_EXPECT(strstr(body.cStr(), "the test service deliberately failed") != nullptr, body);

  // The connection/server remain usable afterwards (per-connection error containment).
  auto ok = f.get("/hello");
  KJ_EXPECT(ok == "Hello from KJ service!");
}

KJ_TEST("hyper server: UNIMPLEMENTED exception maps to 501") {
  TestFixture f;

  auto req =
      f.client->request(kj::HttpMethod::GET, "/throw-unimplemented", f.makeHeaders(), uint64_t(0));
  auto resp = req.response.wait(f.io.waitScope);
  KJ_EXPECT(resp.statusCode == 501);
  auto body = resp.body->readAllText().wait(f.io.waitScope);
  KJ_EXPECT(
      body.startsWith("ERROR: The server does not implement this operation. Details:\n\n"), body);
}

KJ_TEST("hyper server: DISCONNECTED exception closes the connection without a response") {
  TestFixture f;
  TolerateDisconnectErrorLogs tolerateLogs;

  // Raw stream-bound client: see the mid-body test for why the pooled client is avoided here.
  auto stream = f.address->connect().wait(f.io.waitScope);
  auto streamClient = kj::newHttpClient(*f.ids.table, *stream);

  auto req = streamClient->request(
      kj::HttpMethod::GET, "/throw-disconnected", f.makeHeaders(), uint64_t(0));
  auto maybeException = kj::runCatchingExceptions([&]() { req.response.wait(f.io.waitScope); });
  auto& e = KJ_ASSERT_NONNULL(maybeException, "expected the connection to be dropped");
  KJ_EXPECT(e.getType() == kj::Exception::Type::DISCONNECTED, e);
}

KJ_TEST("hyper server: exception mid-body aborts the response, not a clean end") {
  TestFixture f;

  // Speak raw HTTP so the wire framing is directly observable (and no kj-http client machinery
  // reacts to the aborted connection with spurious error logs).
  auto stream = f.address->connect().wait(f.io.waitScope);
  stream->write("GET /throw-mid-body HTTP/1.1\r\nHost: test-host\r\n\r\n"_kjb).wait(f.io.waitScope);

  // Read until EOF or connection reset, collecting the raw response bytes.
  kj::Vector<char> collected;
  char buf[4096];
  bool sawError = false;
  for (;;) {
    size_t n = 0;
    auto maybeException = kj::runCatchingExceptions(
        [&]() { n = stream->tryRead(buf, 1, sizeof(buf)).wait(f.io.waitScope); });
    if (maybeException != kj::none) {
      sawError = true;
      break;
    }
    if (n == 0) break;
    collected.addAll(kj::arrayPtr(buf, n));
  }
  auto text = kj::str(collected.releaseAsArray());

  // The connection must be aborted rather than the chunked body terminating cleanly. Depending
  // on how the abort races hyper's write buffer, the client may observe a connection reset (with
  // any unread data discarded, possibly leaving nothing at all) or a truncated body -- but never
  // a final 0-length chunk.
  KJ_EXPECT(!text.endsWith("0\r\n\r\n"), text);
  if (!sawError && text.size() > 0) {
    KJ_EXPECT(text.startsWith("HTTP/1.1 200 OK"), text);
  }
}

KJ_TEST("hyper server: service not sending a response yields kj's 500 text") {
  TestFixture f;

  auto req = f.client->request(kj::HttpMethod::GET, "/no-response", f.makeHeaders(), uint64_t(0));
  auto resp = req.response.wait(f.io.waitScope);
  KJ_EXPECT(resp.statusCode == 500);
  auto body = resp.body->readAllText().wait(f.io.waitScope);
  KJ_EXPECT(body == "ERROR: The HttpService did not generate a response.", body);
}

KJ_TEST("hyper server: acceptWebSocket on a non-upgrade request throws like kj") {
  TestFixture f;

  auto req =
      f.client->request(kj::HttpMethod::GET, "/accept-websocket", f.makeHeaders(), uint64_t(0));
  auto resp = req.response.wait(f.io.waitScope);
  // The service exception surfaces as a 500 application error, like kj::HttpServer.
  KJ_EXPECT(resp.statusCode == 500);
  auto body = resp.body->readAllText().wait(f.io.waitScope);
  KJ_EXPECT(strstr(body.cStr(), "can't call acceptWebSocket()") != nullptr, body);
}

KJ_TEST("hyper server: CONNECT gets a 501") {
  TestFixture f;

  auto connReq = f.client->connect("example.com:443", f.makeHeaders(), {});
  auto resp = connReq.status.wait(f.io.waitScope);
  KJ_EXPECT(resp.statusCode == 501, resp.statusCode);
}

KJ_TEST("hyper server: client disconnect mid-response cancels the service promise") {
  TestFixture f;

  {
    // Raw connection so the test controls the disconnect.
    auto stream = f.address->connect().wait(f.io.waitScope);
    auto streamClient = kj::newHttpClient(*f.ids.table, *stream);

    auto req = streamClient->request(kj::HttpMethod::GET, "/hang", f.makeHeaders(), uint64_t(0));
    auto resp = req.response.wait(f.io.waitScope);
    KJ_EXPECT(resp.statusCode == 200);

    // Read the first bytes to prove the response is live and the service is now hanging.
    kj::byte buf[7];
    auto n = resp.body->tryRead(buf, 1, sizeof(buf)).wait(f.io.waitScope);
    KJ_EXPECT(n > 0);
    KJ_EXPECT(f.service->inHang);

    // Drop the client/stream: the TCP connection closes mid-response.
  }

  // The hyper connection notices and the KJ-side dispatcher cancels the in-flight
  // kj::HttpService::request() promise (observed via KJ_DEFER in the service coroutine).
  f.spinUntil([&]() { return f.service->canceledCount == 1; });
  KJ_EXPECT(!f.service->inHang);

  // The server remains usable.
  auto ok = f.get("/hello");
  KJ_EXPECT(ok == "Hello from KJ service!");
}

KJ_TEST("hyper server: client disconnect before the response cancels the service promise") {
  TestFixture f;

  {
    auto stream = f.address->connect().wait(f.io.waitScope);
    auto streamClient = kj::newHttpClient(*f.ids.table, *stream);

    auto req = streamClient->request(
        kj::HttpMethod::GET, "/hang-before-send", f.makeHeaders(), uint64_t(0));
    // Wait until the service is inside the handler, then disconnect without ever seeing a
    // response.
    f.spinUntil([&]() { return f.service->inHang; });
  }

  f.spinUntil([&]() { return f.service->canceledCount == 1; });
  KJ_EXPECT(!f.service->inHang);
}

KJ_TEST("hyper server: drain finishes the in-flight request, then resolves") {
  TestFixture f;

  // Park a request behind a gate.
  auto gate = kj::newPromiseAndFulfiller<void>();
  f.service->gate = kj::mv(gate.promise);

  auto req = f.client->request(kj::HttpMethod::GET, "/gate", f.makeHeaders(), uint64_t(0));
  f.spinUntil([&]() { return f.service->requestCount == 1 && f.service->gate == kj::none; });

  // Start draining while the request is in flight.
  auto drainPromise = f.server->drain().eagerlyEvaluate(nullptr);
  KJ_EXPECT(!drainPromise.poll(f.io.waitScope), "drain resolved with a request in flight");
  KJ_EXPECT(!f.listenTask.poll(f.io.waitScope));

  // Release the request; it must complete successfully (with Connection: close).
  gate.fulfiller->fulfill();
  auto resp = req.response.wait(f.io.waitScope);
  KJ_EXPECT(resp.statusCode == 200);
  auto body = resp.body->readAllText().wait(f.io.waitScope);
  KJ_EXPECT(body == "gated response", body);

  // Both drain() and the listen promise now resolve.
  drainPromise.wait(f.io.waitScope);
  f.listenTask.wait(f.io.waitScope);

  // New connections are refused after drain.
  auto maybeException = kj::runCatchingExceptions([&]() {
    auto req2 = f.client->request(kj::HttpMethod::GET, "/hello", f.makeHeaders(), uint64_t(0));
    req2.response.wait(f.io.waitScope);
  });
  KJ_EXPECT(maybeException != kj::none, "expected connection to be refused after drain");
}

KJ_TEST("hyper server: drain with no activity resolves promptly") {
  TestFixture f;

  f.server->drain().wait(f.io.waitScope);
  f.listenTask.wait(f.io.waitScope);
}

// ---------------------------------------------------------------------------------------
// WebSocket tests: kj client -> hyper server.

KJ_TEST("hyper server: WebSocket echo (text, binary, close code) with kj client") {
  TestFixture f;

  auto resp = f.client->openWebSocket("/ws-echo", f.makeHeaders()).wait(f.io.waitScope);
  KJ_EXPECT(resp.statusCode == 101, resp.statusCode);
  KJ_EXPECT(resp.statusText == "Switching Protocols", resp.statusText);
  KJ_EXPECT(KJ_ASSERT_NONNULL(getHeader(*resp.headers, "X-Ws-Accepted")) == "yes");
  auto ws = kj::mv(KJ_ASSERT_NONNULL(resp.webSocketOrBody.tryGet<kj::Own<kj::WebSocket>>()));

  ws->send("hello hyper server"_kj.asArray()).wait(f.io.waitScope);
  auto textMessage = ws->receive().wait(f.io.waitScope);
  KJ_EXPECT(KJ_ASSERT_NONNULL(textMessage.tryGet<kj::String>()) == "hello hyper server");

  const kj::byte binary[] = {0x00, 0xFF, 0x10, 0x80, 0x42};
  ws->send(kj::arrayPtr(binary, sizeof(binary))).wait(f.io.waitScope);
  auto binaryMessage = ws->receive().wait(f.io.waitScope);
  KJ_EXPECT(KJ_ASSERT_NONNULL(binaryMessage.tryGet<kj::Array<kj::byte>>()) ==
      kj::arrayPtr(binary, sizeof(binary)));

  // Close-code echo: the application-chosen code and reason round-trip.
  ws->close(4567, "custom close").wait(f.io.waitScope);
  auto closeMessage = ws->receive().wait(f.io.waitScope);
  auto& close = KJ_ASSERT_NONNULL(closeMessage.tryGet<kj::WebSocket::Close>());
  KJ_EXPECT(close.code == 4567, close.code);
  KJ_EXPECT(close.reason == "custom close", close.reason);
}

KJ_TEST("hyper server: request Sec-WebSocket-Extensions reaches the service verbatim") {
  TestFixture f;

  // kj's MANUAL_COMPRESSION client re-serializes valid offers; use its canonical form so this
  // asserts byte-for-byte passthrough on the hyper-server side.
  auto offer = "permessage-deflate; client_max_window_bits=10"_kj;
  auto headers = f.makeHeaders();
  headers.set(kj::HttpHeaderId::SEC_WEBSOCKET_EXTENSIONS, kj::str(offer));

  auto resp = f.client->openWebSocket("/ws-echo", kj::mv(headers)).wait(f.io.waitScope);
  KJ_EXPECT(resp.statusCode == 101);
  auto ws = kj::mv(KJ_ASSERT_NONNULL(resp.webSocketOrBody.tryGet<kj::Own<kj::WebSocket>>()));

  KJ_EXPECT(KJ_ASSERT_NONNULL(f.service->lastWsExtensions) == offer);
  // /ws-echo passes no extension config to acceptWebSocket, so no agreement was sent.
  KJ_EXPECT(resp.headers->get(kj::HttpHeaderId::SEC_WEBSOCKET_EXTENSIONS) == kj::none);

  ws->close(1000, "").wait(f.io.waitScope);
  ws->receive().wait(f.io.waitScope);
}

KJ_TEST("hyper server: manual permessage-deflate round trip with kj client") {
  TestFixture f;

  auto headers = f.makeHeaders();
  headers.set(kj::HttpHeaderId::SEC_WEBSOCKET_EXTENSIONS,
      kj::str("permessage-deflate; client_no_context_takeover; server_max_window_bits=10"));

  auto resp = f.client->openWebSocket("/ws-echo-compress", kj::mv(headers)).wait(f.io.waitScope);
  KJ_EXPECT(resp.statusCode == 101);
  // The hyper server computed a permessage-deflate agreement (like kj's MANUAL mode, the
  // response carries the generated agreement, not the application's raw header).
  auto agreed = KJ_ASSERT_NONNULL(resp.headers->get(kj::HttpHeaderId::SEC_WEBSOCKET_EXTENSIONS));
  KJ_EXPECT(agreed.startsWith("permessage-deflate"), agreed);
  KJ_EXPECT(strstr(agreed.cStr(), "server_max_window_bits=10") != nullptr, agreed);
  auto ws = kj::mv(KJ_ASSERT_NONNULL(resp.webSocketOrBody.tryGet<kj::Own<kj::WebSocket>>()));

  for (int i = 0; i < 3; i++) {
    auto big = kj::strArray(kj::repeat(kj::StringPtr("deflate this! "), 4096), "");
    ws->send(big.asArray()).wait(f.io.waitScope);
    auto message = ws->receive().wait(f.io.waitScope);
    KJ_EXPECT(KJ_ASSERT_NONNULL(message.tryGet<kj::String>()) == big);
  }

  ws->close(1000, "done").wait(f.io.waitScope);
  auto closeMessage = ws->receive().wait(f.io.waitScope);
  KJ_EXPECT(KJ_ASSERT_NONNULL(closeMessage.tryGet<kj::WebSocket::Close>()).code == 1000);
}

KJ_TEST("hyper server: oversized message closes with 1009 and fails the service receive") {
  TestFixture f;

  auto resp = f.client->openWebSocket("/ws-limit", f.makeHeaders()).wait(f.io.waitScope);
  KJ_EXPECT(resp.statusCode == 101);
  auto ws = kj::mv(KJ_ASSERT_NONNULL(resp.webSocketOrBody.tryGet<kj::Own<kj::WebSocket>>()));

  // The service receives with maxSize=1024; send 2048 bytes.
  auto big = kj::heapArray<kj::byte>(2048);
  for (size_t i = 0; i < big.size(); i++) big[i] = kj::byte(i);
  ws->send(big.asPtr()).wait(f.io.waitScope);

  // kj semantics: the server sends Close(1009, "Message is too large: N > M") and the service's
  // receive() throws.
  auto closeMessage = ws->receive().wait(f.io.waitScope);
  auto& close = KJ_ASSERT_NONNULL(closeMessage.tryGet<kj::WebSocket::Close>());
  KJ_EXPECT(close.code == 1009, close.code);
  KJ_EXPECT(close.reason == "Message is too large: 2048 > 1024", close.reason);

  f.spinUntil([&]() { return f.service->lastWsError != kj::none; });
  auto& error = KJ_ASSERT_NONNULL(f.service->lastWsError);
  KJ_EXPECT(
      strstr(error.getDescription().cStr(), "Message is too large: 2048 > 1024") != nullptr, error);
}

KJ_TEST("hyper server: client abort resolves whenAborted and fails the pending receive") {
  TestFixture f;

  auto resp = f.client->openWebSocket("/ws-abort-watch", f.makeHeaders()).wait(f.io.waitScope);
  KJ_EXPECT(resp.statusCode == 101);
  auto ws = kj::mv(KJ_ASSERT_NONNULL(resp.webSocketOrBody.tryGet<kj::Own<kj::WebSocket>>()));

  // Give the service a beat to get into its receive, then abort.
  f.spinUntil([&]() { return f.service->requestCount == 1; });
  ws->abort();

  f.spinUntil([&]() { return f.service->wsAborted; });
  auto& error = KJ_ASSERT_NONNULL(f.service->lastWsError);
  KJ_EXPECT(error.getType() == kj::Exception::Type::DISCONNECTED, error);
}

// ---------------------------------------------------------------------------------------
// Raw-socket clients: wire-level fidelity for the hyper server's handshake and framing.

kj::FixedArray<kj::byte, 4> testMask() {
  kj::FixedArray<kj::byte, 4> key;
  key[0] = 0x12;
  key[1] = 0x34;
  key[2] = 0x56;
  key[3] = 0x78;
  return key;
}

// Performs the client half of a WebSocket handshake on a raw stream.
kj::String rawHandshake(kj::AsyncIoStream& stream,
    kj::WaitScope& waitScope,
    kj::StringPtr path,
    kj::StringPtr extraHeaders = "") {
  auto request = kj::str("GET ", path,
      " HTTP/1.1\r\n"
      "Host: test-host\r\n"
      "Connection: Upgrade\r\n"
      "Upgrade: websocket\r\n"
      "Sec-WebSocket-Version: 13\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n",
      extraHeaders, "\r\n");
  stream.write(request.asBytes()).wait(waitScope);
  return readHttpHead(stream, waitScope);
}

KJ_TEST("hyper server (raw client): fragmented message reassembly and ping/pong") {
  TestFixture f;

  auto stream = f.address->connect().wait(f.io.waitScope);
  auto head = rawHandshake(*stream, f.io.waitScope, "/ws-echo");
  KJ_EXPECT(head.startsWith("HTTP/1.1 101"), head);
  // RFC 6455's worked example: the accept for "dGhlIHNhbXBsZSBub25jZQ==".
  KJ_EXPECT(strstr(head.cStr(), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != nullptr, head);

  // Send a text message in three masked fragments with a ping in the middle; the service must
  // receive one reassembled message and echo it, and the server must auto-pong.
  stream->write(makeFrame(false, 1, "Hello, "_kj.asBytes(), testMask())).wait(f.io.waitScope);
  stream->write(makeFrame(true, 9, "marco"_kj.asBytes(), testMask())).wait(f.io.waitScope);
  stream->write(makeFrame(false, 0, "fragmented "_kj.asBytes(), testMask())).wait(f.io.waitScope);
  stream->write(makeFrame(true, 0, "world!"_kj.asBytes(), testMask())).wait(f.io.waitScope);

  // The pong comes first (it is queued as soon as the ping frame is parsed).
  auto pong = readFrame(*stream, f.io.waitScope);
  KJ_EXPECT(pong.opcode == 10, pong.opcode);
  KJ_EXPECT(kj::str(pong.payload.asChars()) == "marco");

  auto echo = readFrame(*stream, f.io.waitScope);
  KJ_EXPECT(echo.fin);
  KJ_EXPECT(echo.opcode == 1, echo.opcode);
  KJ_EXPECT(kj::str(echo.payload.asChars()) == "Hello, fragmented world!");

  // Close handshake: the service echoes our close code.
  kj::byte closePayload[] = {0x0F, 0xA1, 'b', 'y', 'e'};  // 4001 + "bye"
  stream->write(makeFrame(true, 8, kj::arrayPtr(closePayload, sizeof(closePayload)), testMask()))
      .wait(f.io.waitScope);
  auto closeEcho = readFrame(*stream, f.io.waitScope);
  KJ_EXPECT(closeEcho.opcode == 8);
  KJ_ASSERT(closeEcho.payload.size() >= 2);
  KJ_EXPECT((uint(closeEcho.payload[0]) << 8 | closeEcho.payload[1]) == 4001);
  KJ_EXPECT(kj::str(closeEcho.payload.slice(2, closeEcho.payload.size()).asChars()) == "bye");
}

KJ_TEST("hyper server (raw client): compressed echo sets RSV1 on the wire") {
  TestFixture f;

  auto stream = f.address->connect().wait(f.io.waitScope);
  auto head = rawHandshake(*stream, f.io.waitScope, "/ws-echo-compress",
      "Sec-WebSocket-Extensions: permessage-deflate\r\n");
  KJ_EXPECT(head.startsWith("HTTP/1.1 101"), head);
  KJ_EXPECT(strstr(head.cStr(), "sec-websocket-extensions: permessage-deflate") != nullptr ||
          strstr(head.cStr(), "Sec-WebSocket-Extensions: permessage-deflate") != nullptr,
      head);

  // Send an uncompressed (RSV1=0) text frame — always legal even when compression is
  // negotiated. The echo must come back as a compressed frame (RSV1 set) whose payload is
  // deflated, not the plain text.
  auto text = "compress me, hyper server! compress me, hyper server!"_kj;
  stream->write(makeFrame(true, 1, text.asBytes(), testMask())).wait(f.io.waitScope);
  auto echo = readFrame(*stream, f.io.waitScope);
  KJ_EXPECT(echo.fin);
  KJ_EXPECT(echo.rsv1, "expected the echoed frame to be compressed");
  KJ_EXPECT(echo.opcode == 1, echo.opcode);
  KJ_EXPECT(echo.payload.size() < text.size(), echo.payload.size(), text.size());

  kj::byte closePayload[] = {0x03, 0xE8};  // 1000
  stream->write(makeFrame(true, 8, kj::arrayPtr(closePayload, sizeof(closePayload)), testMask()))
      .wait(f.io.waitScope);
  auto closeEcho = readFrame(*stream, f.io.waitScope);
  KJ_EXPECT(closeEcho.opcode == 8);
}

KJ_TEST("hyper server (raw client): bad WebSocket handshake gets kj's 400") {
  TestFixture f;

  auto stream = f.address->connect().wait(f.io.waitScope);
  // Missing Sec-WebSocket-Version.
  auto request = kj::str("GET /ws-echo HTTP/1.1\r\n"
                         "Host: test-host\r\n"
                         "Connection: Upgrade\r\n"
                         "Upgrade: websocket\r\n"
                         "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
  stream->write(request.asBytes()).wait(f.io.waitScope);
  auto response = stream->readAllText().wait(f.io.waitScope);
  KJ_EXPECT(response.startsWith("HTTP/1.1 400"), response);
  KJ_EXPECT(strstr(response.cStr(), "ERROR: The requested WebSocket version is not supported.") !=
          nullptr,
      response);
}

// ---------------------------------------------------------------------------------------
// CONNECT tests: kj client -> hyper server.

KJ_TEST("hyper server: CONNECT tunnel carries bidirectional bytes (kj client)") {
  TestFixture f;

  auto req = f.client->connect("echo-host:1234", f.makeHeaders(), {});
  auto status = req.status.wait(f.io.waitScope);
  KJ_EXPECT(status.statusCode == 200, status.statusCode);
  KJ_EXPECT(status.statusText == "OK", status.statusText);
  KJ_EXPECT(KJ_ASSERT_NONNULL(getHeader(*status.headers, "X-Tunnel")) == "ok");

  auto roundTrip = [&](kj::StringPtr data) {
    req.connection->write(data.asBytes()).wait(f.io.waitScope);
    auto buf = kj::heapArray<kj::byte>(data.size());
    auto n = req.connection->tryRead(buf.begin(), buf.size(), buf.size()).wait(f.io.waitScope);
    KJ_ASSERT(n == buf.size());
    KJ_EXPECT(kj::str(buf.asChars()) == data);
  };
  roundTrip("tunnel me");
  roundTrip("tunnel me harder");

  req.connection->shutdownWrite();
  kj::byte buf[16];
  auto n = req.connection->tryRead(buf, 1, sizeof(buf)).wait(f.io.waitScope);
  KJ_EXPECT(n == 0, n);
}

KJ_TEST("hyper server: CONNECT early client close (kj client)") {
  TestFixture f;

  auto req = f.client->connect("echo-host:1234", f.makeHeaders(), {});
  // Write + half-close before consuming the 200; hyper buffers pre-accept tunnel bytes and the
  // echo must still complete.
  req.connection->write("goodbye"_kjb).wait(f.io.waitScope);
  req.connection->shutdownWrite();

  auto status = req.status.wait(f.io.waitScope);
  KJ_EXPECT(status.statusCode == 200, status.statusCode);
  auto echoed = req.connection->readAllText().wait(f.io.waitScope);
  KJ_EXPECT(echoed == "goodbye", echoed);
}

KJ_TEST("hyper server: CONNECT rejection carries status and body (kj client)") {
  TestFixture f;

  auto req = f.client->connect("reject-host:99", f.makeHeaders(), {});
  auto status = req.status.wait(f.io.waitScope);
  KJ_EXPECT(status.statusCode == 403, status.statusCode);
  KJ_EXPECT(status.statusText == "Forbidden", status.statusText);
  auto errorBody = KJ_ASSERT_NONNULL(kj::mv(status.errorBody));
  auto body = errorBody->readAllText().wait(f.io.waitScope);
  KJ_EXPECT(body == "tunnel rejected", body);
}

KJ_TEST("hyper server: CONNECT with a body is rejected with 400 like kj") {
  TestFixture f;

  // kj::HttpServer rejects CONNECT requests carrying Content-Length/Transfer-Encoding up front.
  auto stream = f.address->connect().wait(f.io.waitScope);
  stream
      ->write("CONNECT echo-host:1234 HTTP/1.1\r\nHost: echo-host:1234\r\n"
              "Content-Length: 5\r\n\r\nhello"_kjb)
      .wait(f.io.waitScope);
  auto response = stream->readAllText().wait(f.io.waitScope);
  KJ_EXPECT(response.startsWith("HTTP/1.1 400"), response);
  KJ_EXPECT(f.service->requestCount == 0);
}

// ---------------------------------------------------------------------------------------
// Cross-tests: the same assertions driven through the hyper *client* (C1), so requests flow
// hyper client -> TCP -> hyper server -> C++ service through both bridges.

struct HyperClientFixture: public TestFixture {
  HyperClientFixture() {
    hyperClientService = workerd::rust::kj_hyper::newHyperHttpService(*ids.table, "127.0.0.1",
        uint16_t(server->getPort()), workerd::rust::kj_hyper::newAllowAllHyperPeerFilter());
    hyperClient = kj::newHttpClient(*hyperClientService);
  }

  kj::Own<kj::HttpService> hyperClientService;
  kj::Own<kj::HttpClient> hyperClient;
};

KJ_TEST("hyper client -> hyper server: GET round trip") {
  HyperClientFixture f;

  auto req =
      f.hyperClient->request(kj::HttpMethod::GET, "/hello?x=1", f.makeHeaders(), uint64_t(0));
  auto resp = req.response.wait(f.io.waitScope);
  KJ_EXPECT(resp.statusCode == 200);
  KJ_EXPECT(resp.statusText == "Everything Is Fine", resp.statusText);
  KJ_EXPECT(KJ_ASSERT_NONNULL(resp.headers->get(f.ids.xUrl)) == "/hello?x=1");
  KJ_EXPECT(KJ_ASSERT_NONNULL(resp.body->tryGetLength()) == 22);
  auto body = resp.body->readAllText().wait(f.io.waitScope);
  KJ_EXPECT(body == "Hello from KJ service!", body);
}

KJ_TEST("hyper client -> hyper server: streaming POST round trip") {
  HyperClientFixture f;

  auto content = "exact-length body"_kj;
  auto req = f.hyperClient->request(
      kj::HttpMethod::POST, "/echo", f.makeHeaders(), uint64_t(content.size()));
  req.body->write(content.asBytes()).wait(f.io.waitScope);
  req.body = nullptr;

  auto resp = req.response.wait(f.io.waitScope);
  KJ_EXPECT(resp.statusCode == 200);
  auto reqLen = KJ_ASSERT_NONNULL(getHeader(*resp.headers, "X-Request-Length"));
  KJ_EXPECT(reqLen == kj::str(content.size()), reqLen);
  auto body = resp.body->readAllText().wait(f.io.waitScope);
  KJ_EXPECT(body == content, body);
}

KJ_TEST("hyper client -> hyper server: large bodies both directions") {
  HyperClientFixture f;

  {
    auto req = f.hyperClient->request(kj::HttpMethod::GET, "/large", f.makeHeaders(), uint64_t(0));
    auto resp = req.response.wait(f.io.waitScope);
    KJ_EXPECT(resp.statusCode == 200);
    KJ_EXPECT(KJ_ASSERT_NONNULL(resp.body->tryGetLength()) == LARGE_BODY_SIZE);
    auto buf = kj::heapArray<kj::byte>(CHUNK_SIZE);
    uint64_t offset = 0;
    for (;;) {
      auto n = resp.body->tryRead(buf.begin(), 1, buf.size()).wait(f.io.waitScope);
      if (n == 0) break;
      for (size_t i = 0; i < n; i++) {
        KJ_ASSERT(buf[i] == patternByte(offset + i), "response body corruption", offset + i);
      }
      offset += n;
    }
    KJ_EXPECT(offset == LARGE_BODY_SIZE, offset);
  }

  {
    auto req = f.hyperClient->request(
        kj::HttpMethod::POST, "/drain-body", f.makeHeaders(), uint64_t(LARGE_UPLOAD_SIZE));
    auto buf = kj::heapArray<kj::byte>(CHUNK_SIZE);
    for (uint64_t offset = 0; offset < LARGE_UPLOAD_SIZE; offset += CHUNK_SIZE) {
      for (size_t i = 0; i < CHUNK_SIZE; i++) {
        buf[i] = patternByte(offset + i);
      }
      req.body->write(buf).wait(f.io.waitScope);
    }
    req.body = nullptr;
    auto resp = req.response.wait(f.io.waitScope);
    KJ_EXPECT(resp.statusCode == 200);
    auto body = resp.body->readAllText().wait(f.io.waitScope);
    KJ_EXPECT(body == kj::str(LARGE_UPLOAD_SIZE), body);
  }
}

KJ_TEST("hyper client -> hyper server: header fidelity") {
  HyperClientFixture f;

  const kj::byte binary[] = {'a', 0x80, 0xC3, 0x28, 0xFF, 'z'};

  auto headers = f.makeHeaders();
  headers.set(f.ids.xEcho, kj::str("echo-me"));
  headers.set(f.ids.xBinary, kj::heapString(kj::arrayPtr(binary, sizeof(binary)).asChars()));
  headers.add("X-Multi", "one");
  headers.add("X-Multi", "two");

  auto req = f.hyperClient->request(kj::HttpMethod::GET, "/headers", kj::mv(headers), uint64_t(0));
  auto resp = req.response.wait(f.io.waitScope);
  KJ_EXPECT(resp.statusCode == 200);

  KJ_EXPECT(KJ_ASSERT_NONNULL(resp.headers->get(f.ids.xEchoBack)) == "echo-me");
  KJ_EXPECT(KJ_ASSERT_NONNULL(getHeader(*resp.headers, "X-Multi-Back")) == "one,two");
  auto echoedBinary = KJ_ASSERT_NONNULL(resp.headers->get(f.ids.xBinaryResp));
  KJ_EXPECT(echoedBinary.asBytes() == kj::arrayPtr(binary, sizeof(binary)));
  auto cookies = collectHeader(*resp.headers, "Set-Cookie");
  KJ_ASSERT(cookies.size() == 2);
  KJ_EXPECT(cookies[0] == "a=1", cookies[0]);
  KJ_EXPECT(cookies[1] == "b=2", cookies[1]);
  const kj::byte expectedBinary[] = {'v', 0x80, 0xFF, 0xFE, 'z'};
  auto binaryValue = KJ_ASSERT_NONNULL(getHeader(*resp.headers, "X-Binary-Value"));
  KJ_EXPECT(binaryValue.asBytes() == kj::arrayPtr(expectedBinary, sizeof(expectedBinary)));

  auto body = resp.body->readAllText().wait(f.io.waitScope);
  KJ_EXPECT(body == "ok", body);
}

KJ_TEST("hyper client -> hyper server: HEAD and error responses") {
  HyperClientFixture f;

  {
    auto req = f.hyperClient->request(kj::HttpMethod::HEAD, "/hello", f.makeHeaders(), uint64_t(0));
    auto resp = req.response.wait(f.io.waitScope);
    KJ_EXPECT(resp.statusCode == 200);
    KJ_EXPECT(KJ_ASSERT_NONNULL(resp.body->tryGetLength()) == 22);
    auto body = resp.body->readAllText().wait(f.io.waitScope);
    KJ_EXPECT(body == "", body);
  }

  {
    auto req = f.hyperClient->request(kj::HttpMethod::GET, "/throw", f.makeHeaders(), uint64_t(0));
    auto resp = req.response.wait(f.io.waitScope);
    KJ_EXPECT(resp.statusCode == 500);
    auto body = resp.body->readAllText().wait(f.io.waitScope);
    KJ_EXPECT(body.startsWith("ERROR: The server threw an exception. Details:\n\n"), body);
  }
}

KJ_TEST("hyper client -> hyper server: WebSocket echo (text, binary, close code)") {
  HyperClientFixture f;

  auto resp = f.hyperClient->openWebSocket("/ws-echo", f.makeHeaders()).wait(f.io.waitScope);
  KJ_EXPECT(resp.statusCode == 101, resp.statusCode);
  KJ_EXPECT(KJ_ASSERT_NONNULL(getHeader(*resp.headers, "X-Ws-Accepted")) == "yes");
  auto ws = kj::mv(KJ_ASSERT_NONNULL(resp.webSocketOrBody.tryGet<kj::Own<kj::WebSocket>>()));

  ws->send("full hyper round trip"_kj.asArray()).wait(f.io.waitScope);
  auto textMessage = ws->receive().wait(f.io.waitScope);
  KJ_EXPECT(KJ_ASSERT_NONNULL(textMessage.tryGet<kj::String>()) == "full hyper round trip");

  const kj::byte binary[] = {0xCA, 0xFE, 0x00, 0xBA, 0xBE};
  ws->send(kj::arrayPtr(binary, sizeof(binary))).wait(f.io.waitScope);
  auto binaryMessage = ws->receive().wait(f.io.waitScope);
  KJ_EXPECT(KJ_ASSERT_NONNULL(binaryMessage.tryGet<kj::Array<kj::byte>>()) ==
      kj::arrayPtr(binary, sizeof(binary)));

  ws->close(4999, "all done").wait(f.io.waitScope);
  auto closeMessage = ws->receive().wait(f.io.waitScope);
  auto& close = KJ_ASSERT_NONNULL(closeMessage.tryGet<kj::WebSocket::Close>());
  KJ_EXPECT(close.code == 4999, close.code);
  KJ_EXPECT(close.reason == "all done", close.reason);
}

KJ_TEST("hyper client -> hyper server: fragmented large message via pumps") {
  HyperClientFixture f;

  // Large messages exercise the fragment-capable receive path end to end (both sides send
  // unfragmented like kj, but the 1MB payload crosses every buffer boundary in the stack).
  auto resp = f.hyperClient->openWebSocket("/ws-echo", f.makeHeaders()).wait(f.io.waitScope);
  KJ_EXPECT(resp.statusCode == 101);
  auto ws = kj::mv(KJ_ASSERT_NONNULL(resp.webSocketOrBody.tryGet<kj::Own<kj::WebSocket>>()));

  auto big = kj::heapArray<kj::byte>(1024 * 1024);
  for (size_t i = 0; i < big.size(); i++) big[i] = patternByte(i);
  ws->send(big.asPtr()).wait(f.io.waitScope);
  auto message = ws->receive(big.size() + 1024).wait(f.io.waitScope);
  KJ_EXPECT(KJ_ASSERT_NONNULL(message.tryGet<kj::Array<kj::byte>>()) == big.asPtr());

  ws->close(1000, "").wait(f.io.waitScope);
  ws->receive().wait(f.io.waitScope);
}

KJ_TEST("hyper client -> hyper server: extension offer passes through verbatim") {
  HyperClientFixture f;

  auto offer = "permessage-deflate; client_max_window_bits=11; server_no_context_takeover"_kj;
  auto headers = f.makeHeaders();
  headers.set(kj::HttpHeaderId::SEC_WEBSOCKET_EXTENSIONS, kj::str(offer));

  auto resp = f.hyperClient->openWebSocket("/ws-echo", kj::mv(headers)).wait(f.io.waitScope);
  KJ_EXPECT(resp.statusCode == 101);
  auto ws = kj::mv(KJ_ASSERT_NONNULL(resp.webSocketOrBody.tryGet<kj::Own<kj::WebSocket>>()));

  // Byte-for-byte: hyper client sent it verbatim, hyper server delivered it verbatim.
  KJ_EXPECT(KJ_ASSERT_NONNULL(f.service->lastWsExtensions) == offer);

  ws->close(1000, "").wait(f.io.waitScope);
  ws->receive().wait(f.io.waitScope);
}

KJ_TEST("hyper client -> hyper server: manual permessage-deflate round trip") {
  HyperClientFixture f;

  auto headers = f.makeHeaders();
  headers.set(kj::HttpHeaderId::SEC_WEBSOCKET_EXTENSIONS, kj::str("permessage-deflate"));

  auto resp =
      f.hyperClient->openWebSocket("/ws-echo-compress", kj::mv(headers)).wait(f.io.waitScope);
  KJ_EXPECT(resp.statusCode == 101);
  auto agreed = KJ_ASSERT_NONNULL(resp.headers->get(kj::HttpHeaderId::SEC_WEBSOCKET_EXTENSIONS));
  KJ_EXPECT(agreed.startsWith("permessage-deflate"), agreed);
  auto ws = kj::mv(KJ_ASSERT_NONNULL(resp.webSocketOrBody.tryGet<kj::Own<kj::WebSocket>>()));

  for (int i = 0; i < 3; i++) {
    auto big = kj::strArray(kj::repeat(kj::StringPtr("hyper to hyper deflate. "), 2048), "");
    ws->send(big.asArray()).wait(f.io.waitScope);
    auto message = ws->receive().wait(f.io.waitScope);
    KJ_EXPECT(KJ_ASSERT_NONNULL(message.tryGet<kj::String>()) == big);
  }

  ws->close(1000, "bye").wait(f.io.waitScope);
  auto closeMessage = ws->receive().wait(f.io.waitScope);
  KJ_EXPECT(KJ_ASSERT_NONNULL(closeMessage.tryGet<kj::WebSocket::Close>()).code == 1000);
}

KJ_TEST("hyper client -> hyper server: client abort resolves the server's whenAborted") {
  HyperClientFixture f;

  auto resp = f.hyperClient->openWebSocket("/ws-abort-watch", f.makeHeaders()).wait(f.io.waitScope);
  KJ_EXPECT(resp.statusCode == 101);
  auto ws = kj::mv(KJ_ASSERT_NONNULL(resp.webSocketOrBody.tryGet<kj::Own<kj::WebSocket>>()));

  f.spinUntil([&]() { return f.service->requestCount == 1; });
  ws->abort();

  f.spinUntil([&]() { return f.service->wsAborted; });
  auto& error = KJ_ASSERT_NONNULL(f.service->lastWsError);
  KJ_EXPECT(error.getType() == kj::Exception::Type::DISCONNECTED, error);
}

KJ_TEST("hyper client -> hyper server: CONNECT tunnel, rejection, and early close") {
  HyperClientFixture f;

  {
    // Bidirectional bytes.
    auto req = f.hyperClient->connect("echo-host:1234", f.makeHeaders(), {});
    auto status = req.status.wait(f.io.waitScope);
    KJ_EXPECT(status.statusCode == 200, status.statusCode);
    KJ_EXPECT(KJ_ASSERT_NONNULL(getHeader(*status.headers, "X-Tunnel")) == "ok");
    req.connection->write("hyper tunnel"_kjb).wait(f.io.waitScope);
    kj::byte buf[12];
    auto n = req.connection->tryRead(buf, sizeof(buf), sizeof(buf)).wait(f.io.waitScope);
    KJ_ASSERT(n == sizeof(buf));
    KJ_EXPECT(kj::str(kj::arrayPtr(buf, n).asChars()) == "hyper tunnel");

    // Early client close: half-close propagates and the tunnel drains to EOF.
    req.connection->shutdownWrite();
    auto rest = req.connection->readAllText().wait(f.io.waitScope);
    KJ_EXPECT(rest == "", rest);
  }

  {
    // Rejection with status + body.
    auto req = f.hyperClient->connect("reject-host:99", f.makeHeaders(), {});
    auto status = req.status.wait(f.io.waitScope);
    KJ_EXPECT(status.statusCode == 403, status.statusCode);
    auto errorBody = KJ_ASSERT_NONNULL(kj::mv(status.errorBody));
    auto body = errorBody->readAllText().wait(f.io.waitScope);
    KJ_EXPECT(body == "tunnel rejected", body);
  }
}

// ---------------------------------------------------------------------------------------
// Single-connection serving without the TestServer accept loop: the fixture drives one
// HyperHttpConnection directly over a loopback socket pair.

// Accepts one connection from the client side of a loopback pair and serves it with a
// HyperHttpConnection; the client end is wrapped in a stream-based kj::HttpClient.
struct ConnectionFixture {
  explicit ConnectionFixture(TestFixture& f): f(f) {
    auto listener =
        f.io.provider->getNetwork().parseAddress("127.0.0.1", 0).wait(f.io.waitScope)->listen();
    auto connectPromise = f.io.provider->getNetwork()
                              .parseAddress("127.0.0.1", listener->getPort())
                              .wait(f.io.waitScope)
                              ->connect();
    auto serverStream = listener->accept().wait(f.io.waitScope);
    clientStream = connectPromise.wait(f.io.waitScope);

    // Hand hyper the accepted stream, mirroring HttpListener: its socket is taken natively
    // (these are kj-rs-io streams, so the unwrap tier applies and the wrapper is left hollow)
    // and the kj stream object is dropped right after.
    connection = workerd::rust::kj_hyper::newHyperHttpConnection(
        *f.ids.table, *f.service, kj::mv(serverStream));

    serveTask = connection->serve().eagerlyEvaluate([](kj::Exception&& e) { KJ_LOG(ERROR, e); });
    client = kj::newHttpClient(*f.ids.table, *clientStream,
        {.entropySource = f.entropy,
          .webSocketCompressionMode = kj::HttpClientSettings::MANUAL_COMPRESSION});
  }

  ~ConnectionFixture() noexcept(false) {
    // Quiesce before the TestFixture tears down the KJ event loop (same reason as ~TestFixture):
    // closing the client end makes hyper's connection task exit, resolving serve().
    client = nullptr;
    clientStream = nullptr;
    if (!serveTask.poll(f.io.waitScope)) {
      connection->shutdown();
      serveTask.wait(f.io.waitScope);
    }
  }

  TestFixture& f;
  kj::Own<kj::AsyncIoStream> clientStream;
  kj::Own<workerd::rust::kj_hyper::HyperHttpConnection> connection;
  kj::Promise<void> serveTask = nullptr;
  kj::Own<kj::HttpClient> client;
};

KJ_TEST("hyper connection: serves an externally-accepted connection with keep-alive") {
  TestFixture f;
  ConnectionFixture c(f);

  {
    auto req = c.client->request(kj::HttpMethod::GET, "/hello?one", f.makeHeaders(), uint64_t(0));
    auto resp = req.response.wait(f.io.waitScope);
    KJ_EXPECT(resp.statusCode == 200);
    KJ_EXPECT(KJ_ASSERT_NONNULL(resp.headers->get(f.ids.xUrl)) == "/hello?one");
    auto body = resp.body->readAllText().wait(f.io.waitScope);
    KJ_EXPECT(body == "Hello from KJ service!", body);
  }

  {
    // Keep-alive: a second request on the same connection.
    auto req = c.client->request(kj::HttpMethod::GET, "/hello?two", f.makeHeaders(), uint64_t(0));
    auto resp = req.response.wait(f.io.waitScope);
    KJ_EXPECT(resp.statusCode == 200);
    KJ_EXPECT(KJ_ASSERT_NONNULL(resp.headers->get(f.ids.xUrl)) == "/hello?two");
    resp.body->readAllText().wait(f.io.waitScope);
  }
}

KJ_TEST("hyper connection: WebSocket echo") {
  TestFixture f;
  ConnectionFixture c(f);

  auto resp = c.client->openWebSocket("/ws-echo", f.makeHeaders()).wait(f.io.waitScope);
  KJ_EXPECT(resp.statusCode == 101, resp.statusCode);
  auto ws = kj::mv(KJ_ASSERT_NONNULL(resp.webSocketOrBody.tryGet<kj::Own<kj::WebSocket>>()));

  ws->send("hello hyper connection"_kj.asArray()).wait(f.io.waitScope);
  auto message = ws->receive().wait(f.io.waitScope);
  KJ_EXPECT(KJ_ASSERT_NONNULL(message.tryGet<kj::String>()) == "hello hyper connection");

  ws->close(4321, "bye").wait(f.io.waitScope);
  auto closeMessage = ws->receive().wait(f.io.waitScope);
  auto& close = KJ_ASSERT_NONNULL(closeMessage.tryGet<kj::WebSocket::Close>());
  KJ_EXPECT(close.code == 4321);
}

KJ_TEST("hyper connection: shutdown closes an idle connection and resolves serve()") {
  TestFixture f;
  ConnectionFixture c(f);

  // One request completes normally...
  auto req = c.client->request(kj::HttpMethod::GET, "/hello", f.makeHeaders(), uint64_t(0));
  req.response.wait(f.io.waitScope).body->readAllText().wait(f.io.waitScope);

  // ...then shutdown() closes the now-idle connection and serve() resolves.
  KJ_EXPECT(!c.serveTask.poll(f.io.waitScope));
  c.connection->shutdown();
  c.serveTask.wait(f.io.waitScope);
  // wait() consumed the promise; leave a resolved one for ~ConnectionFixture's poll().
  c.serveTask = kj::Promise<void>(kj::READY_NOW);
}

KJ_TEST("hyper server: drain is not held hostage by I/O-stalled connections (regression)") {
  // Regression test for the teardown stall found under client-abort storms (2026-08): clients
  // that stop reading mid-response (write side) or stop sending mid-request-body (read side)
  // while keeping their sockets open — or that vanish in a way the kernel never reports
  // (observed on macOS loopback: no RST, no EPIPE, no EOF, ever) — used to hold drain()
  // forever: hyper waits for socket progress that never comes, the in-flight service call
  // never completes, and a SIGTERM drain wedges for minutes-to-forever. The I/O-stall watchdog
  // (see "I/O-stall watchdog" in server.rs) bounds this: once draining, a connection with zero
  // I/O progress for the drain grace (10 s) is aborted, cancelling its service call. The bound
  // asserted here is deliberately generous (30 s) to absorb CI noise; before the watchdog this
  // test hung forever.
  TestFixture f;

  // Connection A, read-stalled: a fixed-length request body that never finishes. The service
  // (/echo) sits reading request bytes that will never arrive.
  auto connA = f.address->connect().wait(f.io.waitScope);
  {
    kj::StringPtr head =
        "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 1048576\r\n\r\npartial-body";
    connA->write(head.asBytes()).wait(f.io.waitScope);
  }

  // Connection B, write-stalled: request an 8 MiB response and never read a byte of it; the
  // socket buffers fill and hyper's writes stall while the service pumps into the channel.
  auto connB = f.address->connect().wait(f.io.waitScope);
  {
    kj::StringPtr head = "GET /large HTTP/1.1\r\nHost: x\r\n\r\n";
    connB->write(head.asBytes()).wait(f.io.waitScope);
  }

  // Both requests must be in flight (dispatched to the service) before draining.
  f.spinUntil([&]() { return f.service->requestCount >= 2; });

  const kj::MonotonicClock& clock = kj::systemPreciseMonotonicClock();
  auto start = clock.now();
  f.server->drain().wait(f.io.waitScope);
  auto elapsedMs = (clock.now() - start) / kj::MILLISECONDS;
  KJ_EXPECT(elapsedMs < 30000, elapsedMs);

  // The stalled clients' sockets were held open through the whole drain; only drop them now.
  connA = nullptr;
  connB = nullptr;
}

}  // namespace

#endif  // !_WIN32
