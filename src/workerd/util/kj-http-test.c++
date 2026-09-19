// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// kj's HTTP entry points as workerd calls them -- kj::HttpServer and kj::newHttpClient() -- checked
// against the wire and against kj's contracts. The same tests
// run on kj's own implementation (cxx backend) and on kj-hyper (rust backend), so they pin the
// behavior workerd relies on regardless of which one is linked.

#include <kj/async-io.h>
#include <kj/compat/http.h>
#include <kj/debug.h>
#include <kj/test.h>

#include <cstdlib>

namespace workerd {
namespace {

class TestService final: public kj::HttpService {
 public:
  TestService(kj::HttpHeaderTable& table, kj::HttpHeaderId xCustom)
      : table(table),
        xCustom(xCustom) {}

  bool sawContentLength = false;
  // Fulfilled when a hanging request is cancelled.
  kj::PromiseFulfillerPair<void> cancelled = kj::newPromiseAndFulfiller<void>();
  // Requests to /gate wait on this; `inFlight` counts them.
  kj::PromiseFulfillerPair<void> gateOpen = kj::newPromiseAndFulfiller<void>();
  kj::ForkedPromise<void> gate = gateOpen.promise.fork();
  kj::uint inFlight = 0;
  kj::uint requestCount = 0;
  bool receiveFailed = false;
  kj::Maybe<kj::String> overwriteError;

  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      Response& response) override {
    kj::HttpHeaders responseHeaders(table);
    ++requestCount;
    if (headers.isWebSocket()) {
      if (url == "/ws-deflate") {
        // As workerd does in MANUAL_COMPRESSION mode: accept what the client offered.
        KJ_IF_SOME(offer, headers.get(kj::HttpHeaderId::SEC_WEBSOCKET_EXTENSIONS)) {
          responseHeaders.set(kj::HttpHeaderId::SEC_WEBSOCKET_EXTENSIONS, kj::str(offer));
        }
      }
      if (url == "/ws-refuse") {
        response.send(403, "Nope", responseHeaders, uint64_t(0));
        co_return;
      }
      auto ws = response.acceptWebSocket(responseHeaders);
      if (url == "/ws-hang") {
        KJ_DEFER(cancelled.fulfiller->fulfill());
        co_await kj::Promise<void>(kj::NEVER_DONE);
      }
      if (url == "/ws-extensions") {
        // Reports the offer it received, verbatim.
        co_await ws->send(
            headers.get(kj::HttpHeaderId::SEC_WEBSOCKET_EXTENSIONS).orDefault("none"));
        co_await ws->close(1000, "");
        co_return;
      }
      if (url == "/ws-small") {
        try {
          co_await ws->receive(16);
        } catch (...) {
          receiveFailed = true;
        }
        co_return;
      }
      if (url == "/ws-abort") {
        co_await ws->receive();
        ws->abort();
        co_return;
      }
      if (url == "/ws-app-close") {
        // Answers the peer's Close with a different one of its own; a protocol violation ends it.
        try {
          auto message = co_await ws->receive();
          KJ_EXPECT(message.is<kj::WebSocket::Close>());
          co_await ws->close(4000, "app");
        } catch (...) {}
        co_return;
      }
      for (;;) {
        auto message = co_await ws->receive();
        KJ_SWITCH_ONEOF(message) {
          KJ_CASE_ONEOF(text, kj::String) {
            co_await ws->send(text);
          }
          KJ_CASE_ONEOF(data, kj::Array<kj::byte>) {
            co_await ws->send(data);
          }
          KJ_CASE_ONEOF(close, kj::WebSocket::Close) {
            co_await ws->close(close.code, close.reason);
            co_return;
          }
        }
      }
    } else if (url == "/echo") {
      auto text = co_await requestBody.readAllText();
      responseHeaders.setPtr(xCustom, "v");
      auto out = response.send(403, "Unauthorized", responseHeaders, text.size());
      co_await out->write(text.asBytes());
    } else if (url == "/head-length") {
      // A response to HEAD that describes the representation a GET would get.
      responseHeaders.setPtr(kj::HttpHeaderId::CONTENT_LENGTH, "123");
      response.send(200, "OK", responseHeaders, uint64_t(0));
    } else if (url == "/overwrite") {
      auto out = response.send(200, "OK", responseHeaders, uint64_t(5));
      try {
        co_await out->write("0123456789"_kj.asBytes());
      } catch (...) {
        overwriteError = kj::str(kj::getCaughtExceptionAsKj().getDescription());
      }
      co_await out->write("hello"_kj.asBytes());
    } else if (url == "/hello") {
      auto out = response.send(200, "OK", responseHeaders, uint64_t(5));
      co_await out->write("hello"_kj.asBytes());
    } else if (url == "/no-content") {
      response.send(204, "No Content", responseHeaders, uint64_t(0));
    } else if (url == "/stream") {
      // Echoes the request body, with the response length unknown.
      auto out = response.send(200, "OK", responseHeaders, kj::none);
      co_await requestBody.pumpTo(*out);
    } else if (url == "/framing") {
      // Reports how the request body was framed and how long it was.
      auto text = co_await requestBody.readAllText();
      auto report = kj::str(
          "content-length=", headers.get(kj::HttpHeaderId::CONTENT_LENGTH).orDefault("none"),
          " transfer-encoding=", headers.get(kj::HttpHeaderId::TRANSFER_ENCODING).orDefault("none"),
          " size=", text.size());
      auto out = response.send(200, "OK", responseHeaders, report.size());
      co_await out->write(report.asBytes());
    } else if (url == "/count") {
      auto body = co_await requestBody.readAllBytes();
      auto report = kj::str(body.size());
      auto out = response.send(200, "OK", responseHeaders, report.size());
      co_await out->write(report.asBytes());
    } else if (url == "/large") {
      // 8 MiB in 64 KiB writes, each waiting on the reader.
      auto out = response.send(200, "OK", responseHeaders, uint64_t(8 << 20));
      auto chunk = kj::heapArray<kj::byte>(64 << 10);
      chunk.asPtr().fill('x');
      for (auto i: kj::range(0, 128)) {
        (void)i;
        co_await out->write(chunk);
      }
    } else if (url == "/headers") {
      // Echoes every request header it did not recognize, in order.
      headers.forEach([&](kj::StringPtr name, kj::StringPtr value) {
        if (name.startsWith("X-") || name.startsWith("x-")) {
          responseHeaders.add(kj::str(name), kj::str(value));
        }
      });
      response.send(200, "OK", responseHeaders, uint64_t(0));
    } else if (url == "/not-found") {
      auto out = response.send(404, "Not Found", responseHeaders, uint64_t(7));
      co_await out->write("missing"_kj.asBytes());
    } else if (url == "/not-modified") {
      response.send(304, "Not Modified", responseHeaders, uint64_t(0));
    } else if (url == "/gate") {
      ++inFlight;
      co_await gate.addBranch();
      --inFlight;
      response.send(200, "OK", responseHeaders, uint64_t(0));
    } else if (url == "/hang") {
      KJ_DEFER(cancelled.fulfiller->fulfill());
      co_await kj::Promise<void>(kj::NEVER_DONE);
    } else if (url == "/hang-mid-body") {
      KJ_DEFER(cancelled.fulfiller->fulfill());
      auto out = response.send(200, "OK", responseHeaders, uint64_t(10));
      co_await out->write("hello"_kj.asBytes());
      co_await kj::Promise<void>(kj::NEVER_DONE);
    } else if (url == "/unimplemented") {
      KJ_UNIMPLEMENTED("not here");
    } else if (url == "/disconnected") {
      kj::throwFatalException(KJ_EXCEPTION(DISCONNECTED, "gone"));
    } else if (url == "/fail-mid-body") {
      auto out = response.send(200, "OK", responseHeaders, uint64_t(10));
      co_await out->write("hello"_kj.asBytes());
      KJ_FAIL_REQUIRE("failed mid-body");
    } else if (url == "/no-response") {
      co_return;
    } else if (url == "/accept-plain") {
      response.acceptWebSocket(responseHeaders);
    } else if (url == "/throw") {
      KJ_FAIL_REQUIRE("the service failed");
    } else {
      sawContentLength = headers.get(kj::HttpHeaderId::CONTENT_LENGTH) != kj::none;
      response.send(200, "OK", responseHeaders, uint64_t(0));
    }
  }

  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::HttpConnectSettings settings) override {
    if (host == "throw.example:1") {
      KJ_FAIL_REQUIRE("the tunnel failed");
    }
    if (host == "reject.example:1") {
      auto body = response.reject(403, "Forbidden", kj::HttpHeaders(table), uint64_t(4));
      co_await body->write("nope"_kj.asBytes());
      co_return;
    }
    response.accept(200, "OK", kj::HttpHeaders(table));
    if (host == "hang.example:1") {
      KJ_DEFER(cancelled.fulfiller->fulfill());
      co_await kj::Promise<void>(kj::NEVER_DONE);
    }
    kj::byte buffer[256];
    for (;;) {
      auto n = co_await connection.tryRead(buffer, 1, sizeof(buffer));
      if (n == 0) co_return;
      co_await connection.write(kj::arrayPtr(buffer, n));
    }
  }

 private:
  kj::HttpHeaderTable& table;
  kj::HttpHeaderId xCustom;
};

class TestEntropySource final: public kj::EntropySource {
 public:
  void generate(kj::ArrayPtr<kj::byte> buffer) override {
    buffer.fill(4);
  }
};

struct Fixture {
  kj::AsyncIoContext io = kj::setupAsyncIo();
  kj::WaitScope& ws = io.waitScope;
  kj::Timer& timer = io.provider->getTimer();
  kj::HttpHeaderTable::Builder builder;
  kj::HttpHeaderId xCustom = builder.add("X-Custom-Header");
  kj::Own<kj::HttpHeaderTable> table = builder.build();
  TestService service{*table, xCustom};
  kj::HttpServer server{timer, *table, service,
    {.webSocketCompressionMode = kj::HttpServerSettings::MANUAL_COMPRESSION}};
  kj::Own<kj::ConnectionReceiver> listener =
      io.provider->getNetwork().parseAddress("127.0.0.1", 0).wait(ws)->listen();
  kj::Promise<void> listening = server.listenHttp(*listener).eagerlyEvaluate(nullptr);

  kj::Own<kj::NetworkAddress> address() {
    return io.provider->getNetwork().parseAddress("127.0.0.1", listener->getPort()).wait(ws);
  }

  kj::Own<kj::AsyncIoStream> connect() {
    return address()->connect().wait(ws);
  }
};

kj::String readBytes(kj::WaitScope& ws, kj::AsyncInputStream& stream, size_t size) {
  auto buffer = kj::heapArray<char>(size);
  auto n = stream.tryRead(buffer.begin(), size, size).wait(ws);
  return kj::heapString(buffer.first(n));
}

bool isEof(kj::WaitScope& ws, kj::AsyncInputStream& stream) {
  char c;
  return stream.tryRead(&c, 1, 1).wait(ws) == 0;
}

kj::Promise<kj::String> readHead(kj::AsyncInputStream& stream) {
  kj::Vector<char> head;
  while (!head.asPtr().endsWith("\r\n\r\n"_kjc)) {
    char c;
    if (co_await stream.tryRead(&c, 1, 1) == 0) break;
    head.add(c);
  }
  co_return kj::heapString(head.asPtr());
}

// A WebSocket handshake on a raw connection; returns the response head.
kj::String upgrade(Fixture& f, kj::AsyncIoStream& conn, kj::StringPtr path) {
  conn.write(kj::str("GET ", path,
                 " HTTP/1.1\r\nHost: foo\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                 "Sec-WebSocket-Key: AAAAAAAAAAAAAAAAAAAAAA==\r\nSec-WebSocket-Version: 13\r\n\r\n")
                 .asBytes())
      .wait(f.ws);
  return readHead(conn).wait(f.ws);
}

kj::String bytes(std::initializer_list<kj::byte> b) {
  return kj::heapString(kj::arrayPtr(b.begin(), b.size()).asChars());
}

KJ_TEST("server writes the service's header spellings, reason phrase and body") {
  Fixture f;
  auto conn = f.connect();
  conn->write("POST /echo HTTP/1.1\r\nHost: foo\r\nContent-Length: 5\r\n\r\nhello"_kj.asBytes())
      .wait(f.ws);
  auto expected =
      "HTTP/1.1 403 Unauthorized\r\nContent-Length: 5\r\nX-Custom-Header: v\r\n\r\nhello"_kj;
  KJ_EXPECT(readBytes(f.ws, *conn, expected.size()) == expected);
}

KJ_TEST("client round trip, and an empty GET carries no Content-Length") {
  Fixture f;
  auto addr = f.address();
  auto client = kj::newHttpClient(f.timer, *f.table, *addr, {});
  kj::HttpHeaders headers(*f.table);
  headers.setPtr(kj::HttpHeaderId::HOST, "foo");

  auto empty = client->request(kj::HttpMethod::GET, "/length", headers, uint64_t(0));
  auto emptyResponse = empty.response.wait(f.ws);
  KJ_EXPECT(emptyResponse.statusCode == 200);
  emptyResponse.body->readAllText().wait(f.ws);
  KJ_EXPECT(!f.service.sawContentLength);

  auto request = client->request(kj::HttpMethod::POST, "/echo", headers, uint64_t(5));
  request.body->write("hello"_kj.asBytes()).wait(f.ws);
  request.body = nullptr;
  auto response = request.response.wait(f.ws);
  KJ_EXPECT(response.statusCode == 403);
  KJ_EXPECT(response.statusText == "Unauthorized");
  KJ_EXPECT(KJ_ASSERT_NONNULL(response.headers->get(f.xCustom)) == "v");
  KJ_EXPECT(response.body->readAllText().wait(f.ws) == "hello");
}

KJ_TEST("WebSocket echo over the client and server") {
  Fixture f;
  auto addr = f.address();
  TestEntropySource entropy;
  auto client = kj::newHttpClient(f.timer, *f.table, *addr, {.entropySource = entropy});
  kj::HttpHeaders headers(*f.table);
  headers.setPtr(kj::HttpHeaderId::HOST, "foo");
  auto response = client->openWebSocket("/ws", headers).wait(f.ws);
  KJ_ASSERT(response.statusCode == 101);
  auto& ws = *response.webSocketOrBody.get<kj::Own<kj::WebSocket>>();
  ws.send("hi"_kj).wait(f.ws);
  KJ_EXPECT(ws.receive().wait(f.ws).get<kj::String>() == "hi");
  ws.close(1000, "bye").wait(f.ws);
  auto close = ws.receive().wait(f.ws).get<kj::WebSocket::Close>();
  KJ_EXPECT(close.code == 1000);
  KJ_EXPECT(close.reason == "bye");
}

KJ_TEST("server negotiates permessage-deflate and compresses with context takeover") {
  Fixture f;
  auto conn = f.connect();
  conn->write("GET /ws-deflate HTTP/1.1\r\nHost: foo\r\nUpgrade: websocket\r\n"
              "Connection: Upgrade\r\nSec-WebSocket-Key: AAAAAAAAAAAAAAAAAAAAAA==\r\n"
              "Sec-WebSocket-Version: 13\r\n"
              "Sec-WebSocket-Extensions: permessage-deflate; client_max_window_bits\r\n\r\n"_kj
                  .asBytes())
      .wait(f.ws);
  auto expected =
      "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n"
      "Sec-WebSocket-Accept: ICX+Yqv66kxgM0FcWaLWlFLwTAI=\r\n"
      "Sec-WebSocket-Extensions: permessage-deflate; client_max_window_bits=15\r\n\r\n"_kj;
  KJ_EXPECT(readBytes(f.ws, *conn, expected.size()) == expected);

  // RFC 7692 section 7.2.3.2: "Hello" twice; the second refers back to the first.
  static constexpr kj::byte HELLO_FIRST[] = {0xc1, 0x07, 0xf2, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00};
  static constexpr kj::byte HELLO_AGAIN[] = {0xc1, 0x05, 0xf2, 0x00, 0x11, 0x00, 0x00};
  for (auto frame: {kj::arrayPtr(HELLO_FIRST), kj::arrayPtr(HELLO_AGAIN)}) {
    conn->write(frame).wait(f.ws);
    KJ_EXPECT(readBytes(f.ws, *conn, frame.size()) == kj::heapString(frame.asChars()));
  }
}

KJ_TEST("client and server round-trip messages over permessage-deflate") {
  Fixture f;
  auto addr = f.address();
  TestEntropySource entropy;
  auto client = kj::newHttpClient(f.timer, *f.table, *addr,
      {.entropySource = entropy,
        .webSocketCompressionMode = kj::HttpClientSettings::MANUAL_COMPRESSION});
  kj::HttpHeaders headers(*f.table);
  headers.setPtr(kj::HttpHeaderId::HOST, "foo");
  headers.setPtr(kj::HttpHeaderId::SEC_WEBSOCKET_EXTENSIONS,
      "permessage-deflate; server_no_context_takeover; client_max_window_bits=9");
  auto response = client->openWebSocket("/ws-deflate", headers).wait(f.ws);
  KJ_ASSERT(response.statusCode == 101);
  KJ_EXPECT(KJ_ASSERT_NONNULL(response.headers->get(kj::HttpHeaderId::SEC_WEBSOCKET_EXTENSIONS)) ==
      "permessage-deflate; server_no_context_takeover; client_max_window_bits=9");
  auto& ws = *response.webSocketOrBody.get<kj::Own<kj::WebSocket>>();

  auto parts = kj::heapArray<kj::StringPtr>(10000);
  for (auto& part: parts) part = "compress me "_kj;
  auto text = kj::strArray(parts, "");
  auto binary = kj::heapArray<kj::byte>(50000);
  uint32_t state = 7;
  for (auto& b: binary) {
    state = state * 1664525 + 1013904223;
    b = state >> 24;
  }
  for (auto round: kj::range(0, 3)) {
    ws.send(text).wait(f.ws);
    KJ_EXPECT(ws.receive().wait(f.ws).get<kj::String>() == text, round);
    ws.send(binary).wait(f.ws);
    KJ_EXPECT(ws.receive().wait(f.ws).get<kj::Array<kj::byte>>() == binary, round);
  }
}

KJ_TEST("text messages pass through as bytes, unvalidated") {
  Fixture f;
  auto conn = f.connect();
  KJ_EXPECT(upgrade(f, *conn, "/ws").startsWith("HTTP/1.1 101 "));
  conn->write(bytes({0x81, 0x82, 0, 0, 0, 0, 0xff, 0xfe}).asBytes()).wait(f.ws);
  KJ_EXPECT(readBytes(f.ws, *conn, 4) == bytes({0x81, 0x02, 0xff, 0xfe}));
}

KJ_TEST("the application answers a Close; the WebSocket does not echo it") {
  Fixture f;
  auto conn = f.connect();
  KJ_EXPECT(upgrade(f, *conn, "/ws-app-close").startsWith("HTTP/1.1 101 "));
  conn->write(bytes({0x88, 0x82, 0, 0, 0, 0, 0x03, 0xe8}).asBytes()).wait(f.ws);
  KJ_EXPECT(readBytes(f.ws, *conn, 7) == bytes({0x88, 0x05, 0x0f, 0xa0, 'a', 'p', 'p'}));
}

KJ_TEST("an oversized frame is refused from its header with Close 1009") {
  Fixture f;
  auto conn = f.connect();
  KJ_EXPECT(upgrade(f, *conn, "/ws-app-close").startsWith("HTTP/1.1 101 "));
  // A 1 GiB binary frame's header, and none of its payload.
  conn->write(bytes({0x82, 0xff, 0, 0, 0, 0, 0x40, 0, 0, 0, 0, 0, 0, 0}).asBytes()).wait(f.ws);
  auto close = readBytes(f.ws, *conn, 4);
  KJ_EXPECT(close[0] == '\x88');
  KJ_EXPECT(close.slice(2) == bytes({0x03, 0xf1}));
}

// The disconnect tests run over a socket pair: closing one end disconnects the other in both
// directions, which is what whenWriteDisconnected() observes (a TCP FIN only ends the reads).

KJ_TEST("a peer disconnecting cancels a hanging WebSocket or tunnel") {
  for (auto head: {"GET /ws-hang HTTP/1.1\r\nHost: foo\r\nUpgrade: websocket\r\n"
                   "Connection: Upgrade\r\nSec-WebSocket-Key: AAAAAAAAAAAAAAAAAAAAAA==\r\n"
                   "Sec-WebSocket-Version: 13\r\n\r\n"_kj,
         "CONNECT hang.example:1 HTTP/1.1\r\nHost: hang.example:1\r\n\r\n"_kj}) {
    Fixture f;
    auto pipe = f.io.provider->newTwoWayPipe();
    auto served = f.server.listenHttp(kj::mv(pipe.ends[0])).eagerlyEvaluate(nullptr);
    pipe.ends[1]->write(head.asBytes()).wait(f.ws);
    KJ_EXPECT(readHead(*pipe.ends[1]).wait(f.ws).startsWith("HTTP/1.1 "));
    pipe.ends[1] = nullptr;
    f.service.cancelled.promise.wait(f.ws);
  }
}

KJ_TEST("a client's WebSocket observes the server disconnecting") {
  Fixture f;
  auto pipe = f.io.provider->newTwoWayPipe();
  auto disconnect = kj::newPromiseAndFulfiller<void>();
  auto serving = [](kj::Own<kj::AsyncIoStream> conn,
                     kj::Promise<void> disconnect) -> kj::Promise<void> {
    co_await readHead(*conn);
    co_await conn->write(
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: qxuoi6TdLAVblW3X0cuvPg6Ld2A=\r\n\r\n"_kj.asBytes());
    co_await disconnect;
  }(kj::mv(pipe.ends[1]), kj::mv(disconnect.promise)).eagerlyEvaluate(nullptr);
  TestEntropySource entropy;
  auto client = kj::newHttpClient(*f.table, *pipe.ends[0], {.entropySource = entropy});
  kj::HttpHeaders headers(*f.table);
  headers.setPtr(kj::HttpHeaderId::HOST, "foo");
  auto response = client->openWebSocket("/ws", headers).wait(f.ws);
  KJ_ASSERT(response.statusCode == 101);
  // Watch before disconnecting: the disconnect is an edge, not a state kj can query later.
  auto aborted = response.webSocketOrBody.get<kj::Own<kj::WebSocket>>()->whenAborted();
  disconnect.fulfiller->fulfill();
  serving.wait(f.ws);
  aborted.wait(f.ws);
}

KJ_TEST("a refused WebSocket carries its status text") {
  Fixture f;
  auto addr = f.address();
  TestEntropySource entropy;
  auto client = kj::newHttpClient(f.timer, *f.table, *addr, {.entropySource = entropy});
  kj::HttpHeaders headers(*f.table);
  headers.setPtr(kj::HttpHeaderId::HOST, "foo");
  auto response = client->openWebSocket("/ws-refuse", headers).wait(f.ws);
  KJ_EXPECT(response.statusCode == 403);
  KJ_EXPECT(response.statusText == "Nope");
}

KJ_TEST("the client refuses a handshake with the wrong Sec-WebSocket-Accept") {
  Fixture f;
  auto listener = f.io.provider->getNetwork().parseAddress("127.0.0.1", 0).wait(f.ws)->listen();
  auto serving = [](kj::ConnectionReceiver& listener) -> kj::Promise<void> {
    auto conn = co_await listener.accept();
    co_await readHead(*conn);
    co_await conn->write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                         "Connection: Upgrade\r\nSec-WebSocket-Accept: wrong\r\n\r\n"_kj.asBytes());
    co_await conn->readAllBytes();
  }(*listener).eagerlyEvaluate(nullptr);
  auto addr = f.io.provider->getNetwork().parseAddress("127.0.0.1", listener->getPort()).wait(f.ws);
  TestEntropySource entropy;
  auto client = kj::newHttpClient(f.timer, *f.table, *addr, {.entropySource = entropy});
  kj::HttpHeaders headers(*f.table);
  headers.setPtr(kj::HttpHeaderId::HOST, "foo");
  KJ_EXPECT_THROW_MESSAGE(
      "incorrect Sec-WebSocket-Accept header", client->openWebSocket("/ws", headers).wait(f.ws));
}

KJ_TEST("HEAD and 204 responses carry no body, and the connection stays in sync") {
  Fixture f;
  auto conn = f.connect();
  conn->write("HEAD /hello HTTP/1.1\r\nHost: foo\r\n\r\n"
              "GET /no-content HTTP/1.1\r\nHost: foo\r\n\r\n"
              "GET /hello HTTP/1.1\r\nHost: foo\r\n\r\n"_kj.asBytes())
      .wait(f.ws);
  auto expected = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n"
                  "HTTP/1.1 204 No Content\r\n\r\n"
                  "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"_kj;
  KJ_EXPECT(readBytes(f.ws, *conn, expected.size()) == expected);
}

KJ_TEST("CONNECT tunnels bytes both ways") {
  Fixture f;
  auto conn = f.connect();
  auto client = kj::newHttpClient(*f.table, *conn);
  auto request = client->connect("tunnel.example:1", kj::HttpHeaders(*f.table), {});
  KJ_EXPECT(request.status.wait(f.ws).statusCode == 200);
  request.connection->write("ping"_kj.asBytes()).wait(f.ws);
  KJ_EXPECT(readBytes(f.ws, *request.connection, 4) == "ping");
}

KJ_TEST("a failing service gets a 500 and the connection closes") {
  Fixture f;
  auto conn = f.connect();
  conn->write("GET /throw HTTP/1.1\r\nHost: foo\r\n\r\n"_kj.asBytes()).wait(f.ws);
  auto response = conn->readAllText().wait(f.ws);
  KJ_EXPECT(
      response.startsWith("HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n"), response);
}

KJ_TEST("drain lets a partially received request finish, then closes") {
  Fixture f;
  auto conn = f.connect();
  auto request = "GET /length HTTP/1.1\r\nHost: foo\r\n\r\n"_kj;
  auto ok = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n"_kj;
  conn->write(request.asBytes()).wait(f.ws);
  KJ_EXPECT(readBytes(f.ws, *conn, ok.size()) == ok);

  conn->write(request.asBytes().first(3)).wait(f.ws);
  f.io.provider->getTimer().afterDelay(10 * kj::MILLISECONDS).wait(f.ws);
  auto drained = f.server.drain().eagerlyEvaluate(nullptr);
  conn->write(request.asBytes().slice(3)).wait(f.ws);
  auto closing = "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 0\r\n\r\n"_kj;
  KJ_EXPECT(readBytes(f.ws, *conn, closing.size()) == closing);
  KJ_EXPECT(isEof(f.ws, *conn));
}

// =======================================================================================
// Server and client details.

// Reads a chunked body to its terminating chunk and returns the payload.
kj::String readChunked(kj::WaitScope& ws, kj::AsyncInputStream& stream) {
  kj::Vector<char> payload;
  for (;;) {
    kj::Vector<char> line;
    while (!line.asPtr().endsWith("\r\n"_kjc)) {
      char c;
      KJ_REQUIRE(stream.tryRead(&c, 1, 1).wait(ws) == 1, "EOF inside a chunk size");
      line.add(c);
    }
    auto size = strtoul(kj::heapString(line.asPtr().first(line.size() - 2)).cStr(), nullptr, 16);
    auto chunk = readBytes(ws, stream, size + 2);
    KJ_REQUIRE(chunk.endsWith("\r\n"));
    if (size == 0) return kj::heapString(payload.asPtr());
    payload.addAll(chunk.first(size));
  }
}

KJ_TEST("server: a request body of unknown length is streamed back with an unknown length") {
  Fixture f;
  auto conn = f.connect();
  conn->write("POST /stream HTTP/1.1\r\nHost: foo\r\nTransfer-Encoding: chunked\r\n\r\n"
              "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n"_kj.asBytes())
      .wait(f.ws);
  KJ_EXPECT(readHead(*conn).wait(f.ws) == "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
  KJ_EXPECT(readChunked(f.ws, *conn) == "hello world");
}

KJ_TEST("server: Content-Length and chunked request framing reach the service") {
  Fixture f;
  auto conn = f.connect();
  conn->write("POST /framing HTTP/1.1\r\nHost: foo\r\nContent-Length: 5\r\n\r\nhello"
              "POST /framing HTTP/1.1\r\nHost: foo\r\nTransfer-Encoding: chunked\r\n\r\n"
              "3\r\nabc\r\n0\r\n\r\n"_kj.asBytes())
      .wait(f.ws);
  auto first = "content-length=5 transfer-encoding=none size=5"_kj;
  auto second = "content-length=none transfer-encoding=chunked size=3"_kj;
  KJ_EXPECT(readHead(*conn).wait(f.ws) ==
      kj::str("HTTP/1.1 200 OK\r\nContent-Length: ", first.size(), "\r\n\r\n"));
  KJ_EXPECT(readBytes(f.ws, *conn, first.size()) == first);
  KJ_EXPECT(readHead(*conn).wait(f.ws) ==
      kj::str("HTTP/1.1 200 OK\r\nContent-Length: ", second.size(), "\r\n\r\n"));
  KJ_EXPECT(readBytes(f.ws, *conn, second.size()) == second);
}

KJ_TEST("server: multi-valued and non-UTF-8 headers pass through both directions") {
  Fixture f;
  auto conn = f.connect();
  conn->write("GET /headers HTTP/1.1\r\nHost: foo\r\nX-Multi: a\r\nX-Bytes: \xff\xfe\r\n"
              "X-Multi: b\r\n\r\n"_kj.asBytes())
      .wait(f.ws);
  auto head = readHead(*conn).wait(f.ws);
  KJ_EXPECT(head.startsWith("HTTP/1.1 200 OK\r\n"), head);
  KJ_EXPECT(head.contains(": a\r\n") && head.contains(": b\r\n"), head);
  KJ_EXPECT(head.contains(": \xff\xfe\r\n"), head);
}

KJ_TEST("server: a large response is written only as fast as the client reads") {
  Fixture f;
  auto conn = f.connect();
  conn->write("GET /large HTTP/1.1\r\nHost: foo\r\n\r\n"_kj.asBytes()).wait(f.ws);
  KJ_EXPECT(readHead(*conn).wait(f.ws).startsWith("HTTP/1.1 200 OK\r\n"));
  size_t total = 0;
  auto buffer = kj::heapArray<kj::byte>(1 << 16);
  while (total < (8 << 20)) {
    total += conn->tryRead(buffer.begin(), 1, buffer.size()).wait(f.ws);
  }
  KJ_EXPECT(total == (8 << 20));
}

KJ_TEST("server: a large request body is read in full") {
  Fixture f;
  auto addr = f.address();
  auto client = kj::newHttpClient(f.timer, *f.table, *addr, {});
  kj::HttpHeaders headers(*f.table);
  headers.setPtr(kj::HttpHeaderId::HOST, "foo");
  auto request = client->request(kj::HttpMethod::POST, "/count", headers, uint64_t(8 << 20));
  auto chunk = kj::heapArray<kj::byte>(64 << 10);
  chunk.asPtr().fill('y');
  for (auto i: kj::range(0, 128)) {
    (void)i;
    request.body->write(chunk).wait(f.ws);
  }
  request.body = nullptr;
  auto response = request.response.wait(f.ws);
  KJ_EXPECT(response.body->readAllText().wait(f.ws) == kj::str(8 << 20));
}

KJ_TEST("server: many requests are in flight at once, on many connections") {
  Fixture f;
  auto addr = f.address();
  auto client = kj::newHttpClient(f.timer, *f.table, *addr, {});
  kj::HttpHeaders headers(*f.table);
  headers.setPtr(kj::HttpHeaderId::HOST, "foo");
  kj::Vector<kj::Promise<kj::HttpClient::Response>> responses;
  for (auto i: kj::range(0, 32)) {
    (void)i;
    responses.add(client->request(kj::HttpMethod::GET, "/gate", headers, uint64_t(0)).response);
  }
  auto all = kj::joinPromises(responses.releaseAsArray()).eagerlyEvaluate(nullptr);
  while (f.service.inFlight < 32) {
    f.timer.afterDelay(1 * kj::MILLISECONDS).wait(f.ws);
  }
  f.service.gateOpen.fulfiller->fulfill();
  for (auto& response: all.wait(f.ws)) KJ_EXPECT(response.statusCode == 200);
}

KJ_TEST("server: exception types map to kj's error responses") {
  {
    Fixture f;
    auto conn = f.connect();
    conn->write("GET /unimplemented HTTP/1.1\r\nHost: foo\r\n\r\n"_kj.asBytes()).wait(f.ws);
    auto response = conn->readAllText().wait(f.ws);
    KJ_EXPECT(response.startsWith("HTTP/1.1 501 Not Implemented\r\n"), response);
  }
  {
    Fixture f;
    auto conn = f.connect();
    conn->write("GET /disconnected HTTP/1.1\r\nHost: foo\r\n\r\n"_kj.asBytes()).wait(f.ws);
    KJ_EXPECT(conn->readAllText().wait(f.ws) == "");
  }
  {
    Fixture f;
    auto conn = f.connect();
    conn->write("GET /no-response HTTP/1.1\r\nHost: foo\r\n\r\n"_kj.asBytes()).wait(f.ws);
    auto response = conn->readAllText().wait(f.ws);
    KJ_EXPECT(response.startsWith("HTTP/1.1 500 Internal Server Error\r\n"), response);
    KJ_EXPECT(response.endsWith("ERROR: The HttpService did not generate a response."), response);
  }
  {
    Fixture f;
    auto conn = f.connect();
    conn->write("GET /accept-plain HTTP/1.1\r\nHost: foo\r\n\r\n"_kj.asBytes()).wait(f.ws);
    auto response = conn->readAllText().wait(f.ws);
    KJ_EXPECT(response.startsWith("HTTP/1.1 500 Internal Server Error\r\n"), response);
  }
}

KJ_TEST("server: an exception mid-body cuts the response short") {
  Fixture f;
  auto conn = f.connect();
  conn->write("GET /fail-mid-body HTTP/1.1\r\nHost: foo\r\n\r\n"_kj.asBytes()).wait(f.ws);
  kj::String response;
  KJ_EXPECT_LOG(ERROR, "too late to report error to client");
  response = conn->readAllText().wait(f.ws);
  KJ_EXPECT(response == "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nhello", response);
}

KJ_TEST("server: a response to HEAD keeps the application's Content-Length") {
  Fixture f;
  auto conn = f.connect();
  conn->write("HEAD /head-length HTTP/1.1\r\nHost: foo\r\n\r\n"
              "GET /hello HTTP/1.1\r\nHost: foo\r\n\r\n"_kj.asBytes())
      .wait(f.ws);
  auto expected = "HTTP/1.1 200 OK\r\nContent-Length: 123\r\n\r\n"
                  "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"_kj;
  KJ_EXPECT(readBytes(f.ws, *conn, expected.size()) == expected);
}

KJ_TEST("server: a write beyond the declared length fails, and nothing of it is sent") {
  Fixture f;
  auto conn = f.connect();
  conn->write("GET /overwrite HTTP/1.1\r\nHost: foo\r\n\r\n"_kj.asBytes()).wait(f.ws);
  auto expected = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"_kj;
  KJ_EXPECT(readBytes(f.ws, *conn, expected.size()) == expected);
  KJ_EXPECT(KJ_ASSERT_NONNULL(f.service.overwriteError).contains("overwrote Content-Length"));
}

KJ_TEST("server: 304 carries no body, and the connection stays in sync") {
  Fixture f;
  auto conn = f.connect();
  conn->write("GET /not-modified HTTP/1.1\r\nHost: foo\r\n\r\n"
              "GET /hello HTTP/1.1\r\nHost: foo\r\n\r\n"_kj.asBytes())
      .wait(f.ws);
  auto expected = "HTTP/1.1 304 Not Modified\r\n\r\n"
                  "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"_kj;
  KJ_EXPECT(readBytes(f.ws, *conn, expected.size()) == expected);
}

KJ_TEST("server: a peer disconnecting cancels a hanging request, before or mid-response") {
  for (auto path: {"/hang"_kj, "/hang-mid-body"_kj}) {
    Fixture f;
    auto pipe = f.io.provider->newTwoWayPipe();
    auto served = f.server.listenHttp(kj::mv(pipe.ends[0])).eagerlyEvaluate(nullptr);
    pipe.ends[1]
        ->write(kj::str("GET ", path, " HTTP/1.1\r\nHost: foo\r\n\r\n").asBytes())
        .wait(f.ws);
    while (f.service.requestCount == 0) {
      f.timer.afterDelay(1 * kj::MILLISECONDS).wait(f.ws);
    }
    pipe.ends[1] = nullptr;
    f.service.cancelled.promise.wait(f.ws);
  }
}

KJ_TEST("server: draining stops the listener") {
  Fixture f;
  f.server.drain().wait(f.ws);
  f.listening.wait(f.ws);
}

KJ_TEST("server: a client that shuts down its side once its request is sent gets the response") {
  Fixture f;
  auto conn = f.connect();
  conn->write("GET /hello HTTP/1.1\r\nHost: foo\r\n\r\n"_kj.asBytes()).wait(f.ws);
  conn->shutdownWrite();
  auto response = conn->readAllText().wait(f.ws);
  KJ_EXPECT(response.startsWith("HTTP/1.1 200 OK\r\n") && response.endsWith("hello"), response);
}

KJ_TEST("server: drain with no activity resolves promptly") {
  Fixture f;
  auto conn = f.connect();
  conn->write("GET /length HTTP/1.1\r\nHost: foo\r\n\r\n"_kj.asBytes()).wait(f.ws);
  KJ_EXPECT(readHead(*conn).wait(f.ws).startsWith("HTTP/1.1 200 OK\r\n"));
  f.server.drain()
      .exclusiveJoin(f.timer.afterDelay(1 * kj::SECONDS).then([]() {
    KJ_FAIL_EXPECT("drain did not resolve within a second");
  })).wait(f.ws);
  KJ_EXPECT(isEof(f.ws, *conn));
}

KJ_TEST("server: Sec-WebSocket-Extensions reaches the service verbatim") {
  Fixture f;
  auto conn = f.connect();
  conn->write("GET /ws-extensions HTTP/1.1\r\nHost: foo\r\nUpgrade: websocket\r\n"
              "Connection: Upgrade\r\nSec-WebSocket-Key: AAAAAAAAAAAAAAAAAAAAAA==\r\n"
              "Sec-WebSocket-Version: 13\r\n"
              "Sec-WebSocket-Extensions: permessage-deflate; client_max_window_bits, x-unknown\r\n"
              "\r\n"_kj.asBytes())
      .wait(f.ws);
  KJ_EXPECT(readHead(*conn).wait(f.ws).startsWith("HTTP/1.1 101 "));
  auto offer = "permessage-deflate; client_max_window_bits, x-unknown"_kj;
  KJ_EXPECT(readBytes(f.ws, *conn, 2 + offer.size()) ==
      kj::str(bytes({0x81, kj::byte(offer.size())}), offer));
}

KJ_TEST("server: an oversized message closes with 1009 and fails the service's receive") {
  Fixture f;
  auto conn = f.connect();
  KJ_EXPECT(upgrade(f, *conn, "/ws-small").startsWith("HTTP/1.1 101 "));
  auto frame = kj::heapArray<kj::byte>(6 + 32);
  frame.asPtr().fill('z');
  frame[0] = 0x82;
  frame[1] = 0x80 | 32;
  for (auto i: kj::range(2, 6)) frame[i] = 0;
  conn->write(frame).wait(f.ws);
  auto close = readBytes(f.ws, *conn, 4);
  KJ_EXPECT(close[0] == '\x88');
  KJ_EXPECT(close.slice(2) == bytes({0x03, 0xf1}));
  conn = nullptr;
  f.server.drain().wait(f.ws);
  KJ_EXPECT(f.service.receiveFailed);
}

KJ_TEST("server: an oversized continuation is refused from its header") {
  Fixture f;
  auto conn = f.connect();
  KJ_EXPECT(upgrade(f, *conn, "/ws-small").startsWith("HTTP/1.1 101 "));
  // An 8-byte first fragment, then a continuation header declaring 16 more (the limit is 16), and
  // none of its payload.
  conn->write(bytes({0x02, 0x88, 0, 0, 0, 0, 'z', 'z', 'z', 'z', 'z', 'z', 'z', 'z', 0x80, 0x90, 0,
                      0, 0, 0})
                  .asBytes())
      .wait(f.ws);
  auto close = readBytes(f.ws, *conn, 4);
  KJ_EXPECT(close[0] == '\x88');
  KJ_EXPECT(close.slice(2) == bytes({0x03, 0xf1}));
}

KJ_TEST("server: fragments reassemble, with a ping answered in between") {
  Fixture f;
  auto conn = f.connect();
  KJ_EXPECT(upgrade(f, *conn, "/ws").startsWith("HTTP/1.1 101 "));
  conn->write(bytes({0x01, 0x82, 0, 0, 0, 0, 'h', 'e', 0x89, 0x81, 0, 0, 0, 0, 'p', 0x80, 0x81, 0,
                      0, 0, 0, 'y'})
                  .asBytes())
      .wait(f.ws);
  KJ_EXPECT(readBytes(f.ws, *conn, 8) == bytes({0x8a, 0x01, 'p', 0x81, 0x03, 'h', 'e', 'y'}));
}

KJ_TEST("server: a bad WebSocket handshake gets kj's 400") {
  Fixture f;
  auto conn = f.connect();
  conn->write("GET /ws HTTP/1.1\r\nHost: foo\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
              "Sec-WebSocket-Version: 13\r\n\r\n"_kj.asBytes())
      .wait(f.ws);
  auto response = conn->readAllText().wait(f.ws);
  KJ_EXPECT(response.startsWith("HTTP/1.1 400 Bad Request\r\n"), response);
  KJ_EXPECT(response.endsWith("Missing Sec-WebSocket-Key"), response);
}

KJ_TEST("server: a handshake with an unsupported version or method is refused") {
  for (auto head:
      {"GET /ws HTTP/1.1\r\nHost: foo\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
       "Sec-WebSocket-Key: AAAAAAAAAAAAAAAAAAAAAA==\r\nSec-WebSocket-Version: 12\r\n\r\n"_kj,
        "POST /ws HTTP/1.1\r\nHost: foo\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: AAAAAAAAAAAAAAAAAAAAAA==\r\nSec-WebSocket-Version: 13\r\n"
        "Content-Length: 0\r\n\r\n"_kj}) {
    Fixture f;
    auto conn = f.connect();
    conn->write(head.asBytes()).wait(f.ws);
    auto response = conn->readAllText().wait(f.ws);
    // An error status, and no upgrade (kj answers 400 or 500; RFC 6455 4.2.2 suggests 426 for the
    // version).
    KJ_EXPECT(response.startsWith("HTTP/1.1 4") || response.startsWith("HTTP/1.1 5"), response);
  }
}

KJ_TEST("server: CONNECT rejection carries its status and body") {
  Fixture f;
  auto conn = f.connect();
  auto client = kj::newHttpClient(*f.table, *conn);
  auto request = client->connect("reject.example:1", kj::HttpHeaders(*f.table), {});
  auto status = request.status.wait(f.ws);
  KJ_EXPECT(status.statusCode == 403);
  KJ_EXPECT(status.statusText == "Forbidden");
  KJ_EXPECT(KJ_ASSERT_NONNULL(status.errorBody)->readAllText().wait(f.ws) == "nope");
}

KJ_TEST("server: a CONNECT that fails before answering gets kj's error response") {
  Fixture f;
  auto conn = f.connect();
  conn->write("CONNECT throw.example:1 HTTP/1.1\r\nHost: throw.example:1\r\n\r\n"_kj.asBytes())
      .wait(f.ws);
  auto response = conn->readAllText().wait(f.ws);
  KJ_EXPECT(response.startsWith("HTTP/1.1 500 Internal Server Error\r\n"), response);
}

KJ_TEST("server: a CONNECT tunnel ends when the client shuts down its side") {
  Fixture f;
  auto conn = f.connect();
  auto client = kj::newHttpClient(*f.table, *conn);
  auto request = client->connect("tunnel.example:1", kj::HttpHeaders(*f.table), {});
  KJ_EXPECT(request.status.wait(f.ws).statusCode == 200);
  request.connection->write("bye"_kj.asBytes()).wait(f.ws);
  request.connection->shutdownWrite();
  KJ_EXPECT(request.connection->readAllText().wait(f.ws) == "bye");
}

KJ_TEST("client: requests over one stream wait their turn for its connection") {
  Fixture f;
  auto conn = f.connect();
  auto client = kj::newHttpClient(*f.table, *conn);
  kj::HttpHeaders headers(*f.table);
  headers.setPtr(kj::HttpHeaderId::HOST, "foo");
  auto first = client->request(kj::HttpMethod::GET, "/hello", headers, uint64_t(0)).response;
  auto second = client->request(kj::HttpMethod::GET, "/hello", headers, uint64_t(0)).response;
  KJ_EXPECT(first.wait(f.ws).body->readAllText().wait(f.ws) == "hello");
  KJ_EXPECT(second.wait(f.ws).body->readAllText().wait(f.ws) == "hello");
}

KJ_TEST("client: HEAD sees the length, and no body") {
  Fixture f;
  auto addr = f.address();
  auto client = kj::newHttpClient(f.timer, *f.table, *addr, {});
  kj::HttpHeaders headers(*f.table);
  headers.setPtr(kj::HttpHeaderId::HOST, "foo");
  auto response =
      client->request(kj::HttpMethod::HEAD, "/hello", headers, uint64_t(0)).response.wait(f.ws);
  KJ_EXPECT(response.statusCode == 200);
  KJ_EXPECT(KJ_ASSERT_NONNULL(response.headers->get(kj::HttpHeaderId::CONTENT_LENGTH)) == "5");
  KJ_EXPECT(response.body->readAllText().wait(f.ws) == "");
  // The connection is still usable.
  auto again =
      client->request(kj::HttpMethod::GET, "/hello", headers, uint64_t(0)).response.wait(f.ws);
  KJ_EXPECT(again.body->readAllText().wait(f.ws) == "hello");
}

KJ_TEST("client: request framing follows the declared body size") {
  Fixture f;
  auto addr = f.address();
  auto client = kj::newHttpClient(f.timer, *f.table, *addr, {});
  kj::HttpHeaders headers(*f.table);
  headers.setPtr(kj::HttpHeaderId::HOST, "foo");
  auto send = [&](kj::HttpMethod method, kj::Maybe<uint64_t> size, kj::StringPtr body) {
    auto request = client->request(method, "/framing", headers, size);
    if (body.size() > 0) request.body->write(body.asBytes()).wait(f.ws);
    request.body = nullptr;
    return request.response.wait(f.ws).body->readAllText().wait(f.ws);
  };
  KJ_EXPECT(send(kj::HttpMethod::POST, uint64_t(5), "hello") ==
      "content-length=5 transfer-encoding=none size=5");
  KJ_EXPECT(send(kj::HttpMethod::POST, kj::none, "hello") ==
      "content-length=none transfer-encoding=chunked size=5");
  // A GET of unknown length is sent with no body framing at all.
  KJ_EXPECT(send(kj::HttpMethod::GET, kj::none, "") ==
      "content-length=none transfer-encoding=none size=0");
}

KJ_TEST("client: multi-valued and non-UTF-8 headers pass through both directions") {
  Fixture f;
  auto pipe = f.io.provider->newTwoWayPipe();
  auto client = kj::newHttpClient(*f.table, *pipe.ends[0]);
  kj::HttpHeaders headers(*f.table);
  headers.setPtr(kj::HttpHeaderId::HOST, "foo");
  headers.addPtrPtr("X-Multi", "a");
  headers.addPtrPtr("X-Bytes", "\xff\xfe");
  headers.addPtrPtr("X-Multi", "b");
  auto response = client->request(kj::HttpMethod::GET, "/", headers, uint64_t(0)).response;
  auto head = readHead(*pipe.ends[1]).wait(f.ws);
  KJ_EXPECT(head.contains("X-Multi: a\r\n"), head);
  KJ_EXPECT(head.contains("X-Multi: b\r\n"), head);
  KJ_EXPECT(head.contains("X-Bytes: \xff\xfe\r\n"), head);
  pipe.ends[1]
      ->write("HTTP/1.1 200 OK\r\nX-Multi: a\r\nX-Bytes: \xff\xfe\r\nX-Multi: b\r\n"
              "Content-Length: 0\r\n\r\n"_kj.asBytes())
      .wait(f.ws);
  auto result = response.wait(f.ws);
  kj::Vector<kj::String> seen;
  result.headers->forEach([&](kj::StringPtr name, kj::StringPtr value) {
    if (name != "Content-Length") seen.add(kj::str(name, ": ", value));
  });
  auto joined = kj::strArray(seen, "|");
  KJ_EXPECT(joined.contains(": a") && joined.contains(": b"), joined);
  KJ_EXPECT(joined.contains(": \xff\xfe"), joined);
}

KJ_TEST("client: a write beyond the declared length fails") {
  Fixture f;
  auto addr = f.address();
  auto client = kj::newHttpClient(f.timer, *f.table, *addr, {});
  kj::HttpHeaders headers(*f.table);
  headers.setPtr(kj::HttpHeaderId::HOST, "foo");
  auto request = client->request(kj::HttpMethod::POST, "/count", headers, uint64_t(5));
  KJ_EXPECT_THROW_MESSAGE(
      "overwrote Content-Length", request.body->write("0123456789"_kj.asBytes()).wait(f.ws));
}

KJ_TEST("client: large bodies both directions, and a 404 passes through") {
  Fixture f;
  auto addr = f.address();
  auto client = kj::newHttpClient(f.timer, *f.table, *addr, {});
  kj::HttpHeaders headers(*f.table);
  headers.setPtr(kj::HttpHeaderId::HOST, "foo");
  auto large =
      client->request(kj::HttpMethod::GET, "/large", headers, uint64_t(0)).response.wait(f.ws);
  KJ_EXPECT(large.body->readAllBytes().wait(f.ws).size() == (8 << 20));
  auto missing =
      client->request(kj::HttpMethod::GET, "/not-found", headers, uint64_t(0)).response.wait(f.ws);
  KJ_EXPECT(missing.statusCode == 404);
  KJ_EXPECT(missing.statusText == "Not Found");
  KJ_EXPECT(missing.body->readAllText().wait(f.ws) == "missing");
}

class CountingReceiver final: public kj::ConnectionReceiver {
 public:
  explicit CountingReceiver(kj::Own<kj::ConnectionReceiver> inner): inner(kj::mv(inner)) {}
  kj::uint accepted = 0;
  kj::Promise<kj::Own<kj::AsyncIoStream>> accept() override {
    auto stream = co_await inner->accept();
    ++accepted;
    co_return stream;
  }
  kj::uint getPort() override {
    return inner->getPort();
  }

 private:
  kj::Own<kj::ConnectionReceiver> inner;
};

KJ_TEST("client: a connection is reused once a response body has been read") {
  Fixture f;
  CountingReceiver counting(
      f.io.provider->getNetwork().parseAddress("127.0.0.1", 0).wait(f.ws)->listen());
  auto listening = f.server.listenHttp(counting).eagerlyEvaluate(nullptr);
  auto addr = f.io.provider->getNetwork().parseAddress("127.0.0.1", counting.getPort()).wait(f.ws);
  auto client = kj::newHttpClient(f.timer, *f.table, *addr, {});
  kj::HttpHeaders headers(*f.table);
  headers.setPtr(kj::HttpHeaderId::HOST, "foo");
  for (auto i: kj::range(0, 5)) {
    (void)i;
    auto response =
        client->request(kj::HttpMethod::GET, "/hello", headers, uint64_t(0)).response.wait(f.ws);
    KJ_EXPECT(response.body->readAllText().wait(f.ws) == "hello");
  }
  KJ_EXPECT(counting.accepted == 1, counting.accepted);
}

KJ_TEST("client: a refused connection fails with DISCONNECTED") {
  Fixture f;
  auto port =
      f.io.provider->getNetwork().parseAddress("127.0.0.1", 0).wait(f.ws)->listen()->getPort();
  auto addr = f.io.provider->getNetwork().parseAddress("127.0.0.1", port).wait(f.ws);
  auto client = kj::newHttpClient(f.timer, *f.table, *addr, {});
  kj::HttpHeaders headers(*f.table);
  headers.setPtr(kj::HttpHeaderId::HOST, "foo");
  auto result = client->request(kj::HttpMethod::GET, "/", headers, uint64_t(0))
                    .response
                    .then([](auto&&) -> kj::Maybe<kj::Exception> { return kj::none; },
                        [](kj::Exception&& e) -> kj::Maybe<kj::Exception> {
    return kj::mv(e);
  }).wait(f.ws);
  auto& e = KJ_ASSERT_NONNULL(result);
  KJ_EXPECT(e.getType() == kj::Exception::Type::DISCONNECTED, e);
}

KJ_TEST("client: dropping an in-flight request cancels it on the server") {
  Fixture f;
  auto pipe = f.io.provider->newTwoWayPipe();
  auto served = f.server.listenHttp(kj::mv(pipe.ends[0])).eagerlyEvaluate(nullptr);
  {
    auto client = kj::newHttpClient(*f.table, *pipe.ends[1]);
    kj::HttpHeaders headers(*f.table);
    headers.setPtr(kj::HttpHeaderId::HOST, "foo");
    auto response = client->request(kj::HttpMethod::GET, "/hang", headers, uint64_t(0)).response;
    while (f.service.requestCount == 0) {
      f.timer.afterDelay(1 * kj::MILLISECONDS).wait(f.ws);
    }
  }
  pipe.ends[1] = nullptr;
  f.service.cancelled.promise.wait(f.ws);
}

KJ_TEST("client: a server's abort fails the WebSocket's receive") {
  Fixture f;
  auto addr = f.address();
  TestEntropySource entropy;
  auto client = kj::newHttpClient(f.timer, *f.table, *addr, {.entropySource = entropy});
  kj::HttpHeaders headers(*f.table);
  headers.setPtr(kj::HttpHeaderId::HOST, "foo");
  auto response = client->openWebSocket("/ws-abort", headers).wait(f.ws);
  KJ_ASSERT(response.statusCode == 101);
  auto& ws = *response.webSocketOrBody.get<kj::Own<kj::WebSocket>>();
  ws.send("abort now"_kj).wait(f.ws);
  KJ_EXPECT_THROW(DISCONNECTED, ws.receive().wait(f.ws));
}

// Answers one request head on `conn` with `response`, then waits for `done`.
kj::Promise<void> fakeServer(
    kj::AsyncIoStream& conn, kj::StringPtr response, kj::Promise<void> done = kj::NEVER_DONE) {
  co_await readHead(conn);
  co_await conn.write(response.asBytes());
  co_await done;
}

KJ_TEST("client: fragments from a server reassemble, and its ping is answered") {
  Fixture f;
  auto pipe = f.io.provider->newTwoWayPipe();
  auto serving = fakeServer(*pipe.ends[1],
      "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
      "Sec-WebSocket-Accept: qxuoi6TdLAVblW3X0cuvPg6Ld2A=\r\n\r\n"
      "\x01\x02he\x89\x01p\x80\x01y")
                     .eagerlyEvaluate(nullptr);
  TestEntropySource entropy;
  auto client = kj::newHttpClient(*f.table, *pipe.ends[0], {.entropySource = entropy});
  kj::HttpHeaders headers(*f.table);
  headers.setPtr(kj::HttpHeaderId::HOST, "foo");
  auto response = client->openWebSocket("/ws", headers).wait(f.ws);
  KJ_ASSERT(response.statusCode == 101);
  auto& ws = *response.webSocketOrBody.get<kj::Own<kj::WebSocket>>();
  KJ_EXPECT(ws.receive().wait(f.ws).get<kj::String>() == "hey");
  auto pong = readBytes(f.ws, *pipe.ends[1], 7);
  KJ_EXPECT(pong[0] == '\x8a');
  KJ_EXPECT(pong[1] == '\x81');
  KJ_EXPECT((pong[6] ^ pong[2]) == 'p');
}

KJ_TEST("client: a non-101 answer to an upgrade is a regular response with its body") {
  Fixture f;
  auto pipe = f.io.provider->newTwoWayPipe();
  auto serving =
      fakeServer(*pipe.ends[1], "HTTP/1.1 404 Not Found\r\nContent-Length: 4\r\n\r\nnope")
          .eagerlyEvaluate(nullptr);
  TestEntropySource entropy;
  auto client = kj::newHttpClient(*f.table, *pipe.ends[0], {.entropySource = entropy});
  kj::HttpHeaders headers(*f.table);
  headers.setPtr(kj::HttpHeaderId::HOST, "foo");
  auto response = client->openWebSocket("/ws", headers).wait(f.ws);
  KJ_EXPECT(response.statusCode == 404);
  KJ_EXPECT(response.statusText == "Not Found");
  auto& body = *response.webSocketOrBody.get<kj::Own<kj::AsyncInputStream>>();
  KJ_EXPECT(body.readAllText().wait(f.ws) == "nope");
}

KJ_TEST("client: a response cut short fails the body read") {
  Fixture f;
  auto pipe = f.io.provider->newTwoWayPipe();
  auto closeServer = kj::newPromiseAndFulfiller<void>();
  auto serving = fakeServer(*pipe.ends[1], "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nhello",
      kj::mv(closeServer.promise))
                     .eagerlyEvaluate(nullptr);
  auto client = kj::newHttpClient(*f.table, *pipe.ends[0]);
  kj::HttpHeaders headers(*f.table);
  headers.setPtr(kj::HttpHeaderId::HOST, "foo");
  auto response =
      client->request(kj::HttpMethod::GET, "/", headers, uint64_t(0)).response.wait(f.ws);
  KJ_EXPECT(response.statusCode == 200);
  closeServer.fulfiller->fulfill();
  serving.wait(f.ws);
  pipe.ends[1] = nullptr;
  KJ_EXPECT_THROW(DISCONNECTED, response.body->readAllText().wait(f.ws));
}

}  // namespace
}  // namespace workerd
