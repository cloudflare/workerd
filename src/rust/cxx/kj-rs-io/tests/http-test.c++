// kj-http integration ("the acid test"): a real kj::HttpServer serving on a kj-rs-io listener
// and a kj::HttpClient over a kj-rs-io connection, all driven by the tokio event loop. Proves
// that kj-http works unchanged over tokio-backed streams.

#include "kj-rs-io/async-io.h"

#include <kj/async.h>
#include <kj/compat/http.h>
#include <kj/debug.h>
#include <kj/test.h>

#include <cstring>

namespace kj_rs_io_test {
namespace {

using kj_rs_io::setupTokioAsyncIo;

// Echoes the request body back as the response body, streaming (pumpTo), preserving the
// content length when known.
class EchoService final: public kj::HttpService {
 public:
  explicit EchoService(kj::HttpHeaderTable &table): table(table) {}

  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders &headers,
      kj::AsyncInputStream &requestBody,
      Response &response) override {
    kj::HttpHeaders responseHeaders(table);
    auto body = response.send(200, "OK", responseHeaders, requestBody.tryGetLength());
    co_await requestBody.pumpTo(*body);
  }

 private:
  kj::HttpHeaderTable &table;
};

kj::Array<kj::byte> makeBody(size_t size) {
  auto data = kj::heapArray<kj::byte>(size);
  for (size_t i = 0; i < size; i++) {
    data[i] = static_cast<kj::byte>(('A' + i / 8192 + i * 13) & 0xff);
  }
  return data;
}

// Writes `data` in chunks to the request body stream, then closes it.
kj::Promise<void> writeBody(
    kj::Own<kj::AsyncOutputStream> body, kj::ArrayPtr<const kj::byte> data) {
  constexpr size_t CHUNK = 128 * 1024;
  size_t offset = 0;
  while (offset < data.size()) {
    size_t n = kj::min(CHUNK, data.size() - offset);
    co_await body->write(data.slice(offset, offset + n));
    offset += n;
  }
  // Dropping `body` (coroutine frame teardown) finishes the request body.
}

KJ_TEST("bodyless GET works (regression: kj-http never awaits its header-write "
        "queue for bodyless requests, relying on KJ hot-promise write semantics)") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();
  kj::HttpHeaderTable table;
  EchoService service(table);
  kj::HttpServer server(io.getTimer(), table, service);
  auto listener = io.getNetwork().parseAddress("127.0.0.1", 0).wait(ws)->listen();
  auto listenTask = server.listenHttp(*listener).eagerlyEvaluate(nullptr);
  auto addr = io.getNetwork().parseAddress(kj::str("127.0.0.1:", listener->getPort())).wait(ws);
  auto connection = addr->connect().wait(ws);
  auto client = kj::newHttpClient(table, *connection);
  kj::HttpHeaders headers(table);
  auto request = client->request(kj::HttpMethod::GET, "/x", headers, static_cast<uint64_t>(0));
  auto response = request.response.wait(ws);
  KJ_EXPECT(response.statusCode == 200);
  auto body = response.body->readAllBytes().wait(ws);
  KJ_EXPECT(body.size() == 0);
}

KJ_TEST("kj-http round trip with streaming bodies over kj-rs-io streams on the "
        "tokio loop") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  kj::HttpHeaderTable table;
  EchoService service(table);
  kj::HttpServer server(io.getTimer(), table, service);

  // Server side: kj::HttpServer accepting from a kj-rs-io ConnectionReceiver.
  auto listener = io.getNetwork().parseAddress("127.0.0.1", 0).wait(ws)->listen();
  auto listenTask = server.listenHttp(*listener).eagerlyEvaluate(
      [](kj::Exception &&e) { KJ_FAIL_EXPECT("HTTP server failed", e); });

  // Client side: kj::HttpClient over a kj-rs-io connection.
  auto addr = io.getNetwork().parseAddress(kj::str("127.0.0.1:", listener->getPort())).wait(ws);
  auto connection = addr->connect().wait(ws);
  auto client = kj::newHttpClient(table, *connection);

  // Round trip 1: 4 MB POST with a streamed request body, echoed back and read while the
  // request body is still being written (full-duplex streaming through the tokio loop).
  {
    constexpr size_t SIZE = 4 * 1024 * 1024;
    auto data = makeBody(SIZE);

    kj::HttpHeaders headers(table);
    auto request =
        client->request(kj::HttpMethod::POST, "/echo", headers, static_cast<uint64_t>(SIZE));

    auto writeTask = writeBody(kj::mv(request.body), data).eagerlyEvaluate(nullptr);
    auto response = request.response.wait(ws);
    KJ_EXPECT(response.statusCode == 200);
    KJ_EXPECT(KJ_ASSERT_NONNULL(response.body->tryGetLength()) == SIZE);

    auto echoed = response.body->readAllBytes(SIZE + 1).wait(ws);
    KJ_ASSERT(echoed.size() == SIZE);
    KJ_ASSERT(memcmp(echoed.begin(), data.begin(), SIZE) == 0);
    writeTask.wait(ws);
  }

  // Round trip 2 on the same connection (keep-alive): small GET, empty echoed body.
  {
    kj::HttpHeaders headers(table);
    auto request =
        client->request(kj::HttpMethod::GET, "/again", headers, static_cast<uint64_t>(0));
    auto response = request.response.wait(ws);
    KJ_EXPECT(response.statusCode == 200);
    auto body = response.body->readAllBytes().wait(ws);
    KJ_EXPECT(body.size() == 0);
  }

  // Round trip 3: chunked request body (no expected size -> Transfer-Encoding: chunked).
  {
    kj::HttpHeaders headers(table);
    auto request = client->request(kj::HttpMethod::POST, "/chunked", headers);
    auto data = makeBody(64 * 1024);
    auto writeTask = writeBody(kj::mv(request.body), data).eagerlyEvaluate(nullptr);
    auto response = request.response.wait(ws);
    KJ_EXPECT(response.statusCode == 200);
    auto echoed = response.body->readAllBytes().wait(ws);
    KJ_ASSERT(echoed.size() == data.size());
    KJ_ASSERT(memcmp(echoed.begin(), data.begin(), data.size()) == 0);
    writeTask.wait(ws);
  }
}

}  // namespace
}  // namespace kj_rs_io_test
