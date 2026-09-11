// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// End-to-end tests for the hyper-backed outbound HTTP client: a real kj::HttpServer serves on a
// loopback socket on the test's KJ event loop, while the hyper client (running on the global
// tokio runtime) connects to it over TCP through the kj::HttpService / kj::HttpClient interfaces.

#include "kj-hyper/tests/test-harness.h"

#include <kj-hyper/hyper-http.h>
#include <kj-rs-io/async-io.h>

#include <kj/async-io.h>
#include <kj/compat/http.h>
#include <kj/debug.h>
#include <kj/test.h>
#include <kj/time.h>
#include <kj/vector.h>

#include <cstdlib>
#include <cstring>

namespace {

using namespace kj_hyper_test;

void ensureTokioInitialized() {
  static bool initialized = []() {
    // Shrink the client I/O-stall watchdog's steady grace (default 60s; see
    // kj-hyper/stall.rs) so the stall tests below run in seconds. The variable is
    // read once per process, before the first connection arms a watchdog, so it must be set
    // before any test dials. Harmless for the other tests: their connections always make I/O
    // progress within the shrunk grace, and upgraded (WebSocket/CONNECT) streams are exempt.
    setenv("WORKERD_HYPER_IO_STALL_GRACE_MS", "4000", 1);
    return true;
  }();
  (void)initialized;
}

// The test upstream: a plain kj::HttpService with path-dispatched behaviors.

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
  // The exact Sec-WebSocket-Extensions value the last WebSocket request carried, if any.
  kj::Maybe<kj::String> lastWsExtensions;

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
        // request's extension offer into acceptWebSocket()'s headers, and kj (in
        // MANUAL_COMPRESSION mode) computes the agreement from it.
        kj::HttpHeaders respHeaders(table);
        KJ_IF_SOME(ext, headers.get(kj::HttpHeaderId::SEC_WEBSOCKET_EXTENSIONS)) {
          respHeaders.set(kj::HttpHeaderId::SEC_WEBSOCKET_EXTENSIONS, kj::str(ext));
        }
        auto ws = response.acceptWebSocket(respHeaders);
        co_await wsEcho(*ws);
        co_return;
      } else if (url == "/ws-abort") {
        // Accept, read one message, then abort the connection.
        kj::HttpHeaders respHeaders(table);
        auto ws = response.acceptWebSocket(respHeaders);
        co_await ws->receive();
        ws->abort();
        co_return;
      }
      // Fall through: treat as a regular request (kj lets services ignore the upgrade).
    }

    if (url.startsWith("/hello")) {
      kj::HttpHeaders respHeaders(table);
      respHeaders.set(kj::HttpHeaderId::CONTENT_TYPE, kj::str("text/plain"));
      respHeaders.set(xUrl, kj::str(url));
      auto body = "Hello from KJ server!"_kj;
      auto out = response.send(200, "Everything Is Fine", respHeaders, body.size());
      co_await out->write(body.asBytes());
    } else if (url == "/echo") {
      // Capture the advertised length before reading: kj's entity-body wrapper reports the
      // *remaining* length, which drops to zero once the body is consumed.
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
    } else if (url == "/drain") {
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
    } else if (url == "/slow") {
      // Send headers and a little data, then hang forever (until cancelled at teardown).
      kj::HttpHeaders respHeaders(table);
      auto out = response.send(200, "OK", respHeaders, kj::none);
      co_await out->write("partial"_kjb);
      co_await kj::Promise<void>(kj::NEVER_DONE);
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

struct TestFixture {
  TestFixture() {
    ensureTokioInitialized();

    service = kj::heap<TestHttpService>(
        *ids.table, ids.xEcho, ids.xBinary, ids.xEchoBack, ids.xBinaryResp, ids.xUrl);

    auto addr = io.provider->getNetwork().parseAddress("127.0.0.1", 0).wait(io.waitScope);
    listener = kj::heap<CountingReceiver>(addr->listen());
    // Mirror workerd's kj::HttpServer configuration (server.c++): the application negotiates
    // Sec-WebSocket-Extensions itself.
    server = kj::heap<kj::HttpServer>(io.provider->getTimer(), *ids.table, *service,
        kj::HttpServerSettings{
          .webSocketCompressionMode = kj::HttpServerSettings::MANUAL_COMPRESSION});
    listenTask =
        server->listenHttp(*listener).eagerlyEvaluate([](kj::Exception&& e) { KJ_LOG(ERROR, e); });

    clientService = workerd::rust::kj_hyper::newHyperHttpService(*ids.table, "127.0.0.1",
        listener->getPort(), workerd::rust::kj_hyper::newAllowAllHyperPeerFilter());
    client = kj::newHttpClient(*clientService);
  }

  kj::HttpHeaders makeHeaders() {
    kj::HttpHeaders headers(*ids.table);
    headers.set(kj::HttpHeaderId::HOST, kj::str("test-host"));
    return headers;
  }

  kj::String get(kj::StringPtr url, uint expectedStatus = 200) {
    auto req = client->request(kj::HttpMethod::GET, url, makeHeaders(), uint64_t(0));
    auto resp = req.response.wait(io.waitScope);
    KJ_EXPECT(resp.statusCode == expectedStatus, resp.statusCode);
    return resp.body->readAllText().wait(io.waitScope);
  }

  TokioTestIo io;
  HeaderIds ids;
  kj::Own<TestHttpService> service;
  kj::Own<CountingReceiver> listener;
  kj::Own<kj::HttpServer> server;
  kj::Promise<void> listenTask = nullptr;
  kj::Own<kj::HttpService> clientService;
  kj::Own<kj::HttpClient> client;
};

KJ_TEST("hyper client: GET round trip") {
  TestFixture f;

  auto req = f.client->request(kj::HttpMethod::GET, "/hello?foo=bar", f.makeHeaders(), uint64_t(0));
  auto resp = req.response.wait(f.io.waitScope);

  KJ_EXPECT(resp.statusCode == 200);
  // hyper preserves non-canonical reason phrases from the wire.
  KJ_EXPECT(resp.statusText == "Everything Is Fine", resp.statusText);
  // Builtin indexed response header.
  KJ_EXPECT(KJ_ASSERT_NONNULL(resp.headers->get(kj::HttpHeaderId::CONTENT_TYPE)) == "text/plain");
  // The URL (path + query) passed through verbatim.
  KJ_EXPECT(KJ_ASSERT_NONNULL(resp.headers->get(f.ids.xUrl)) == "/hello?foo=bar");
  // Content-Length is visible and the body streams through.
  KJ_EXPECT(KJ_ASSERT_NONNULL(resp.body->tryGetLength()) == 21);
  auto body = resp.body->readAllText().wait(f.io.waitScope);
  KJ_EXPECT(body == "Hello from KJ server!", body);
}

KJ_TEST("hyper client: HEAD request") {
  TestFixture f;

  auto req = f.client->request(kj::HttpMethod::HEAD, "/hello", f.makeHeaders(), uint64_t(0));
  auto resp = req.response.wait(f.io.waitScope);

  KJ_EXPECT(resp.statusCode == 200);
  KJ_EXPECT(KJ_ASSERT_NONNULL(resp.body->tryGetLength()) == 21);
  auto body = resp.body->readAllText().wait(f.io.waitScope);
  KJ_EXPECT(body == "", body);
}

KJ_TEST("hyper client: POST with streaming chunked request body") {
  TestFixture f;

  // No expectedBodySize: the client must send the request body chunked, streaming each write.
  auto req = f.client->request(kj::HttpMethod::POST, "/echo", f.makeHeaders());
  req.body->write("Hello, "_kjb).wait(f.io.waitScope);
  req.body->write("streaming "_kjb).wait(f.io.waitScope);
  req.body->write("world!"_kjb).wait(f.io.waitScope);
  req.body = nullptr;  // EOF

  auto resp = req.response.wait(f.io.waitScope);
  KJ_EXPECT(resp.statusCode == 200);
  // The server saw no up-front length (chunked).
  auto reqLen = KJ_ASSERT_NONNULL(getHeader(*resp.headers, "X-Request-Length"));
  KJ_EXPECT(reqLen == "unknown", reqLen);
  // Chunked response body (no expected size) streams back.
  KJ_EXPECT(resp.body->tryGetLength() == kj::none);
  auto body = resp.body->readAllText().wait(f.io.waitScope);
  KJ_EXPECT(body == "Hello, streaming world!", body);
}

KJ_TEST("hyper client: POST with known request body length sends Content-Length") {
  TestFixture f;

  auto content = "exact-length body"_kj;
  auto req =
      f.client->request(kj::HttpMethod::POST, "/echo", f.makeHeaders(), uint64_t(content.size()));
  req.body->write(content.asBytes()).wait(f.io.waitScope);
  req.body = nullptr;

  auto resp = req.response.wait(f.io.waitScope);
  KJ_EXPECT(resp.statusCode == 200);
  // The server observed the exact length, i.e. the client sent Content-Length, not chunked.
  auto reqLen = KJ_ASSERT_NONNULL(getHeader(*resp.headers, "X-Request-Length"));
  KJ_EXPECT(reqLen == kj::str(content.size()), reqLen);
  auto body = resp.body->readAllText().wait(f.io.waitScope);
  KJ_EXPECT(body == content, body);
}

KJ_TEST("hyper client: header fidelity (multi-value, non-UTF-8 bytes, both directions)") {
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

  // Request headers arrived at the server intact...
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

KJ_TEST("hyper client: large streamed response body") {
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

KJ_TEST("hyper client: connection reuse after fully-pumped response bodies") {
  TestFixture f;

  // Two sequential large downloads, each consumed to EOF. The response body flows through the
  // tokio-side pump into the frame channel; the channel only reports EOF after the pump
  // consumed hyper's `Incoming` to completion, so by the time the first request resolves the
  // connection is back in the keep-alive pool and the second request must reuse it.
  for (int i = 0; i < 2; i++) {
    auto req = f.client->request(kj::HttpMethod::GET, "/large", f.makeHeaders(), uint64_t(0));
    auto resp = req.response.wait(f.io.waitScope);
    KJ_EXPECT(resp.statusCode == 200);
    auto body = resp.body->readAllBytes().wait(f.io.waitScope);
    KJ_EXPECT(body.size() == LARGE_BODY_SIZE, body.size());
  }
  KJ_EXPECT(f.listener->count == 1, f.listener->count);
}

KJ_TEST("hyper client: large streamed request body") {
  TestFixture f;

  auto req = f.client->request(
      kj::HttpMethod::POST, "/drain", f.makeHeaders(), uint64_t(LARGE_UPLOAD_SIZE));
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

KJ_TEST("hyper client: keep-alive connection reuse and concurrent requests") {
  TestFixture f;

  // Sequential requests reuse a single connection.
  for (int i = 0; i < 3; i++) {
    auto body = f.get("/hello");
    KJ_EXPECT(body == "Hello from KJ server!");
  }
  KJ_EXPECT(f.listener->count == 1, f.listener->count);
  KJ_EXPECT(f.service->requestCount == 3);

  // Concurrent requests each get their own connection (one reused from the pool, two new).
  auto makeRequest = [&]() -> kj::Promise<kj::String> {
    auto req = f.client->request(kj::HttpMethod::GET, "/hello", f.makeHeaders(), uint64_t(0));
    return req.response.then([](kj::HttpClient::Response&& resp) {
      KJ_EXPECT(resp.statusCode == 200);
      auto promise = resp.body->readAllText();
      return promise.attach(kj::mv(resp.body));
    });
  };
  kj::Vector<kj::Promise<kj::String>> promises;
  for (int i = 0; i < 3; i++) {
    promises.add(makeRequest());
  }
  auto results = kj::joinPromisesFailFast(promises.releaseAsArray()).wait(f.io.waitScope);
  for (auto& result: results) {
    KJ_EXPECT(result == "Hello from KJ server!");
  }
  KJ_EXPECT(f.listener->count == 3, f.listener->count);
  KJ_EXPECT(f.service->requestCount == 6);

  // After the burst, the pool has three idle connections; more sequential requests reuse them.
  auto body = f.get("/hello");
  KJ_EXPECT(body == "Hello from KJ server!");
  KJ_EXPECT(f.listener->count == 3, f.listener->count);
}

KJ_TEST("hyper client: connect refused surfaces as DISCONNECTED kj::Exception") {
  TestFixture f;

  // Find a port with no listener: bind an ephemeral port, note it, then close the listener.
  uint deadPort;
  {
    auto addr = f.io.provider->getNetwork().parseAddress("127.0.0.1", 0).wait(f.io.waitScope);
    auto tmpListener = addr->listen();
    deadPort = tmpListener->getPort();
  }

  auto deadService = workerd::rust::kj_hyper::newHyperHttpService(
      *f.ids.table, "127.0.0.1", deadPort, workerd::rust::kj_hyper::newAllowAllHyperPeerFilter());
  auto deadClient = kj::newHttpClient(*deadService);

  auto req = deadClient->request(kj::HttpMethod::GET, "/hello", f.makeHeaders(), uint64_t(0));
  auto maybeException = kj::runCatchingExceptions([&]() { req.response.wait(f.io.waitScope); });
  auto& e = KJ_ASSERT_NONNULL(maybeException, "expected connection-refused exception");
  KJ_EXPECT(e.getType() == kj::Exception::Type::DISCONNECTED, e);
  KJ_EXPECT(strstr(e.getDescription().cStr(), "Connection refused") != nullptr, e);
  // Note: kj-http's own text is a syscall message like "connect(): Connection refused"; the
  // hyper client mimics the prefix but the OS-error suffix differs ("(os error 61)").
  KJ_EXPECT(strstr(e.getDescription().cStr(), "connect()") != nullptr, e);
}

// =======================================================================================
// restrictPeers (the allow/deny network ACL) enforcement on the outbound hyper dial.
//
// The hyper client dials host:port on tokio itself, so it must re-apply the same peer filter kj
// applies inside connect(). Each client carries a HyperPeerFilter and, at every dial, resolves
// the host, checks each resolved address against the filter, and connects to that exact address
// (client.rs `dial_filtered`). These tests prove: a blocked peer fails with kj's exact error and
// never reaches the server; every request re-applies the filter (no unfiltered pooled leg); and
// an allowing rule still lets the peer through.

KJ_TEST("hyper client: restrictPeers blocks a loopback peer with kj's exact error") {
  TestFixture f;

  // allow=["public"] denies loopback (127.0.0.0/8), exactly as kj::Network::restrictPeers would.
  kj::Array<kj::StringPtr> allowRules = kj::heapArray<kj::StringPtr>({"public"_kj});
  auto blockedService = workerd::rust::kj_hyper::newHyperHttpService(*f.ids.table, "127.0.0.1",
      uint16_t(f.listener->getPort()),
      workerd::rust::kj_hyper::newHyperPeerFilter(allowRules, nullptr));
  auto blockedClient = kj::newHttpClient(*blockedService);

  // Two sequential requests: BOTH must fail with the same restrictPeers error. The second proves
  // there is no unfiltered pooled/keep-alive leg -- every dial re-applies the filter, closing the
  // review's "the pooled client keeps dialing unfiltered" concern.
  for (uint i = 0; i < 2; i++) {
    auto req = blockedClient->request(kj::HttpMethod::GET, "/hello", f.makeHeaders(), uint64_t(0));
    auto maybeException = kj::runCatchingExceptions([&]() { req.response.wait(f.io.waitScope); });
    auto& e = KJ_ASSERT_NONNULL(maybeException, "expected restrictPeers block", i);
    // kj's exact error text and type (kj async-io.c++ / kj_rs_io::async-io.c++).
    KJ_EXPECT(e.getType() == kj::Exception::Type::FAILED, e);
    KJ_EXPECT(e.getDescription().contains("connect() blocked by restrictPeers()"_kj),
        e.getDescription(), i);
  }

  // The dial was refused before any TCP connection, so the server accepted nothing.
  KJ_EXPECT(f.listener->count == 0, f.listener->count);
}

KJ_TEST("hyper client: restrictPeers allowing local permits the loopback peer") {
  TestFixture f;

  // allow=["local"] permits 127.0.0.0/8 -- the allowed host must still work, unchanged.
  kj::Array<kj::StringPtr> allowRules = kj::heapArray<kj::StringPtr>({"local"_kj});
  auto service = workerd::rust::kj_hyper::newHyperHttpService(*f.ids.table, "127.0.0.1",
      uint16_t(f.listener->getPort()),
      workerd::rust::kj_hyper::newHyperPeerFilter(allowRules, nullptr));
  auto client = kj::newHttpClient(*service);

  // Two requests: both succeed and reuse a single (filtered-at-dial) keep-alive connection.
  for (uint i = 0; i < 2; i++) {
    auto req = client->request(kj::HttpMethod::GET, "/hello", f.makeHeaders(), uint64_t(0));
    auto resp = req.response.wait(f.io.waitScope);
    KJ_EXPECT(resp.statusCode == 200);
    auto body = resp.body->readAllText().wait(f.io.waitScope);
    KJ_EXPECT(body == "Hello from KJ server!", body);
  }
  KJ_EXPECT(f.listener->count == 1, f.listener->count);
}

KJ_TEST("hyper client: restrictPeers deny of a CIDR blocks a matching loopback peer") {
  TestFixture f;

  // allow=["private"] permits loopback (127.0.0.0/8 is in kj's local set, included by "private"),
  // but deny=["127.0.0.0/8"] takes it back at equal specificity -- kj's allow/deny tie-break.
  // Exercises the deny path, not just an empty-allow deny-all.
  kj::Array<kj::StringPtr> allowRules = kj::heapArray<kj::StringPtr>({"private"_kj});
  kj::Array<kj::StringPtr> denyRules = kj::heapArray<kj::StringPtr>({"127.0.0.0/8"_kj});
  auto blockedService = workerd::rust::kj_hyper::newHyperHttpService(*f.ids.table, "127.0.0.1",
      uint16_t(f.listener->getPort()),
      workerd::rust::kj_hyper::newHyperPeerFilter(allowRules, denyRules));
  auto blockedClient = kj::newHttpClient(*blockedService);

  auto req = blockedClient->request(kj::HttpMethod::GET, "/hello", f.makeHeaders(), uint64_t(0));
  auto maybeException = kj::runCatchingExceptions([&]() { req.response.wait(f.io.waitScope); });
  auto& e = KJ_ASSERT_NONNULL(maybeException, "expected restrictPeers block");
  KJ_EXPECT(e.getType() == kj::Exception::Type::FAILED, e);
  KJ_EXPECT(
      e.getDescription().contains("connect() blocked by restrictPeers()"_kj), e.getDescription());
  KJ_EXPECT(f.listener->count == 0, f.listener->count);
}

KJ_TEST("hyper client: dropping the promise cancels the in-flight request") {
  TestFixture f;

  {
    auto req = f.client->request(kj::HttpMethod::GET, "/slow", f.makeHeaders(), uint64_t(0));
    auto resp = req.response.wait(f.io.waitScope);
    KJ_EXPECT(resp.statusCode == 200);

    // Read a little of the (never-ending) response to prove it is live...
    kj::byte buf[7];
    auto n = resp.body->tryRead(buf, 1, sizeof(buf)).wait(f.io.waitScope);
    KJ_EXPECT(n > 0);

    // ... then drop the response body / request promise mid-response. This must cancel the
    // Rust-side future and with it the in-flight hyper request.
  }
  KJ_EXPECT(f.listener->count == 1, f.listener->count);

  // The client remains usable. The poisoned (mid-response) connection must NOT be reused, so a
  // new connection is dialed.
  auto body = f.get("/hello");
  KJ_EXPECT(body == "Hello from KJ server!");
  KJ_EXPECT(f.listener->count == 2, f.listener->count);
}

KJ_TEST("hyper client: GET with unknown request body length uses chunked encoding") {
  TestFixture f;

  // No expectedBodySize and nothing written: the client sends an empty chunked body.
  auto req = f.client->request(kj::HttpMethod::GET, "/hello", f.makeHeaders());
  req.body = nullptr;
  auto resp = req.response.wait(f.io.waitScope);
  KJ_EXPECT(resp.statusCode == 200);
  auto body = resp.body->readAllText().wait(f.io.waitScope);
  KJ_EXPECT(body == "Hello from KJ server!", body);
}

KJ_TEST("hyper client: 404 response passes through") {
  TestFixture f;

  auto req = f.client->request(kj::HttpMethod::GET, "/nope", f.makeHeaders(), uint64_t(0));
  auto resp = req.response.wait(f.io.waitScope);
  KJ_EXPECT(resp.statusCode == 404);
  KJ_EXPECT(resp.statusText == "Not Found", resp.statusText);
}

KJ_TEST("hyper client: CONNECT to a service that does not implement it gets a 501 rejection") {
  TestFixture f;

  // The kj HttpServer's service does not implement connect(); kj replies 501 through its error
  // handler, which must surface as a rejected CONNECT (status + error body), not an exception.
  auto connReq = f.client->connect("example.com:443", f.makeHeaders(), {});
  auto status = connReq.status.wait(f.io.waitScope);
  KJ_EXPECT(status.statusCode == 501, status.statusCode);
  auto errorBody = KJ_ASSERT_NONNULL(kj::mv(status.errorBody));
  auto body = errorBody->readAllText().wait(f.io.waitScope);
  KJ_EXPECT(
      body.startsWith("ERROR: The server does not implement this operation. Details:\n\n"), body);
}

// ---------------------------------------------------------------------------------------
// WebSocket tests: hyper client -> kj server (via kj::newHttpClient(HttpService&), the shape
// workerd uses), plus raw-socket servers for wire-level fidelity.

KJ_TEST("hyper client -> kj server: WebSocket echo (text, binary, close code)") {
  TestFixture f;

  auto resp = f.client->openWebSocket("/ws-echo", f.makeHeaders()).wait(f.io.waitScope);
  KJ_EXPECT(resp.statusCode == 101, resp.statusCode);
  KJ_EXPECT(KJ_ASSERT_NONNULL(getHeader(*resp.headers, "X-Ws-Accepted")) == "yes");
  auto ws = kj::mv(KJ_ASSERT_NONNULL(resp.webSocketOrBody.tryGet<kj::Own<kj::WebSocket>>()));

  // Text echo.
  ws->send("hello websocket"_kj.asArray()).wait(f.io.waitScope);
  auto textMessage = ws->receive().wait(f.io.waitScope);
  KJ_EXPECT(KJ_ASSERT_NONNULL(textMessage.tryGet<kj::String>()) == "hello websocket");

  // Binary echo (including non-UTF-8 bytes).
  const kj::byte binary[] = {0x01, 0x00, 0xFF, 0x80, 0x7F};
  ws->send(kj::arrayPtr(binary, sizeof(binary))).wait(f.io.waitScope);
  auto binaryMessage = ws->receive().wait(f.io.waitScope);
  KJ_EXPECT(KJ_ASSERT_NONNULL(binaryMessage.tryGet<kj::Array<kj::byte>>()) ==
      kj::arrayPtr(binary, sizeof(binary)));

  // Close: the service echoes our application-chosen code and reason.
  ws->close(4321, "done here").wait(f.io.waitScope);
  auto closeMessage = ws->receive().wait(f.io.waitScope);
  auto& close = KJ_ASSERT_NONNULL(closeMessage.tryGet<kj::WebSocket::Close>());
  KJ_EXPECT(close.code == 4321, close.code);
  KJ_EXPECT(close.reason == "done here", close.reason);
}

KJ_TEST("hyper client: upgraded WebSocket connections are exempt from the stall watchdog") {
  TestFixture f;

  auto resp = f.client->openWebSocket("/ws-echo", f.makeHeaders()).wait(f.io.waitScope);
  KJ_EXPECT(resp.statusCode == 101, resp.statusCode);
  auto ws = kj::mv(KJ_ASSERT_NONNULL(resp.webSocketOrBody.tryGet<kj::Own<kj::WebSocket>>()));

  // Idle well past the (test-shrunk, 4s) stall grace with no I/O at all: an upgraded
  // connection is long-lived by design and must not be reaped by the watchdog (the connection
  // future resolves at the upgrade handoff, taking the watchdog down with it).
  f.io.provider->getTimer().afterDelay(6 * kj::SECONDS).wait(f.io.waitScope);

  ws->send("still alive"_kj.asArray()).wait(f.io.waitScope);
  auto message = ws->receive().wait(f.io.waitScope);
  KJ_EXPECT(KJ_ASSERT_NONNULL(message.tryGet<kj::String>()) == "still alive");
  ws->close(1000, "done").wait(f.io.waitScope);
  ws->receive().wait(f.io.waitScope);
}

KJ_TEST("hyper client -> kj server: Sec-WebSocket-Extensions offer passes through verbatim") {
  TestFixture f;

  // The hyper client must not rewrite or renegotiate the application's extension offer.
  auto offer = "permessage-deflate; client_max_window_bits=10"_kj;
  auto headers = f.makeHeaders();
  headers.set(kj::HttpHeaderId::SEC_WEBSOCKET_EXTENSIONS, kj::str(offer));

  auto resp = f.client->openWebSocket("/ws-echo", kj::mv(headers)).wait(f.io.waitScope);
  KJ_EXPECT(resp.statusCode == 101);
  auto ws = kj::mv(KJ_ASSERT_NONNULL(resp.webSocketOrBody.tryGet<kj::Own<kj::WebSocket>>()));

  // The service observed the exact bytes the application offered.
  KJ_EXPECT(KJ_ASSERT_NONNULL(f.service->lastWsExtensions) == offer);
  // The service did not enable compression (/ws-echo passes no extension config), so the 101
  // carried no Sec-WebSocket-Extensions and the session must be uncompressed but functional.
  KJ_EXPECT(resp.headers->get(kj::HttpHeaderId::SEC_WEBSOCKET_EXTENSIONS) == kj::none);
  ws->send("plain"_kj.asArray()).wait(f.io.waitScope);
  auto message = ws->receive().wait(f.io.waitScope);
  KJ_EXPECT(KJ_ASSERT_NONNULL(message.tryGet<kj::String>()) == "plain");
  ws->close(1000, "").wait(f.io.waitScope);
  ws->receive().wait(f.io.waitScope);
}

KJ_TEST("hyper client -> kj server: manual permessage-deflate round trip") {
  TestFixture f;

  // Drive compression exactly the way workerd does (api/web-socket.c++): the application offers
  // permessage-deflate itself; the kj server (MANUAL_COMPRESSION) service passes the offer into
  // acceptWebSocket(), and kj computes the agreement.
  auto headers = f.makeHeaders();
  headers.set(kj::HttpHeaderId::SEC_WEBSOCKET_EXTENSIONS,
      kj::str("permessage-deflate; client_no_context_takeover; server_max_window_bits=10"));

  auto resp = f.client->openWebSocket("/ws-echo-compress", kj::mv(headers)).wait(f.io.waitScope);
  KJ_EXPECT(resp.statusCode == 101);
  auto agreed = KJ_ASSERT_NONNULL(resp.headers->get(kj::HttpHeaderId::SEC_WEBSOCKET_EXTENSIONS));
  KJ_EXPECT(agreed.startsWith("permessage-deflate"), agreed);
  auto ws = kj::mv(KJ_ASSERT_NONNULL(resp.webSocketOrBody.tryGet<kj::Own<kj::WebSocket>>()));

  // Round-trip several highly-compressible messages (multiple, to exercise the
  // context-takeover reset paths on both sides), interleaved with a binary one.
  for (int i = 0; i < 3; i++) {
    auto big = kj::strArray(kj::repeat(kj::StringPtr("compress me! "), 4096), "");
    ws->send(big.asArray()).wait(f.io.waitScope);
    auto message = ws->receive().wait(f.io.waitScope);
    KJ_EXPECT(KJ_ASSERT_NONNULL(message.tryGet<kj::String>()) == big);
  }
  const kj::byte binary[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0xDE, 0xAD, 0xBE, 0xEF};
  ws->send(kj::arrayPtr(binary, sizeof(binary))).wait(f.io.waitScope);
  auto binaryMessage = ws->receive().wait(f.io.waitScope);
  KJ_EXPECT(KJ_ASSERT_NONNULL(binaryMessage.tryGet<kj::Array<kj::byte>>()) ==
      kj::arrayPtr(binary, sizeof(binary)));

  ws->close(1000, "bye").wait(f.io.waitScope);
  auto closeMessage = ws->receive().wait(f.io.waitScope);
  KJ_EXPECT(KJ_ASSERT_NONNULL(closeMessage.tryGet<kj::WebSocket::Close>()).code == 1000);
}

KJ_TEST("hyper client -> kj server: server abort surfaces as an exception") {
  TestFixture f;

  auto resp = f.client->openWebSocket("/ws-abort", f.makeHeaders()).wait(f.io.waitScope);
  KJ_EXPECT(resp.statusCode == 101);
  auto ws = kj::mv(KJ_ASSERT_NONNULL(resp.webSocketOrBody.tryGet<kj::Own<kj::WebSocket>>()));

  ws->send("trigger"_kj.asArray()).wait(f.io.waitScope);
  auto maybeException = kj::runCatchingExceptions([&]() { ws->receive().wait(f.io.waitScope); });
  auto& e = KJ_ASSERT_NONNULL(maybeException, "expected receive() to fail after server abort");
  KJ_EXPECT(e.getType() == kj::Exception::Type::DISCONNECTED, e);
}

// ---------------------------------------------------------------------------------------
// Raw-socket servers: wire-level fidelity for the hyper client's handshake and framing.

// Extracts a header value. The name lookup is case-insensitive (pass it in lowercase): the
// client serializes names in Title-Case on the wire (title_case_headers, matching kj's
// registered-case serialization), e.g. "Sec-Websocket-Key".
kj::String extractHeader(kj::StringPtr head, kj::StringPtr name) {
  auto lowerHead = kj::str(head);
  for (char& c: lowerHead) {
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  }
  auto prefix = kj::str(name, ": ");
  const char* found = strstr(lowerHead.cStr(), prefix.cStr());
  KJ_ASSERT(found != nullptr, "header not found in request head", name, head);
  const char* start = head.cStr() + (found - lowerHead.cStr()) + prefix.size();
  const char* end = strstr(start, "\r\n");
  KJ_ASSERT(end != nullptr, "unterminated header line", name, head);
  return kj::heapString(start, end - start);
}

kj::String acceptKeyFor(kj::StringPtr key) {
  auto result = workerd::rust::kj_hyper::websocket_accept_key(
      ::rust::Slice<const uint8_t>(key.asBytes().begin(), key.size()));
  return kj::heapString(result.data(), result.size());
}

// A raw TCP server for driving the hyper client's WebSocket path byte by byte.
struct RawServerFixture {
  explicit RawServerFixture(TestFixture& f): f(f) {
    auto addr = f.io.provider->getNetwork().parseAddress("127.0.0.1", 0).wait(f.io.waitScope);
    listener = addr->listen();
    clientService = workerd::rust::kj_hyper::newHyperHttpService(*f.ids.table, "127.0.0.1",
        uint16_t(listener->getPort()), workerd::rust::kj_hyper::newAllowAllHyperPeerFilter());
    client = kj::newHttpClient(*clientService);
  }

  // Accepts the connection and performs the server half of the WebSocket handshake, echoing
  // back any extension header verbatim if requested.
  kj::Own<kj::AsyncIoStream> acceptHandshake(kj::StringPtr extraResponseHeaders = "") {
    auto stream = listener->accept().wait(f.io.waitScope);
    auto head = readHttpHead(*stream, f.io.waitScope);
    KJ_EXPECT(head.startsWith("GET"), head);
    KJ_EXPECT(extractHeader(head, "upgrade") == "websocket" ||
            extractHeader(head, "upgrade") == "Websocket",
        head);
    auto key = extractHeader(head, "sec-websocket-key");
    auto response = kj::str("HTTP/1.1 101 Switching Protocols\r\n"
                            "Upgrade: websocket\r\n"
                            "Connection: Upgrade\r\n"
                            "Sec-WebSocket-Accept: ",
        acceptKeyFor(key), "\r\n", extraResponseHeaders, "\r\n");
    stream->write(response.asBytes()).wait(f.io.waitScope);
    return stream;
  }

  TestFixture& f;
  kj::Own<kj::ConnectionReceiver> listener;
  kj::Own<kj::HttpService> clientService;
  kj::Own<kj::HttpClient> client;
};

KJ_TEST("hyper client (raw server): fragmented message reassembly and ping/pong") {
  TestFixture f;
  RawServerFixture raw(f);

  auto wsPromise = raw.client->openWebSocket("/", f.makeHeaders());
  auto stream = raw.acceptHandshake();
  auto resp = wsPromise.wait(f.io.waitScope);
  KJ_EXPECT(resp.statusCode == 101);
  auto ws = kj::mv(KJ_ASSERT_NONNULL(resp.webSocketOrBody.tryGet<kj::Own<kj::WebSocket>>()));

  // Server sends a text message split into three fragments, with a ping wedged between the
  // fragments (interleaved control frames are legal and kj handles them mid-message).
  stream->write(makeFrame(false, 1, "Hello, "_kj.asBytes())).wait(f.io.waitScope);
  stream->write(makeFrame(true, 9, "marco"_kj.asBytes())).wait(f.io.waitScope);  // ping
  stream->write(makeFrame(false, 0, "fragmented "_kj.asBytes())).wait(f.io.waitScope);
  stream->write(makeFrame(true, 0, "world!"_kj.asBytes())).wait(f.io.waitScope);

  auto message = ws->receive().wait(f.io.waitScope);
  KJ_EXPECT(KJ_ASSERT_NONNULL(message.tryGet<kj::String>()) == "Hello, fragmented world!");

  // The client must have auto-ponged the ping with the same payload (kj semantics), masked
  // (client side).
  auto pong = readFrame(*stream, f.io.waitScope);
  KJ_EXPECT(pong.opcode == 10, pong.opcode);
  KJ_EXPECT(kj::str(pong.payload.asChars()) == "marco");

  // Clean close initiated by the server; client app echoes it.
  kj::byte closePayload[] = {0x0F, 0xA1, 'b', 'y', 'e'};  // 4001 + "bye"
  stream->write(makeFrame(true, 8, kj::arrayPtr(closePayload, sizeof(closePayload))))
      .wait(f.io.waitScope);
  auto closeMessage = ws->receive().wait(f.io.waitScope);
  auto& close = KJ_ASSERT_NONNULL(closeMessage.tryGet<kj::WebSocket::Close>());
  KJ_EXPECT(close.code == 4001, close.code);
  KJ_EXPECT(close.reason == "bye", close.reason);
  ws->close(close.code, close.reason).wait(f.io.waitScope);
  auto closeEcho = readFrame(*stream, f.io.waitScope);
  KJ_EXPECT(closeEcho.opcode == 8);
  KJ_EXPECT(closeEcho.payload.size() >= 2);
  KJ_EXPECT((uint(closeEcho.payload[0]) << 8 | closeEcho.payload[1]) == 4001);
}

KJ_TEST("hyper client (raw server): handshake validation failures match kj") {
  TestFixture f;

  {
    // Wrong Sec-WebSocket-Accept.
    RawServerFixture raw(f);
    auto wsPromise = raw.client->openWebSocket("/", f.makeHeaders());
    auto stream = raw.listener->accept().wait(f.io.waitScope);
    readHttpHead(*stream, f.io.waitScope);
    stream
        ->write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                "Connection: Upgrade\r\nSec-WebSocket-Accept: bogus\r\n\r\n"_kjb)
        .wait(f.io.waitScope);
    auto maybeException = kj::runCatchingExceptions([&]() { wsPromise.wait(f.io.waitScope); });
    auto& e = KJ_ASSERT_NONNULL(maybeException, "expected handshake failure");
    KJ_EXPECT(
        strstr(e.getDescription().cStr(),
            "Server failed WebSocket handshake: incorrect Sec-WebSocket-Accept header") != nullptr,
        e);
  }

  {
    // 101 without an Upgrade header (kj rejects before looking at anything else). Note this
    // covers the "101-with-body" quirk too: a 101 that is not a valid upgrade is rejected
    // rather than treated as a response with a body.
    RawServerFixture raw(f);
    auto wsPromise = raw.client->openWebSocket("/", f.makeHeaders());
    auto stream = raw.listener->accept().wait(f.io.waitScope);
    readHttpHead(*stream, f.io.waitScope);
    stream->write("HTTP/1.1 101 Switching Protocols\r\nContent-Length: 5\r\n\r\nhello"_kjb)
        .wait(f.io.waitScope);
    auto maybeException = kj::runCatchingExceptions([&]() { wsPromise.wait(f.io.waitScope); });
    auto& e = KJ_ASSERT_NONNULL(maybeException, "expected handshake failure");
    KJ_EXPECT(strstr(e.getDescription().cStr(),
                  "Server failed WebSocket handshake: missing Upgrade header.") != nullptr,
        e);
  }
}

KJ_TEST("hyper client (raw server): non-101 response to an upgrade is a regular response") {
  TestFixture f;
  RawServerFixture raw(f);

  auto wsPromise = raw.client->openWebSocket("/", f.makeHeaders());
  auto stream = raw.listener->accept().wait(f.io.waitScope);
  readHttpHead(*stream, f.io.waitScope);
  stream->write("HTTP/1.1 403 Forbidden\r\nContent-Length: 6\r\n\r\ndenied"_kjb)
      .wait(f.io.waitScope);
  auto resp = wsPromise.wait(f.io.waitScope);
  KJ_EXPECT(resp.statusCode == 403, resp.statusCode);
  auto& body = KJ_ASSERT_NONNULL(resp.webSocketOrBody.tryGet<kj::Own<kj::AsyncInputStream>>());
  KJ_EXPECT(body->readAllText().wait(f.io.waitScope) == "denied");
}

KJ_TEST("hyper client (raw server): mid-stream response failure preserves the error text") {
  TestFixture f;
  RawServerFixture raw(f);

  auto req = raw.client->request(kj::HttpMethod::GET, "/", f.makeHeaders(), uint64_t(0));
  auto stream = raw.listener->accept().wait(f.io.waitScope);
  readHttpHead(*stream, f.io.waitScope);
  stream->write("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\npartial"_kjb).wait(f.io.waitScope);
  auto resp = req.response.wait(f.io.waitScope);
  KJ_EXPECT(resp.statusCode == 200);
  KJ_EXPECT(KJ_ASSERT_NONNULL(resp.body->tryGetLength()) == 100);

  // Close the connection mid-body (100 bytes promised, 7 sent): the pumped body must surface
  // hyper's mid-stream error with the pre-pump error text and DISCONNECTED type.
  stream = nullptr;
  auto maybeException =
      kj::runCatchingExceptions([&]() { resp.body->readAllBytes().wait(f.io.waitScope); });
  auto& e = KJ_ASSERT_NONNULL(maybeException, "expected mid-stream body failure");
  KJ_EXPECT(e.getType() == kj::Exception::Type::DISCONNECTED, e);
  KJ_EXPECT(strstr(e.getDescription().cStr(), "read HTTP response body") != nullptr, e);
}

KJ_TEST("hyper client (raw server): I/O-stall watchdog fails a download from a silent server") {
  TestFixture f;
  RawServerFixture raw(f);

  auto req = raw.client->request(kj::HttpMethod::GET, "/", f.makeHeaders(), uint64_t(0));
  auto stream = raw.listener->accept().wait(f.io.waitScope);
  readHttpHead(*stream, f.io.waitScope);
  // Headers plus a partial body...
  stream->write("HTTP/1.1 200 OK\r\nContent-Length: 1048576\r\n\r\n"_kjb).wait(f.io.waitScope);
  auto partial = kj::heapArray<kj::byte>(64 * 1024);
  memset(partial.begin(), 'x', partial.size());
  stream->write(partial).wait(f.io.waitScope);
  auto resp = req.response.wait(f.io.waitScope);
  KJ_EXPECT(resp.statusCode == 200);

  // ... then the server goes silent, holding the socket open: no more bytes, no FIN, no RST,
  // so the client kernel never reports anything. The I/O-stall watchdog (steady grace shrunk
  // to 4s via WORKERD_HYPER_IO_STALL_GRACE_MS in ensureTokioInitialized) must abort the
  // connection and fail the read instead of letting it hang forever.
  auto& clock = kj::systemPreciseMonotonicClock();
  auto start = clock.now();
  auto maybeException =
      kj::runCatchingExceptions([&]() { resp.body->readAllBytes().wait(f.io.waitScope); });
  auto elapsed = clock.now() - start;
  auto& e = KJ_ASSERT_NONNULL(maybeException, "expected the stalled download to fail");
  KJ_EXPECT(e.getType() == kj::Exception::Type::DISCONNECTED, e);
  KJ_EXPECT(strstr(e.getDescription().cStr(), "read HTTP response body") != nullptr, e);
  // Generous CI bound over the 4s grace + 1s watchdog check interval.
  KJ_EXPECT(elapsed < 30 * kj::SECONDS, elapsed / kj::MILLISECONDS);

  // The client stays usable afterwards: the aborted connection is not reused (a fresh one is
  // dialed) and the next request completes normally.
  auto req2 = raw.client->request(kj::HttpMethod::GET, "/", f.makeHeaders(), uint64_t(0));
  auto stream2 = raw.listener->accept().wait(f.io.waitScope);
  readHttpHead(*stream2, f.io.waitScope);
  stream2->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"_kjb).wait(f.io.waitScope);
  auto resp2 = req2.response.wait(f.io.waitScope);
  KJ_EXPECT(resp2.statusCode == 200);
  KJ_EXPECT(resp2.body->readAllText().wait(f.io.waitScope) == "ok");
}

KJ_TEST("hyper client (raw server): a response head slower than the stall grace is not reaped") {
  TestFixture f;
  RawServerFixture raw(f);

  // Waiting for a response *head* must follow kj semantics: wait indefinitely, exempt from the
  // I/O-stall watchdog (only mid-response silence marks a dead origin — previous test). A
  // server may legitimately take arbitrarily long to start responding; kj's own HttpServer
  // error path (e.g. the 501 for CONNECT to a service that does not implement it) can take
  // longer than the test-shrunk 4s grace to serialize the error. Before the awaiting-response
  // exemption the watchdog dropped the connection here and the request failed with hyper's
  // "dispatch task is gone" instead of ever seeing the response.
  auto req = raw.client->request(kj::HttpMethod::GET, "/", f.makeHeaders(), uint64_t(0));
  auto stream = raw.listener->accept().wait(f.io.waitScope);
  readHttpHead(*stream, f.io.waitScope);
  // Sit silent past the 4s grace plus the 1s watchdog sampling interval, then respond.
  f.io.provider->getTimer().afterDelay(6 * kj::SECONDS).wait(f.io.waitScope);
  stream->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"_kjb).wait(f.io.waitScope);
  auto resp = req.response.wait(f.io.waitScope);
  KJ_EXPECT(resp.statusCode == 200, resp.statusCode);
  KJ_EXPECT(resp.body->readAllText().wait(f.io.waitScope) == "ok");
}

// ---------------------------------------------------------------------------------------
// CONNECT tests: hyper client -> kj server.

KJ_TEST("hyper client -> kj server: CONNECT tunnel carries bidirectional bytes") {
  TestFixture f;

  auto req = f.client->connect("echo-host:1234", f.makeHeaders(), {});
  auto status = req.status.wait(f.io.waitScope);
  KJ_EXPECT(status.statusCode == 200, status.statusCode);
  KJ_EXPECT(KJ_ASSERT_NONNULL(getHeader(*status.headers, "X-Tunnel")) == "ok");

  auto roundTrip = [&](kj::StringPtr data) {
    req.connection->write(data.asBytes()).wait(f.io.waitScope);
    auto buf = kj::heapArray<kj::byte>(data.size());
    auto n = req.connection->tryRead(buf.begin(), buf.size(), buf.size()).wait(f.io.waitScope);
    KJ_ASSERT(n == buf.size());
    KJ_EXPECT(kj::str(buf.asChars()) == data);
  };
  roundTrip("first payload");
  roundTrip("second payload, slightly longer");

  // Half-close propagates: after we shut down our write end, the echo service sees EOF and
  // shuts down its side, so our reads hit EOF.
  req.connection->shutdownWrite();
  kj::byte buf[16];
  auto n = req.connection->tryRead(buf, 1, sizeof(buf)).wait(f.io.waitScope);
  KJ_EXPECT(n == 0, n);
}

KJ_TEST("hyper client -> kj server: CONNECT early client close") {
  TestFixture f;

  auto req = f.client->connect("echo-host:1234", f.makeHeaders(), {});
  // Write and close immediately, without even waiting for the 200: the data and EOF must still
  // make it through, echoed back before the tunnel winds down.
  req.connection->write("goodbye"_kjb).wait(f.io.waitScope);
  req.connection->shutdownWrite();

  auto status = req.status.wait(f.io.waitScope);
  KJ_EXPECT(status.statusCode == 200, status.statusCode);
  auto echoed = req.connection->readAllText().wait(f.io.waitScope);
  KJ_EXPECT(echoed == "goodbye", echoed);
}

KJ_TEST("hyper client -> kj server: CONNECT rejection carries status and body") {
  TestFixture f;

  auto req = f.client->connect("reject-host:99", f.makeHeaders(), {});
  auto status = req.status.wait(f.io.waitScope);
  KJ_EXPECT(status.statusCode == 403, status.statusCode);
  KJ_EXPECT(status.statusText == "Forbidden", status.statusText);
  auto errorBody = KJ_ASSERT_NONNULL(kj::mv(status.errorBody));
  auto body = errorBody->readAllText().wait(f.io.waitScope);
  KJ_EXPECT(body == "tunnel rejected", body);
}

}  // namespace
