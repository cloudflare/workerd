// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "server.h"

#include <workerd/jsg/setup.h>
#include <workerd/util/autogate.h>
#include <workerd/util/capnp-mock.h>

#include <kj/async-queue.h>
#include <kj/test.h>

#include <cstdlib>
#include <regex>

namespace workerd::server {
namespace {

#define KJ_FAIL_EXPECT_AT(location, ...) KJ_LOG_AT(ERROR, location, ##__VA_ARGS__);
#define KJ_EXPECT_AT(cond, location, ...)                                                          \
  if (auto _kjCondition = ::kj::_::MAGIC_ASSERT << cond)                                           \
    ;                                                                                              \
  else                                                                                             \
    KJ_FAIL_EXPECT_AT(location, "failed: expected " #cond, _kjCondition, ##__VA_ARGS__)

jsg::V8System v8System;
// This can only be created once per process, so we have to put it at the top level.

const bool verboseLog = ([]() {
  // TODO(beta): Improve uncaught exception reporting so that we dontt have to do this.
  kj::_::Debug::setLogLevel(kj::LogSeverity::INFO);
  return true;
})();

kj::Own<config::Config::Reader> parseConfig(kj::StringPtr text, kj::SourceLocation loc) {
  capnp::MallocMessageBuilder builder;
  auto root = builder.initRoot<config::Config>();
  KJ_IF_SOME(exception, kj::runCatchingExceptions([&]() { TEXT_CODEC.decode(text, root); })) {
    KJ_FAIL_REQUIRE_AT(loc, exception);
  }

  util::Autogate::initAutogate(root.asReader().getAutogates());

  return capnp::clone(root.asReader());
}

// Accept an indented block of text and remove the indentation. From each line of text, this will
// remove a number of spaces up to the indentation of the first line.
//
// This is intended to allow multi-line raw text to be specified conveniently using C++11
// `R"(blah)"` literal syntax, without the need to mess up indentation relative to the
// surrounding code.
kj::String operator"" _blockquote(const char* str, size_t n) {
  kj::StringPtr text(str, n);

  // Ignore a leading newline so that `R"(` can be placed on the line before the initial indent.
  if (text.startsWith("\n")) {
    text = text.slice(1);
  }

  // Count indent size.
  size_t indent = 0;
  while (text.startsWith(" ")) {
    text = text.slice(1);
    ++indent;
  }

  // Process lines.
  kj::Vector<char> result;
  while (text != nullptr) {
    // Add data from this line.
    auto nl = text.findFirst('\n').orDefault(text.size() - 1) + 1;
    result.addAll(text.first(nl));
    text = text.slice(nl);

    // Skip indent of next line, up to the expected indent size.
    size_t seenIndent = 0;
    while (seenIndent < indent && text.startsWith(" ")) {
      text = text.slice(1);
      ++seenIndent;
    }
  }

  result.add('\0');
  return kj::String(result.releaseAsArray());
}

class TestStream {
 public:
  TestStream(kj::WaitScope& ws, kj::Own<kj::AsyncIoStream> stream)
      : ws(ws),
        stream(kj::mv(stream)) {}

  void send(kj::StringPtr data, kj::SourceLocation loc = {}) {
    stream->write(data.asBytes()).wait(ws);
  }
  void recv(kj::StringPtr expected, kj::SourceLocation loc = {}) {
    auto actual = readAllAvailable();
    if (actual == nullptr) {
      KJ_FAIL_EXPECT_AT(loc, "message never received");
    } else {
      KJ_EXPECT_AT(actual == expected, loc);
    }
  }
  void recvRegex(kj::StringPtr matcher, kj::SourceLocation loc = {}) {
    auto actual = readAllAvailable();
    if (actual == nullptr) {
      KJ_FAIL_EXPECT_AT(loc, "message never received");
    } else {
      std::regex target(matcher.cStr());
      KJ_EXPECT(std::regex_match(actual.cStr(), target), actual, matcher, loc);
    }
  }

  void recvWebSocket(kj::StringPtr expected, kj::SourceLocation loc = {}) {
    auto actual = readWebSocketMessage();
    KJ_EXPECT_AT(actual == expected, loc);
  }

  void recvWebSocketRegex(kj::StringPtr matcher, kj::SourceLocation loc = {}) {
    auto actual = readWebSocketMessage();
    std::regex target(matcher.cStr());
    KJ_EXPECT(std::regex_match(actual.cStr(), target), actual, matcher, loc);
  }

  void recvWebSocketClose(int expectedCode) {
    auto actual = readWebSocketMessage();
    KJ_EXPECT(actual.size() >= 2);
    int gotCode = (static_cast<uint8_t>(actual[0]) << 8) + static_cast<uint8_t>(actual[1]);
    KJ_EXPECT(gotCode == expectedCode);
  }

  void sendHttpGet(kj::StringPtr path, kj::SourceLocation loc = {}) {
    send(kj::str("GET ", path,
             " HTTP/1.1\n"
             "Host: foo\n"
             "\n"),
        loc);
  }

  void recvHttp200(kj::StringPtr expectedResponse, kj::SourceLocation loc = {}) {
    recv(kj::str("HTTP/1.1 200 OK\n"
                 "Content-Length: ",
             expectedResponse.size(),
             "\n"
             "Content-Type: text/plain;charset=UTF-8\n"
             "\n",
             expectedResponse),
        loc);
  }

  void httpGet200(kj::StringPtr path, kj::StringPtr expectedResponse, kj::SourceLocation loc = {}) {
    sendHttpGet(path, loc);
    recvHttp200(expectedResponse, loc);
  }

  // Return true if the stream is at EOF.
  bool isEof() {
    if (premature != kj::none) {
      // We still have unread data so we're definitely not at EOF.
      return false;
    }

    char c;
    auto promise = stream->tryRead(&c, 1, 1);
    if (!promise.poll(ws)) {
      // Read didn't complete immediately. We have no data available, but we're not at EOF.
      return false;
    }

    size_t n = promise.wait(ws);
    if (n == 0) {
      return true;
    } else {
      // Oops, the stream had data available and we accidentally read a byte of it. Store that off
      // to the side.
      KJ_ASSERT(n == 1);
      premature = c;
      return false;
    }
  }

  void upgradeToWebSocket() {
    send(R"(
      GET / HTTP/1.1
      Host: foo
      Upgrade: websocket
      Sec-WebSocket-Key: AAAAAAAAAAAAAAAAAAAAAA==
      Sec-WebSocket-Version: 13

    )"_blockquote,
        {});

    recv(R"(
      HTTP/1.1 101 Switching Protocols
      Connection: Upgrade
      Upgrade: websocket
      Sec-WebSocket-Accept: ICX+Yqv66kxgM0FcWaLWlFLwTAI=

    )"_blockquote,
        {});
  }

 private:
  kj::WaitScope& ws;
  kj::Own<kj::AsyncIoStream> stream;

  // isEof() may prematurely read a character. Keep it off to the side for the next actual read.
  kj::Maybe<char> premature;

  kj::String readAllAvailable() {
    kj::Vector<char> buffer(256);
    KJ_IF_SOME(p, premature) {
      buffer.add(p);
    }

    // Continuously try to read until there's nothing to read (or we've gone way past the size
    // expected).
    for (;;) {
      size_t pos = buffer.size();
      buffer.resize(kj::max(buffer.size() + 256, buffer.capacity()));

      auto promise = stream->tryRead(buffer.begin() + pos, 1, buffer.size() - pos);
      if (!promise.poll(ws)) {
        // A tryRead() of 1 byte didn't resolve, there must be no data to read.
        buffer.resize(pos);
        break;
      }
      size_t n = promise.wait(ws);
      if (n == 0) {
        buffer.resize(pos);
        break;
      }

      // Strip out `\r`s for convenience. We do this in-place...
      for (size_t i: kj::range(pos, pos + n)) {
        if (buffer[i] != '\r') {
          buffer[pos++] = buffer[i];
        }
      }
      buffer.resize(pos);
    };

    buffer.add('\0');
    return kj::String(buffer.releaseAsArray());
  }

  kj::String readWebSocketMessage(size_t maxMessageSize = 1 << 24) {
    // Reads a single, non-fragmented WebSocket message. Returns just the payload.
    kj::Vector<uint8_t> header(256);
    kj::Vector<uint8_t> mask(4);

    KJ_IF_SOME(p, premature) {
      header.add(p);
      premature = kj::Maybe<char>();
    }

    tryRead(header, 2 - header.size(), "reading first two bytes of header");
    bool masked = header[1] & 0x80;
    size_t sevenBitPayloadLength = header[1] & 0x7f;
    size_t realPayloadLength = sevenBitPayloadLength;

    if (sevenBitPayloadLength == 126) {
      tryRead(header, 2, "reading 16-bit payload length");
      realPayloadLength = (static_cast<size_t>(header[2]) << 8) + static_cast<size_t>(header[3]);
    } else if (sevenBitPayloadLength == 127) {
      tryRead(header, 8, "reading 64-bit payload length");
      realPayloadLength = (static_cast<size_t>(header[2]) << 56) +
          (static_cast<size_t>(header[3]) << 48) + (static_cast<size_t>(header[4]) << 40) +
          (static_cast<size_t>(header[5]) << 32) + (static_cast<size_t>(header[6]) << 24) +
          (static_cast<size_t>(header[7]) << 16) + (static_cast<size_t>(header[8]) << 8) +
          (static_cast<size_t>(header[9]));

      KJ_REQUIRE(realPayloadLength <= maxMessageSize,
          kj::str("Payload size too big (", realPayloadLength, " > ", maxMessageSize, ")"));
    }

    if (masked) {
      tryRead(mask, 4, "reading mask key");
      // Currently we assume the mask is always 0, so its application is a no-op, hence we don't
      // bother.
    }
    kj::Vector<char> payload(realPayloadLength + 1);

    tryRead(payload, realPayloadLength, "reading payload");
    payload.add('\0');
    return kj::String(payload.releaseAsArray());
  }

  template <typename T>
  void tryRead(kj::Vector<T>& buffer, size_t bytesToRead, kj::StringPtr what) {
    static_assert(sizeof(T) == 1, "not byte-sized");

    size_t pos = buffer.size();
    size_t bytesRead = 0;
    buffer.resize(buffer.size() + bytesToRead);
    while (bytesRead < bytesToRead) {
      auto promise = stream->tryRead(buffer.begin() + pos, 1, buffer.size() - pos);
      KJ_REQUIRE(promise.poll(ws), kj::str("No data available while ", what));
      // A tryRead() of 1 byte didn't resolve, there must be no data to read.

      size_t n = promise.wait(ws);
      KJ_REQUIRE(n > 0, kj::str("Not enough data while ", what));
      bytesRead += n;
    }
  }
};

class TestServer final: private kj::Filesystem, private kj::EntropySource, private kj::Clock {
 public:
  TestServer(kj::StringPtr configText, kj::SourceLocation loc = {})
      : ws(loop),
        config(parseConfig(configText, loc)),
        root(kj::newInMemoryDirectory(*this)),
        pwd(kj::Path({"current", "dir"})),
        cwd(root->openSubdir(pwd, kj::WriteMode::CREATE | kj::WriteMode::CREATE_PARENT)),
        timer(kj::origin<kj::TimePoint>()),
        server(*this,
            timer,
            mockNetwork,
            *this,
            Worker::ConsoleMode::INSPECTOR_ONLY,
            [this](kj::String error) {
              if (expectedErrors.startsWith(error) && expectedErrors[error.size()] == '\n') {
                expectedErrors = expectedErrors.slice(error.size() + 1);
              } else {
                KJ_FAIL_EXPECT(error, expectedErrors);
              }
            }),
        fakeDate(kj::UNIX_EPOCH),
        mockNetwork(*this, {}, {}) {}

  ~TestServer() noexcept(false) {
    for (auto& subq: subrequests) {
      subq.value->rejectAll(KJ_EXCEPTION(FAILED, "test ended"));
    }

    if (!unwindDetector.isUnwinding()) {
      // Make sure any errors are reported.
      KJ_IF_SOME(t, runTask) {
        t.poll(ws);
      }
    }
  }

  // Start the server. Call before connect().
  void start(kj::Promise<void> drainWhen = kj::NEVER_DONE) {
    KJ_REQUIRE(runTask == kj::none);
    auto task =
        server.run(v8System, *config, kj::mv(drainWhen)).eagerlyEvaluate([](kj::Exception&& e) {
      KJ_FAIL_EXPECT(e);
    });
    KJ_EXPECT(!task.poll(ws));
    runTask = kj::mv(task);
  }

  // Call instead of `start()` when the config is expected to produce errors. The parameter is
  // the expected list of errors messages, one per line.
  void expectErrors(kj::StringPtr expected) {
    expectedErrors = expected;
    server.run(v8System, *config).poll(ws);
    KJ_EXPECT(expectedErrors == nullptr, "some expected errors weren't seen");
  }

  // Connect to the server on the given address. The string just has to match what is in the
  // config; the actual connection is in-memory with no network involved.
  TestStream connect(kj::StringPtr addr) {
    return TestStream(ws, KJ_REQUIRE_NONNULL(sockets.find(addr), addr)->connect().wait(ws));
  }

  // Try to connect to the address and return whether or not this connection attempt hangs,
  // i.e. a listener exists but connections are not being accepted.
  bool connectHangs(kj::StringPtr addr) {
    return !KJ_REQUIRE_NONNULL(sockets.find(addr), addr)->connect().poll(ws);
  }

  // Expect an incoming connection on the given address and from a network with the given
  // allowed / denied peer list.
  TestStream receiveSubrequest(kj::StringPtr addr,
      kj::ArrayPtr<const kj::StringPtr> allowedPeers = nullptr,
      kj::ArrayPtr<const kj::StringPtr> deniedPeers = nullptr,
      kj::SourceLocation loc = {}) {
    auto expectedFilter = peerFilterToString(allowedPeers, deniedPeers);

    auto promise = getSubrequestQueue(addr).pop();
    KJ_ASSERT_AT(promise.poll(ws), loc, "never received expected subrequest", addr);

    auto info = promise.wait(ws);
    auto actualFilter = info.peerFilter;
    KJ_EXPECT_AT(actualFilter == expectedFilter, loc);

    auto pipe = kj::newTwoWayPipe();
    info.fulfiller->fulfill(kj::mv(pipe.ends[0]));
    return TestStream(ws, kj::mv(pipe.ends[1]));
  }

  TestStream receiveInternetSubrequest(kj::StringPtr addr, kj::SourceLocation loc = {}) {
    return receiveSubrequest(addr, {"public"_kj}, {}, loc);
  }

  // Advance the timer through `seconds` seconds of virtual time.
  void wait(size_t seconds) {
    auto delayPromise = timer.afterDelay(seconds * kj::SECONDS).eagerlyEvaluate(nullptr);
    while (!delayPromise.poll(ws)) {
      // Since this test has no external I/O at all other than time, we know no events could
      // possibly occur until the next timer event. So just advance directly to it and continue.
      timer.advanceTo(KJ_ASSERT_NONNULL(timer.nextEvent()));
    }
    delayPromise.wait(ws);
  }

  kj::EventLoop loop;
  kj::WaitScope ws;

  kj::Own<config::Config::Reader> config;
  kj::Own<const kj::Directory> root;
  kj::Path pwd;
  kj::Own<const kj::Directory> cwd;
  kj::TimerImpl timer;
  Server server;

  kj::Maybe<kj::Promise<void>> runTask;
  kj::StringPtr expectedErrors;

  kj::Date fakeDate;

 private:
  kj::UnwindDetector unwindDetector;

  // ---------------------------------------------------------------------------
  // implements Filesystem

  const kj::Directory& getRoot() const override {
    return *root;
  }
  const kj::Directory& getCurrent() const override {
    return *cwd;
  }
  kj::PathPtr getCurrentPath() const override {
    return pwd;
  }

  // ---------------------------------------------------------------------------
  // implements Network

  // Addresses that the server is listening on.
  kj::HashMap<kj::String, kj::Own<kj::NetworkAddress>> sockets;

  class MockNetwork;

  struct SubrequestInfo {
    kj::Own<kj::PromiseFulfiller<kj::Own<kj::AsyncIoStream>>> fulfiller;
    kj::StringPtr peerFilter;
  };
  using SubrequestQueue = kj::ProducerConsumerQueue<SubrequestInfo>;
  // Expected incoming connections and callbacks that should be used to handle them.
  kj::HashMap<kj::String, kj::Own<SubrequestQueue>> subrequests;

  SubrequestQueue& getSubrequestQueue(kj::StringPtr addr) {
    return *subrequests.findOrCreate(addr, [&]() -> decltype(subrequests)::Entry {
      return {kj::str(addr), kj::heap<SubrequestQueue>()};
    });
  }

  static kj::String peerFilterToString(
      kj::ArrayPtr<const kj::StringPtr> allow, kj::ArrayPtr<const kj::StringPtr> deny) {
    if (allow == nullptr && deny == nullptr) {
      return kj::str("(none)");
    } else {
      return kj::str("allow: [", kj::strArray(allow, ", "),
          "], "
          "deny: [",
          kj::strArray(deny, ", "), "]");
    }
  }

  class MockAddress final: public kj::NetworkAddress {
   public:
    MockAddress(TestServer& test, kj::StringPtr peerFilter, kj::String address)
        : test(test),
          peerFilter(peerFilter),
          address(kj::mv(address)) {}

    kj::Promise<kj::Own<kj::AsyncIoStream>> connect() override {
      KJ_IF_SOME(addr, test.sockets.find(address)) {
        // If someone is listening on this address, connect directly to them.
        return addr->connect();
      }

      auto [promise, fulfiller] = kj::newPromiseAndFulfiller<kj::Own<kj::AsyncIoStream>>();

      test.getSubrequestQueue(address).push({kj::mv(fulfiller), peerFilter});

      return kj::mv(promise);
    }
    kj::Own<kj::ConnectionReceiver> listen() override {
      auto pipe = kj::newCapabilityPipe();
      auto receiver = kj::heap<kj::CapabilityStreamConnectionReceiver>(*pipe.ends[0])
                          .attach(kj::mv(pipe.ends[0]));
      auto sender = kj::heap<kj::CapabilityStreamNetworkAddress>(kj::none, *pipe.ends[1])
                        .attach(kj::mv(pipe.ends[1]));
      test.sockets.insert(kj::str(address), kj::mv(sender));
      return receiver;
    }
    kj::Own<kj::NetworkAddress> clone() override {
      KJ_UNIMPLEMENTED("unused");
    }
    kj::String toString() override {
      KJ_UNIMPLEMENTED("unused");
    }

   private:
    TestServer& test;
    kj::StringPtr peerFilter;
    kj::String address;
  };

  class MockNetwork final: public kj::Network {
   public:
    MockNetwork(TestServer& test,
        kj::ArrayPtr<const kj::StringPtr> allow,
        kj::ArrayPtr<const kj::StringPtr> deny)
        : test(test),
          filter(peerFilterToString(allow, deny)) {}

    kj::Promise<kj::Own<kj::NetworkAddress>> parseAddress(
        kj::StringPtr addr, uint portHint = 0) override {
      return kj::Own<kj::NetworkAddress>(kj::heap<MockAddress>(test, filter, kj::str(addr)));
    }
    kj::Own<kj::NetworkAddress> getSockaddr(const void* sockaddr, uint len) override {
      KJ_UNIMPLEMENTED("unused");
    }
    kj::Own<kj::Network> restrictPeers(
        kj::ArrayPtr<const kj::StringPtr> allow, kj::ArrayPtr<const kj::StringPtr> deny) override {
      KJ_ASSERT(filter == "(none)", "can't nest restrictPeers()");
      return kj::heap<MockNetwork>(test, allow, deny);
    }

   private:
    TestServer& test;
    kj::String filter;
  };

  MockNetwork mockNetwork;

  // ---------------------------------------------------------------------------
  // implements EntropySource

  void generate(kj::ArrayPtr<kj::byte> buffer) override {
    kj::byte random = 4;  // chosen by fair die roll by Randall Munroe in 2007.
                          // guaranteed to be random.
    buffer.fill(random);
  }

  // ---------------------------------------------------------------------------
  // implements Clock

  kj::Date now() const override {
    return fakeDate;
  }
};

// =======================================================================================
// Test Workers

kj::String singleWorker(kj::StringPtr def) {
  return kj::str(R"((
    services = [
      ( name = "hello",
        worker = )"_kj,
      def, R"(
      )
    ],
    sockets = [
      ( name = "main",
        address = "test-addr",
        service = "hello"
      )
    ]
  ))"_kj);
}

KJ_TEST("Server: serve basic Service Worker") {
  TestServer test(singleWorker(R"((
    compatibilityDate = "2022-08-17",
    serviceWorkerScript =
        `addEventListener("fetch", event => {
        `  event.respondWith(new Response("Hello: " + event.request.url + "\n"));
        `})
  ))"_kj));

  test.start();

  auto conn = test.connect("test-addr");

  // Send a request, get a response.
  conn.httpGet200("/", "Hello: http://foo/\n");

  // Send another request on the same connection, different path and host.
  conn.send(R"(
    GET /baz/qux?corge=grault HTTP/1.1
    Host: bar

  )"_blockquote);
  conn.recv(R"(
    HTTP/1.1 200 OK
    Content-Length: 39
    Content-Type: text/plain;charset=UTF-8

    Hello: http://bar/baz/qux?corge=grault
  )"_blockquote);

  // A request without `Host:` should 400.
  conn.send(R"(
    GET /baz/qux?corge=grault HTTP/1.1

  )"_blockquote);
  conn.recv(R"(
    HTTP/1.1 400 Bad Request
    Content-Length: 11

    Bad Request)"_blockquote);
}

KJ_TEST("Server: use service name as Service Worker origin") {
  TestServer test(singleWorker(R"((
    compatibilityDate = "2022-08-17",
    serviceWorkerScript =
        `addEventListener("fetch", event => {
        `  event.respondWith(new Response(new Error("Doh!").stack));
        `})
  ))"_kj));

  test.start();
  auto conn = test.connect("test-addr");
  conn.httpGet200("/", R"(
    Error: Doh!
        at hello:2:34)"_blockquote);
}

KJ_TEST("Server: serve basic modular Worker") {
  TestServer test(singleWorker(R"((
    compatibilityDate = "2022-08-17",
    modules = [
      ( name = "main.js",
        esModule =
          `export default {
          `  async fetch(request) {
          `    return new Response("Hello: " + request.url);
          `  }
          `}
      )
    ]
  ))"_kj));
  test.start();
  auto conn = test.connect("test-addr");
  conn.httpGet200("/", "Hello: http://foo/");
}

KJ_TEST("Server: serve modular Worker with imports") {
  TestServer test(singleWorker(R"((
    compatibilityDate = "2022-08-17",
    modules = [
      ( name = "main.js",
        esModule =
          `import { MESSAGE as FOO } from "foo.js";
          `import BAR from "bar.txt";
          `import BAZ from "baz.bin";
          `import QUX from "qux.json";
          `import CORGE from "corge.js";
          `import SQUARE_WASM from "square.wasm";
          `const SQUARE = new WebAssembly.Instance(SQUARE_WASM, {});
          `export default {
          `  async fetch(request) {
          `    return new Response([
          `        FOO, BAR, new TextDecoder().decode(BAZ), QUX.message, CORGE.message,
          `        "square.wasm says square(5) = " + SQUARE.exports.square(5)]
          `        .join("\n"));
          `  }
          `}
      ),
      ( name = "foo.js",
        esModule =
          `export let MESSAGE = "Hello from foo.js"
      ),
      ( name = "bar.txt",
        text = "Hello from bar.txt"
      ),
      ( name = "baz.bin",
        data = "Hello from baz.bin"
      ),
      ( name = "qux.json",
        json = `{"message": "Hello from qux.json"}
      ),
      ( name = "corge.js",
        commonJsModule =
          `module.exports.message = "Hello from corge.js";
      ),
      ( name = "square.wasm",
        # Exports a function 'square(x)' that returns x^2.
        wasm = 0x"00 61 73 6d 01 00 00 00  01 06 01 60 01 7f 01 7f
                  03 02 01 00 05 03 01 00  02 06 08 01 7f 01 41 80
                  88 04 0b 07 13 02 06 6d  65 6d 6f 72 79 02 00 06
                  73 71 75 61 72 65 00 00  0a 09 01 07 00 20 00 20
                  00 6c 0b"
      )
    ]
  ))"_kj));

  test.start();
  auto conn = test.connect("test-addr");
  conn.httpGet200("/",
      "Hello from foo.js\n"
      "Hello from bar.txt\n"
      "Hello from baz.bin\n"
      "Hello from qux.json\n"
      "Hello from corge.js\n"
      "square.wasm says square(5) = 25");
}

KJ_TEST("Server: compatibility dates") {
  // The easiest flag to test is the presence of the global `navigator`.
  auto selfNavigatorCheckerWorker = [](kj::StringPtr compatProperties) {
    return singleWorker(kj::str(R"((
      )",
        compatProperties, R"(,
      modules = [
        ( name = "main.js",
          esModule =
              `export default {
              `  async fetch(request) {
              `    return new Response(!!self.navigator);
              `  }
              `}
        )
      ]
    ))"_kj));
  };

  {
    TestServer test(selfNavigatorCheckerWorker("compatibilityDate = \"2022-08-17\""));

    test.start();
    auto conn = test.connect("test-addr");
    conn.httpGet200("/", "true");
  }

  // In the past, the global wasn't there.
  {
    TestServer test(selfNavigatorCheckerWorker("compatibilityDate = \"2020-01-01\""));

    test.start();
    auto conn = test.connect("test-addr");
    conn.httpGet200("/", "false");
  }

  // Disable using a flag instead of a date.
  {
    TestServer test(selfNavigatorCheckerWorker(
        "compatibilityDate = \"2022-08-17\", compatibilityFlags = [\"no_global_navigator\"]"));

    test.start();
    auto conn = test.connect("test-addr");
    conn.httpGet200("/", "false");
  }
}

KJ_TEST("Server: compatibility dates are required") {
  TestServer test(singleWorker(R"((
    serviceWorkerScript =
        `addEventListener("fetch", event => {
        `  event.respondWith(new Response("Hello: " + event.request.url + "\n"));
        `})
  ))"_kj));

  test.expectErrors(R"(
    service hello: Worker must specify compatibilityDate.
  )"_blockquote);
}

KJ_TEST("Server: value bindings") {
#if _WIN32
  _putenv("TEST_ENVIRONMENT_VAR=Hello from environment variable");
#else
  setenv("TEST_ENVIRONMENT_VAR", "Hello from environment variable", true);
#endif

  TestServer test(singleWorker(R"((
    compatibilityDate = "2022-08-17",
    # (Must use Service Worker syntax to allow Wasm bindings.)
    serviceWorkerScript =
      `const SQUARE = new WebAssembly.Instance(BAZ, {});
      `async function handle(request) {
      `  let items = [];
      `  items.push(FOO);
      `  items.push(new TextDecoder().decode(BAR));
      `  items.push("wasm says square(5) = " + SQUARE.exports.square(5));
      `  items.push(QUX.message);
      `  items.push(CORGE);
      `  items.push("GRAULT is null? " + (GRAULT === null));
      `  return new Response(items.join("\n"));
      `}
      `addEventListener("fetch", event => {
      `  event.respondWith(handle(event.request));
      `});
      ,
    bindings = [
      ( name = "FOO", text = "Hello from text binding" ),
      ( name = "BAR", data = "Hello from data binding" ),
      ( name = "BAZ",
        # Exports a function 'square(x)' that returns x^2.
        wasmModule = 0x"00 61 73 6d 01 00 00 00  01 06 01 60 01 7f 01 7f
                        03 02 01 00 05 03 01 00  02 06 08 01 7f 01 41 80
                        88 04 0b 07 13 02 06 6d  65 6d 6f 72 79 02 00 06
                        73 71 75 61 72 65 00 00  0a 09 01 07 00 20 00 20
                        00 6c 0b"
      ),
      ( name = "QUX",
        json = `{"message": "Hello from json binding"}
      ),
      ( name = "CORGE", fromEnvironment = "TEST_ENVIRONMENT_VAR" ),
      ( name = "GRAULT", fromEnvironment = "TEST_NONEXISTENT_ENVIRONMENT_VAR" ),
    ]
  ))"_kj));

  test.start();
  auto conn = test.connect("test-addr");
  conn.httpGet200("/",
      "Hello from text binding\n"
      "Hello from data binding\n"
      "wasm says square(5) = 25\n"
      "Hello from json binding\n"
      "Hello from environment variable\n"
      "GRAULT is null? true");
}

KJ_TEST("Server: WebCrypto bindings") {
  TestServer test(singleWorker(R"((
    compatibilityDate = "2022-08-17",
    modules = [
      ( name = "main.js",
        esModule =
          `function hex(buffer) {
          `  return [...new Uint8Array(buffer)]
          `      .map(x => x.toString(16).padStart(2, '0'))
          `      .join('');
          `}
          `
          `export default {
          `  async fetch(request, env) {
          `    let items = [];
          `
          `    let plaintext = new TextEncoder().encode("hello");
          `    let sig = await crypto.subtle.sign({"name": "HMAC", "hash": "SHA-256"},
          `                                       env.hmac, plaintext);
          `    items.push("hmac signature is " + hex(sig));
          `    let ver1 = await crypto.subtle.verify({"name": "HMAC", "hash": "SHA-256"},
          `                                          env.hmac, sig, plaintext);
          `    let ver2 = await crypto.subtle.verify({"name": "HMAC", "hash": "SHA-256"},
          `                                          env.hmac, sig, new Uint8Array([12, 34]));
          `    items.push("hmac verifications: " + ver1 + ", " + ver2);
          `    items.push("hmac extractable? " + env.hmac.extractable);
          `
          `    let hexSig = await crypto.subtle.sign({"name": "HMAC", "hash": "SHA-256"},
          `                                          env.hmacHex, plaintext);
          `    let b64Sig = await crypto.subtle.sign({"name": "HMAC", "hash": "SHA-256"},
          `                                          env.hmacBase64, plaintext);
          `    let jwkSig = await crypto.subtle.sign({"name": "HMAC", "hash": "SHA-256"},
          `                                          env.hmacJwk, plaintext);
          `    items.push("hmac signature (hex key) is " + hex(hexSig));
          `    items.push("hmac signature (base64 key) is " + hex(b64Sig));
          `    items.push("hmac signature (jwk key) is " + hex(jwkSig));
          `
          `    try {
          `      await crypto.subtle.verify({"name": "HMAC", "hash": "SHA-256"},
          `                                 env.hmacHex, sig, plaintext);
          `      items.push("verification with hmacHex was allowed");
          `    } catch (err) {
          `      items.push("verification with hmacHex was not allowed: " + err.message);
          `    }
          `
          `    let ecsig = await crypto.subtle.sign(
          `        {"name": "ECDSA", "namedCurve": "P-256", "hash": "SHA-256"},
          `        env.ecPriv, plaintext);
          `    let ecver = await crypto.subtle.verify(
          `        {"name": "ECDSA", "namedCurve": "P-256", "hash": "SHA-256"},
          `        env.ecPub, ecsig, plaintext);
          `    items.push("ec verification: " + ecver);
          `    items.push("ec extractable? " + env.ecPriv.extractable +
          `                             ", " + env.ecPub.extractable);
          `
          `    return new Response(items.join("\n"));
          `  }
          `}
      )
    ],
    bindings = [
      ( name = "hmac",
        cryptoKey = (
          raw = "testkey",
          algorithm = (
            json = `{"name": "HMAC", "hash": "SHA-256"}
          ),
          usages = [ sign, verify ]
        )
      ),
      ( name = "hmacHex",
        cryptoKey = (
          hex = "746573746b6579",
          algorithm = (
            json = `{"name": "HMAC", "hash": "SHA-256"}
          ),
          usages = [ sign ]
        )
      ),
      ( name = "hmacBase64",
        cryptoKey = (
          base64 = "dGVzdGtleQ==",
          algorithm = (
            json = `{"name": "HMAC", "hash": "SHA-256"}
          ),
          usages = [ sign ]
        )
      ),
      ( name = "hmacJwk",
        cryptoKey = (
          jwk = `{"alg":"HS256","k":"dGVzdGtleQ","kty":"oct"}
          ,
          algorithm = (
            json = `{"name": "HMAC", "hash": "SHA-256"}
          ),
          usages = [ sign ]
        )
      ),

      ( name = "ecPriv",
        cryptoKey = (
          pkcs8 =
            `-----BEGIN PRIVATE KEY-----
            `MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgXB5SjGILYt4DxPho
            `VUX/lMnLzpJD5R6Jl0bLCuRj8V2hRANCAAQ6pM4KrujAsw2xz0qA6l4DF/waMYVP
            `QNOAakb+S9GwkOgrTbw6AYoawTaW68Vbwadfe2S02ya6yEKGyE3N56by
            `-----END PRIVATE KEY-----
          ,
          algorithm = (
            json = `{"name": "ECDSA", "namedCurve": "P-256"}
          ),
          usages = [ sign ]
        )
      ),

      ( name = "ecPub",
        cryptoKey = (
          spki =
            `-----BEGIN PUBLIC KEY-----
            `MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEOqTOCq7owLMNsc9KgOpeAxf8GjGF
            `T0DTgGpG/kvRsJDoK028OgGKGsE2luvFW8GnX3tktNsmushChshNzeem8g==
            `-----END PUBLIC KEY-----
          ,
          algorithm = (
            json = `{"name": "ECDSA", "namedCurve": "P-256"}
          ),
          usages = [ verify ],
          extractable = true
        )
      )
    ]
  ))"_kj));

  test.start();
  auto conn = test.connect("test-addr");
  conn.httpGet200("/",
      "hmac signature is 4a27693183b28d2616209d6ff5e77646af5fc06ea6affac37415995b07be2ddf\n"
      "hmac verifications: true, false\n"
      "hmac extractable? false\n"
      "hmac signature (hex key) is "
      "4a27693183b28d2616209d6ff5e77646af5fc06ea6affac37415995b07be2ddf\n"
      "hmac signature (base64 key) is "
      "4a27693183b28d2616209d6ff5e77646af5fc06ea6affac37415995b07be2ddf\n"
      "hmac signature (jwk key) is "
      "4a27693183b28d2616209d6ff5e77646af5fc06ea6affac37415995b07be2ddf\n"
      "verification with hmacHex was not allowed: "
      "Requested key usage \"verify\" does not match any usage listed in this CryptoKey.\n"
      "ec verification: true\n"
      "ec extractable? false, true");
}

KJ_TEST("Server: subrequest to default outbound") {
  TestServer test(singleWorker(R"((
    compatibilityDate = "2022-08-17",
    modules = [
      ( name = "main.js",
        esModule =
          `export default {
          `  async fetch(request, env) {
          `    let resp = await fetch("http://subhost/foo");
          `    let txt = await resp.text();
          `    return new Response(
          `        "sub X-Foo header: " + resp.headers.get("X-Foo") + "\n" +
          `        "sub body: " + txt);
          `  }
          `}
      )
    ]
  ))"_kj));

  test.start();
  auto conn = test.connect("test-addr");
  conn.sendHttpGet("/");

  auto subreq = test.receiveInternetSubrequest("subhost");
  subreq.recv(R"(
    GET /foo HTTP/1.1
    Host: subhost

  )"_blockquote);
  subreq.send(R"(
    HTTP/1.1 200 OK
    Content-Length: 6
    X-Foo: bar

    corge
  )"_blockquote);

  conn.recvHttp200(R"(
    sub X-Foo header: bar
    sub body: corge
  )"_blockquote);
}

KJ_TEST("Server: override 'internet' service") {
  TestServer test(R"((
    services = [
      ( name = "hello",
        worker = (
          compatibilityDate = "2022-08-17",
          modules = [
            ( name = "main.js",
              esModule =
                `export default {
                `  async fetch(request, env) {
                `    return fetch(request);
                `  }
                `}
            )
          ]
        )
      ),
      ( name = "internet",
        external = "proxy-host" )
    ],
    sockets = [
      ( name = "main",
        address = "test-addr",
        service = "hello"
      )
    ]
  ))"_kj);

  test.start();
  auto conn = test.connect("test-addr");
  conn.sendHttpGet("/");

  auto subreq = test.receiveSubrequest("proxy-host");
  subreq.recv(R"(
    GET / HTTP/1.1
    Host: foo

  )"_blockquote);
  subreq.send(R"(
    HTTP/1.1 200 OK
    Content-Length: 2
    Content-Type: text/plain;charset=UTF-8

    OK
  )"_blockquote);

  conn.recvHttp200("OK");
}

KJ_TEST("Server: override globalOutbound") {
  TestServer test(R"((
    services = [
      ( name = "hello",
        worker = (
          compatibilityDate = "2022-08-17",
          modules = [
            ( name = "main.js",
              esModule =
                `export default {
                `  async fetch(request, env) {
                `    return fetch(request);
                `  }
                `}
            )
          ],
          globalOutbound = "alternate-outbound"
        )
      ),
      ( name = "alternate-outbound",
        external = "proxy-host" )
    ],
    sockets = [
      ( name = "main",
        address = "test-addr",
        service = "hello"
      )
    ]
  ))"_kj);

  test.start();
  auto conn = test.connect("test-addr");
  conn.sendHttpGet("/");

  auto subreq = test.receiveSubrequest("proxy-host");
  subreq.recv(R"(
    GET / HTTP/1.1
    Host: foo

  )"_blockquote);
  subreq.send(R"(
    HTTP/1.1 200 OK
    Content-Length: 2
    Content-Type: text/plain;charset=UTF-8

    OK
  )"_blockquote);

  conn.recvHttp200("OK");
}

KJ_TEST("Server: capability bindings") {
  TestServer test(R"((
    services = [
      ( name = "hello",
        worker = (
          compatibilityDate = "2022-08-17",
          modules = [
            ( name = "main.js",
              esModule =
                `export default {
                `  async fetch(request, env) {
                `    let items = [];
                `    items.push(await (await env.fetcher.fetch("http://foo")).text());
                `    items.push(await env.kv.get("bar"));
                `    items.push(await (await env.r2.get("baz")).text());
                `    await env.queue.send("hello");
                `    items.push("Hello from Queue\n");
                `    const connection = await env.hyperdrive.connect();
                `    const encoded = new TextEncoder().encode("hyperdrive-test");
                `    await connection.writable.getWriter().write(new Uint8Array(encoded));
                `    items.push(`Hello from Hyperdrive(${env.hyperdrive.user})\n`);
                `    return new Response(items.join(""));
                `  }
                `}
            )
          ],
          bindings = [
            ( name = "fetcher",
              service = "service-outbound"
            ),
            ( name = "kv",
              kvNamespace = "kv-outbound"
            ),
            ( name = "r2",
              r2Bucket = "r2-outbound"
            ),
            ( name = "queue",
              queue = "queue-outbound"
            ),
            ( name = "hyperdrive",
              hyperdrive = (
                designator = "hyperdrive-outbound",
                database = "test-db",
                user = "test-user",
                password = "test-password",
                scheme = "postgresql"
              )
            )
          ]
        )
      ),
      ( name = "service-outbound", external = "service-host" ),
      ( name = "kv-outbound", external = "kv-host" ),
      ( name = "r2-outbound", external = "r2-host" ),
      ( name = "queue-outbound", external = "queue-host" ),
      ( name = "hyperdrive-outbound", external = (
        address = "hyperdrive-host",
        tcp = ()
      ))
    ],
    sockets = [
      ( name = "main",
        address = "test-addr",
        service = "hello"
      )
    ]
  ))"_kj);

  test.start();
  auto conn = test.connect("test-addr");
  conn.sendHttpGet("/");

  {
    auto subreq = test.receiveSubrequest("service-host");
    subreq.recv(R"(
      GET / HTTP/1.1
      Host: foo

    )"_blockquote);
    subreq.send(R"(
      HTTP/1.1 200 OK
      Content-Length: 16
      Content-Type: text/plain;charset=UTF-8

      Hello from HTTP
    )"_blockquote);
  }

  {
    auto subreq = test.receiveSubrequest("kv-host");
    subreq.recv(R"(
      GET /bar?urlencoded=true HTTP/1.1
      Host: fake-host
      CF-KV-FLPROD-405: https://fake-host/bar?urlencoded=true

    )"_blockquote);
    subreq.send(R"(
      HTTP/1.1 200 OK
      Content-Length: 14

      Hello from KV
    )"_blockquote);
  }

  {
    auto subreq = test.receiveSubrequest("r2-host");
    subreq.recv(R"(
      GET / HTTP/1.1
      Host: fake-host
      CF-R2-Request: {"version":1,"method":"get","object":"baz"}

    )"_blockquote);
    subreq.send(R"(
      HTTP/1.1 200 OK
      Content-Length: 16
      CF-R2-Metadata-Size: 2

      {}Hello from R2
    )"_blockquote);
  }

  {
    auto subreq = test.receiveSubrequest("queue-host");
    // We use a regex match to avoid dealing with the non-text characters in the POST body (which
    // may change as v8 serialization versions change over time).
    subreq.recvRegex(R"(
      POST /message HTTP/1.1
      Content-Length: 9
      Host: fake-host
      Content-Type: application/octet-stream

      .+hello)"_blockquote);
    subreq.send(R"(
      HTTP/1.1 200 OK
      Content-Length: 2

      OK
    )"_blockquote);
  }

  {
    auto subreq = test.receiveSubrequest("hyperdrive-host");
    subreq.recv("hyperdrive-test");
  }
  conn.recvHttp200(R"(
    Hello from HTTP
    Hello from KV
    Hello from R2
    Hello from Queue
    Hello from Hyperdrive(test-user)
  )"_blockquote);
}

KJ_TEST("Server: cyclic bindings") {
  TestServer test(R"((
    services = [
      ( name = "service1",
        worker = (
          compatibilityDate = "2022-08-17",
          modules = [
            ( name = "main.js",
              esModule =
                `export default {
                `  async fetch(request, env) {
                `    if (request.url.endsWith("/done")) {
                `      return new Response("!");
                `    } else {
                `      let resp2 = await env.service2.fetch(request);
                `      let text = await resp2.text();
                `      return new Response("Hello " + text);
                `    }
                `  }
                `}
            )
          ],
          bindings = [(name = "service2", service = "service2")]
        )
      ),
      ( name = "service2",
        worker = (
          compatibilityDate = "2022-08-17",
          modules = [
            ( name = "main.js",
              esModule =
                `export default {
                `  async fetch(request, env) {
                `    let resp2 = await env.service1.fetch("http://foo/done");
                `    let text = await resp2.text();
                `    return new Response("World" + text);
                `  }
                `}
            )
          ],
          bindings = [(name = "service1", service = "service1")]
        )
      ),
    ],
    sockets = [
      ( name = "main",
        address = "test-addr",
        service = "service1"
      )
    ]
  ))"_kj);

  test.start();
  auto conn = test.connect("test-addr");
  conn.httpGet200("/", "Hello World!");
}

KJ_TEST("Server: named entrypoints") {
  TestServer test(R"((
    services = [
      ( name = "hello",
        worker = (
          compatibilityDate = "2022-08-17",
          modules = [
            ( name = "main.js",
              esModule =
                `export default {
                `  async fetch(request, env) {
                `    return new Response("hello from default entrypoint");
                `  }
                `}
                `export let foo = {
                `  async fetch(request, env) {
                `    return new Response("hello from foo entrypoint");
                `  }
                `}
                `export let bar = {
                `  async fetch(request, env) {
                `    return new Response("hello from bar entrypoint");
                `  }
                `}
                `
                `// Also export some symbols that aren't valid entrypoints, but we should still
                `// be allowed to point sockets at them. (Sending any actual requests to them
                `// will still fail.)
                `export let invalidObj = {};  // no handlers
                `export let invalidArray = [1, 2];
                `export let invalidMap = new Map();
            )
          ]
        )
      ),
    ],
    sockets = [
      ( name = "main", address = "test-addr", service = "hello" ),
      ( name = "alt1", address = "foo-addr", service = (name = "hello", entrypoint = "foo")),
      ( name = "alt2", address = "bar-addr", service = (name = "hello", entrypoint = "bar")),

      ( name = "invalid1", address = "invalid1-addr",
        service = (name = "hello", entrypoint = "invalidObj")),
      ( name = "invalid2", address = "invalid2-addr",
        service = (name = "hello", entrypoint = "invalidArray")),
      ( name = "invalid3", address = "invalid3-addr",
        service = (name = "hello", entrypoint = "invalidMap")),
    ]
  ))"_kj);

  test.start();

  {
    auto conn = test.connect("test-addr");
    conn.httpGet200("/", "hello from default entrypoint");
  }

  {
    auto conn = test.connect("foo-addr");
    conn.httpGet200("/", "hello from foo entrypoint");
  }

  {
    auto conn = test.connect("bar-addr");
    conn.httpGet200("/", "hello from bar entrypoint");
  }
}

KJ_TEST("Server: invalid entrypoint") {
  TestServer test(R"((
    services = [
      ( name = "hello",
        worker = (
          compatibilityDate = "2022-08-17",
          modules = [
            ( name = "main.js",
              esModule =
                `export default {
                `  async fetch(request, env) {
                `    return env.svc.fetch(request);
                `  }
                `}
            )
          ],
          bindings = [(name = "svc", service = (name = "hello", entrypoint = "bar"))],
        )
      ),
    ],
    sockets = [
      ( name = "main", address = "test-addr", service = "hello" ),
      ( name = "alt1", address = "foo-addr", service = (name = "hello", entrypoint = "foo")),
    ]
  ))"_kj);

  test.expectErrors(
      "Worker \"hello\"'s binding \"svc\" refers to service \"hello\" with a named entrypoint "
      "\"bar\", but \"hello\" has no such named entrypoint.\n"
      "Socket \"alt1\" refers to service \"hello\" with a named entrypoint \"foo\", but \"hello\" "
      "has no such named entrypoint.\n");
}

KJ_TEST("Server: call queue handler on service binding") {
  TestServer test(R"((
    services = [
      ( name = "service1",
        worker = (
          compatibilityDate = "2022-08-17",
          compatibilityFlags = ["service_binding_extra_handlers"],
          modules = [
            ( name = "main.js",
              esModule =
                `export default {
                `  async fetch(request, env) {
                `    let result = await env.service2.queue("queueName1", [
                `        {id: "1", timestamp: 12345, body: "my message", attempts: 1},
                `        {id: "msg2", timestamp: 23456, body: 22, attempts: 2},
                `    ]);
                `    return new Response(`queue outcome: ${result.outcome}, ackAll: ${result.ackAll}`);
                `  }
                `}
            )
          ],
          bindings = [(name = "service2", service = "service2")]
        )
      ),
      ( name = "service2",
        worker = (
          compatibilityDate = "2022-08-17",
          modules = [
            ( name = "main.js",
              esModule =
                `export default {
                `  async fetch(request, env) {
                `    throw new Error("unimplemented");
                `  },
                `  async queue(event) {
                `    if (event.queue == "queueName1" &&
                `        event.messages.length == 2 &&
                `        event.messages[0].id == "1" &&
                `        event.messages[0].timestamp.getTime() == 12345 &&
                `        event.messages[0].body == "my message" &&
                `        event.messages[0].attempts == 1 &&
                `        event.messages[1].id == "msg2" &&
                `        event.messages[1].timestamp.getTime() == 23456 &&
                `        event.messages[1].body == 22 &&
                `        event.messages[1].attempts == 2) {
                `      event.ackAll();
                `      return;
                `    }
                `    throw new Error("messages didn't match expectations: " + JSON.stringify(event.messages));
                `  }
                `}
            )
          ]
        )
      ),
    ],
    sockets = [
      ( name = "main",
        address = "test-addr",
        service = "service1"
      )
    ]
  ))"_kj);

  test.server.allowExperimental();
  test.start();
  auto conn = test.connect("test-addr");
  conn.httpGet200("/", "queue outcome: ok, ackAll: true");
}

KJ_TEST("Server: Durable Objects (in memory)") {
  TestServer test(R"((
    services = [
      ( name = "hello",
        worker = (
          compatibilityDate = "2022-08-17",
          modules = [
            ( name = "main.js",
              esModule =
                `export default {
                `  async fetch(request, env) {
                `    let id = env.ns.idFromName(request.url)
                `    let actor = env.ns.get(id)
                `    return await actor.fetch(request)
                `  }
                `}
                `export class MyActorClass {
                `  constructor(state, env) {
                `    this.storage = state.storage;
                `    this.id = state.id;
                `  }
                `  async fetch(request) {
                `    let count = (await this.storage.get("foo")) || 0;
                `    this.storage.put("foo", count + 1);
                `    return new Response(this.id + ": " + request.url + " " + count);
                `  }
                `}
            )
          ],
          bindings = [(name = "ns", durableObjectNamespace = "MyActorClass")],
          durableObjectNamespaces = [
            ( className = "MyActorClass",
              uniqueKey = "mykey",
            )
          ],
          durableObjectStorage = (inMemory = void)
        )
      ),
    ],
    sockets = [
      ( name = "main",
        address = "test-addr",
        service = "hello"
      )
    ]
  ))"_kj);

  test.start();
  auto conn = test.connect("test-addr");
  conn.httpGet200(
      "/", "59002eb8cf872e541722977a258a12d6a93bbe8192b502e1c0cb250aa91af234: http://foo/ 0");
  conn.httpGet200(
      "/", "59002eb8cf872e541722977a258a12d6a93bbe8192b502e1c0cb250aa91af234: http://foo/ 1");
  conn.httpGet200(
      "/", "59002eb8cf872e541722977a258a12d6a93bbe8192b502e1c0cb250aa91af234: http://foo/ 2");
  conn.httpGet200(
      "/bar", "02b496f65dd35cbac90e3e72dc5a398ee93926ea4a3821e26677082d2e6f9b79: http://foo/bar 0");
  conn.httpGet200(
      "/bar", "02b496f65dd35cbac90e3e72dc5a398ee93926ea4a3821e26677082d2e6f9b79: http://foo/bar 1");
  conn.httpGet200(
      "/", "59002eb8cf872e541722977a258a12d6a93bbe8192b502e1c0cb250aa91af234: http://foo/ 3");
  conn.httpGet200(
      "/bar", "02b496f65dd35cbac90e3e72dc5a398ee93926ea4a3821e26677082d2e6f9b79: http://foo/bar 2");
}

KJ_TEST("Server: Durable Objects (on disk)") {
  kj::StringPtr config = R"((
    services = [
      ( name = "hello",
        worker = (
          compatibilityDate = "2022-08-17",
          modules = [
            ( name = "main.js",
              esModule =
                `export default {
                `  async fetch(request, env) {
                `    let id = env.ns.idFromName(request.url)
                `    let actor = env.ns.get(id)
                `    return await actor.fetch(request)
                `  }
                `}
                `export class MyActorClass {
                `  constructor(state, env) {
                `    this.storage = state.storage;
                `    this.id = state.id;
                `  }
                `  async fetch(request) {
                `    let count = (await this.storage.get("foo")) || 0;
                `    this.storage.put("foo", count + 1);
                `    return new Response(this.id + ": " + request.url + " " + count);
                `  }
                `}
            )
          ],
          bindings = [(name = "ns", durableObjectNamespace = "MyActorClass")],
          durableObjectNamespaces = [
            ( className = "MyActorClass",
              uniqueKey = "mykey",
            )
          ],
          durableObjectStorage = (localDisk = "my-disk")
        )
      ),
      ( name = "my-disk",
        disk = (
          path = "../../var/do-storage",
          writable = true,
        )
      ),
    ],
    sockets = [
      ( name = "main",
        address = "test-addr",
        service = "hello"
      )
    ]
  ))"_kj;

  // Create a directory outside of the test scope which we can use across multiple TestServers.
  auto dir = kj::newInMemoryDirectory(kj::nullClock());

  {
    TestServer test(config);

    // Link our directory into the test filesystem.
    test.root->transfer(kj::Path({"var"_kj, "do-storage"_kj}),
        kj::WriteMode::CREATE | kj::WriteMode::CREATE_PARENT, *dir, nullptr,
        kj::TransferMode::LINK);

    test.start();
    auto conn = test.connect("test-addr");
    conn.httpGet200(
        "/", "59002eb8cf872e541722977a258a12d6a93bbe8192b502e1c0cb250aa91af234: http://foo/ 0");
    conn.httpGet200(
        "/", "59002eb8cf872e541722977a258a12d6a93bbe8192b502e1c0cb250aa91af234: http://foo/ 1");
    conn.httpGet200(
        "/", "59002eb8cf872e541722977a258a12d6a93bbe8192b502e1c0cb250aa91af234: http://foo/ 2");
    conn.httpGet200("/bar",
        "02b496f65dd35cbac90e3e72dc5a398ee93926ea4a3821e26677082d2e6f9b79: http://foo/bar 0");
    conn.httpGet200("/bar",
        "02b496f65dd35cbac90e3e72dc5a398ee93926ea4a3821e26677082d2e6f9b79: http://foo/bar 1");
    conn.httpGet200(
        "/", "59002eb8cf872e541722977a258a12d6a93bbe8192b502e1c0cb250aa91af234: http://foo/ 3");
    conn.httpGet200("/bar",
        "02b496f65dd35cbac90e3e72dc5a398ee93926ea4a3821e26677082d2e6f9b79: http://foo/bar 2");

    // The storage directory contains .sqlite and .sqlite-wal files for both objects. Note that
    // the `-shm` files are missing because SQLite doesn't actually tell the VFS to create these
    // as separate files, it leaves it up to the VFS to decide how shared memory works, and our
    // KJ-wrapping VFS currently doesn't put this in SHM files. If we were using a real disk
    // directory, though, they would be there.
    KJ_EXPECT(dir->openSubdir(kj::Path({"mykey"}))->listNames().size() == 4);
    KJ_EXPECT(dir->exists(kj::Path(
        {"mykey", "02b496f65dd35cbac90e3e72dc5a398ee93926ea4a3821e26677082d2e6f9b79.sqlite"})));
    KJ_EXPECT(dir->exists(kj::Path(
        {"mykey", "02b496f65dd35cbac90e3e72dc5a398ee93926ea4a3821e26677082d2e6f9b79.sqlite-wal"})));
    KJ_EXPECT(dir->exists(kj::Path(
        {"mykey", "59002eb8cf872e541722977a258a12d6a93bbe8192b502e1c0cb250aa91af234.sqlite"})));
    KJ_EXPECT(dir->exists(kj::Path(
        {"mykey", "59002eb8cf872e541722977a258a12d6a93bbe8192b502e1c0cb250aa91af234.sqlite-wal"})));
  }

  // Having torn everything down, the WAL files should be gone.
  KJ_EXPECT(dir->openSubdir(kj::Path({"mykey"}))->listNames().size() == 2);
  KJ_EXPECT(dir->exists(kj::Path(
      {"mykey", "02b496f65dd35cbac90e3e72dc5a398ee93926ea4a3821e26677082d2e6f9b79.sqlite"})));
  KJ_EXPECT(dir->exists(kj::Path(
      {"mykey", "59002eb8cf872e541722977a258a12d6a93bbe8192b502e1c0cb250aa91af234.sqlite"})));

  // Let's start a new server and verify it can load the files from disk.
  {
    TestServer test(config);

    // Link our directory into the test filesystem.
    test.root->transfer(kj::Path({"var"_kj, "do-storage"_kj}),
        kj::WriteMode::CREATE | kj::WriteMode::CREATE_PARENT, *dir, nullptr,
        kj::TransferMode::LINK);

    test.start();
    auto conn = test.connect("test-addr");
    conn.httpGet200(
        "/", "59002eb8cf872e541722977a258a12d6a93bbe8192b502e1c0cb250aa91af234: http://foo/ 4");
    conn.httpGet200(
        "/", "59002eb8cf872e541722977a258a12d6a93bbe8192b502e1c0cb250aa91af234: http://foo/ 5");
    conn.httpGet200("/bar",
        "02b496f65dd35cbac90e3e72dc5a398ee93926ea4a3821e26677082d2e6f9b79: http://foo/bar 3");
  }
}

KJ_TEST("Server: Ephemeral Objects") {
  TestServer test(R"((
    services = [
      ( name = "hello",
        worker = (
          compatibilityDate = "2022-08-17",
          modules = [
            ( name = "main.js",
              esModule =
                `export default {
                `  async fetch(request, env) {
                `    let actor = env.ns.get(request.url)
                `    return await actor.fetch(request)
                `  }
                `}
                `export class MyActorClass {
                `  constructor(state, env) {
                `    if (state.storage) throw new Error("storage shouldn't be present");
                `    this.id = state.id;
                `    this.count = 0;
                `  }
                `  async fetch(request) {
                `    return new Response(this.id + ": " + request.url + " " + this.count++);
                `  }
                `}
            )
          ],
          bindings = [(name = "ns", durableObjectNamespace = "MyActorClass")],
          durableObjectNamespaces = [
            ( className = "MyActorClass",
              ephemeralLocal = void,
            )
          ],
          durableObjectStorage = (inMemory = void)
        )
      ),
    ],
    sockets = [
      ( name = "main",
        address = "test-addr",
        service = "hello"
      )
    ]
  ))"_kj);

  test.server.allowExperimental();
  test.start();
  auto conn = test.connect("test-addr");
  conn.httpGet200("/", "http://foo/: http://foo/ 0");
  conn.httpGet200("/", "http://foo/: http://foo/ 1");
  conn.httpGet200("/", "http://foo/: http://foo/ 2");
  conn.httpGet200("/bar", "http://foo/bar: http://foo/bar 0");
  conn.httpGet200("/bar", "http://foo/bar: http://foo/bar 1");
  conn.httpGet200("/", "http://foo/: http://foo/ 3");
  conn.httpGet200("/bar", "http://foo/bar: http://foo/bar 2");
}

KJ_TEST("Server: Durable Objects (ephemeral) eviction") {
  TestServer test(R"((
    services = [
      ( name = "hello",
        worker = (
          compatibilityDate = "2023-08-17",
          modules = [
            ( name = "main.js",
              esModule =
                `export default {
                `  async fetch(request, env) {
                `    let id = env.ns.idFromName("59002eb8cf872e541722977a258a12d6a93bbe8192b502e1c0cb250aa91af234");
                `    let obj = env.ns.get(id)
                `    if (request.url.endsWith("/setup")) {
                `      return await obj.fetch("http://example.com/setup");
                `    } else if (request.url.endsWith("/check")) {
                `      try {
                `        return await obj.fetch("http://example.com/check");
                `      } catch(e) {
                `        throw e;
                `      }
                `    } else if (request.url.endsWith("/checkEvicted")) {
                `      return await obj.fetch("http://example.com/checkEvicted");
                `    }
                `    return new Response("Invalid Route!")
                `  }
                `}
                `export class MyActorClass {
                `  constructor(state, env) {
                `    this.defaultMessage = false; // Set to true on first "setup" request
                `  }
                `  async fetch(request) {
                `    if (request.url.endsWith("/setup")) {
                `      // Request 1, set defaultMessage, will remain true as long as actor is live.
                `      this.defaultMessage = true;
                `      return new Response("OK");
                `    } else if (request.url.endsWith("/check")) {
                `      // Request 2, assert that actor is still in alive (defaultMessage is still true).
                `      if (this.defaultMessage) {
                `        // Actor is still alive and we did not re-run the constructor
                `        return new Response("OK");
                `      }
                `      throw new Error("Error: Actor was evicted!");
                `    } else if (request.url.endsWith("/checkEvicted")) {
                `      // Final request (3), check if the defaultMessage has been set to false,
                `      //  indicating the actor was evicted
                `      if (!this.defaultMessage) {
                `        // Actor was evicted and we re-ran the constructor!
                `        return new Response("OK");
                `      }
                `      throw new Error("Error: Actor was not evicted! We were still alive.");
                `    }
                `  }
                `}
            )
          ],
          bindings = [(name = "ns", durableObjectNamespace = "MyActorClass")],
          durableObjectNamespaces = [
            ( className = "MyActorClass",
              uniqueKey = "mykey",
            )
          ],
          durableObjectStorage = (inMemory = void)
        )
      ),
    ],
    sockets = [
      ( name = "main",
        address = "test-addr",
        service = "hello"
      )
    ]
  ))"_kj);

  test.start();
  auto conn = test.connect("test-addr");
  conn.httpGet200("/setup", "OK");
  conn.httpGet200("/check", "OK");

  // Force hibernation by waiting 10 seconds.
  test.wait(10);
  // Need a second connection because of 5 second HTTP timeout.
  auto connTwo = test.connect("test-addr");
  connTwo.httpGet200("/checkEvicted", "OK");
}

KJ_TEST("Server: Durable Objects (ephemeral) prevent eviction") {
  TestServer test(R"((
    services = [
      ( name = "hello",
        worker = (
          compatibilityDate = "2023-08-17",
          modules = [
            ( name = "main.js",
              esModule =
                `export default {
                `  async fetch(request, env) {
                `    let id = env.ns.idFromName("59002eb8cf872e541722977a258a12d6a93bbe8192b502e1c0cb250aa91af234");
                `    let obj = env.ns.get(id);
                `    if (request.url.endsWith("/setup")) {
                `      return await obj.fetch("http://example.com/setup");
                `    } else if (request.url.endsWith("/assertNotEvicted")) {
                `      try {
                `        return await obj.fetch("http://example.com/assertNotEvicted");
                `      } catch(e) {
                `        throw e;
                `      }
                `    }
                `    return new Response("Invalid Route!")
                `  }
                `}
                `export class MyActorClass {
                `  constructor(state, env) {
                `    this.defaultMessage = false; // Set to true on first "setup" request
                `  }
                `  async fetch(request) {
                `    if (request.url.endsWith("/setup")) {
                `      // Request 1, set defaultMessage, will remain true as long as actor is live.
                `      this.defaultMessage = true;
                `      return new Response("OK");
                `    } else if (request.url.endsWith("/assertNotEvicted")) {
                `      // Request 2, assert that actor is still in alive (defaultMessage is still true).
                `      if (this.defaultMessage) {
                `        // Actor is still alive and we did not re-run the constructor
                `        return new Response("OK");
                `      }
                `      throw new Error("Error: Actor was evicted!");
                `    }
                `  }
                `}
            )
          ],
          bindings = [(name = "ns", durableObjectNamespace = "MyActorClass")],
          durableObjectNamespaces = [
            ( className = "MyActorClass",
              uniqueKey = "mykey",
              preventEviction = true,
            )
          ],
          durableObjectStorage = (inMemory = void)
        )
      ),
    ],
    sockets = [
      ( name = "main",
        address = "test-addr",
        service = "hello"
      )
    ]
  ))"_kj);

  test.start();
  auto conn = test.connect("test-addr");
  conn.httpGet200("/setup", "OK");
  conn.httpGet200("/assertNotEvicted", "OK");

  // Attempt to force hibernation by waiting 10 seconds.
  test.wait(10);
  // Need a second connection because of 5 second HTTP timeout.
  auto connTwo = test.connect("test-addr");
  connTwo.httpGet200("/assertNotEvicted", "OK");
}

KJ_TEST("Server: Durable Object evictions when callback scheduled") {
  kj::StringPtr config = R"((
    services = [
      ( name = "hello",
        worker = (
          compatibilityDate = "2023-08-17",
          modules = [
            ( name = "main.js",
              esModule =
                `export default {
                `  async fetch(request, env) {
                `    let id = env.ns.idFromName("59002eb8cf872e541722977a258a12d6a93bbe8192b502e1c0cb250aa91af234");
                `    let obj = env.ns.get(id)
                `    return await obj.fetch(request.url);
                `  }
                `}
                `export class MyActorClass {
                `  constructor(state, env) {
                `    this.defaultMessage = false; // Set to true on first "setup" request
                `    this.storage = state.storage;
                `    this.count = 0;
                `  }
                `  async fetch(request) {
                `    if (request.url.endsWith("/15Seconds")) {
                `      // Schedule a callback to run in 15 seconds.
                `      // The DO should NOT be evicted by the inactivity timeout before this runs.
                `      this.defaultMessage = true;
                `      let id = setInterval(() => { clearInterval(id); }, 15000);
                `      return new Response("OK");
                `    } else if (request.url.endsWith("/20Seconds")) {
                `      // Schedule a callback to run every 20 seconds.
                `      // The DO should expire after 70 seconds.
                `      this.defaultMessage = true;
                `      this.count = 0;
                `      await this.storage.put("count", this.count);
                `      let id = setInterval(() => {
                `        // Increment number of times we ran this.
                `        this.count += 1;
                `        this.storage.put("count", this.count);
                `      }, 20000);
                `      return new Response("OK");
                `    } else if (request.url.endsWith("/assertActive")) {
                `      // Assert that actor is still in alive (defaultMessage is still true).
                `      if (this.defaultMessage) {
                `        // Actor is still alive and we did not re-run the constructor
                `        return new Response("OK");
                `      }
                `      throw new Error("Error: Actor was evicted!");
                `    } else if (request.url.endsWith("/assertEvicted")) {
                `      // Check if the defaultMessage has been set to false,
                `      // indicating the actor was evicted
                `      if (!this.defaultMessage) {
                `        // Actor was evicted and we re-ran the constructor!
                `        return new Response("OK");
                `      }
                `      throw new Error("Error: Actor was not evicted! We were still alive.");
                `    } else if (request.url.endsWith("/assertEvictedAndCount")) {
                `      // Check if the defaultMessage has been set to false,
                `      // indicating the actor was evicted
                `      if (!this.defaultMessage) {
                `        var count = await this.storage.get("count");
                `        if (!(4 < count && count < 8)) {
                `          // Something must have gone wrong. We have a 70 sec expiration,
                `          // and worst case is it takes ~140 seconds to evict. The callback runs
                `          // every 20 seconds, so it has to be evicted before the 8th callback.
                `          throw new Error(`Callback ran ${count} times, expected between 4 to 8!`);
                `        }
                `        // Actor was evicted and we had the right count!
                `        return new Response("OK");
                `      }
                `      throw new Error("Error: Actor was not evicted! We were still alive.");
                `    }
                `  }
                `}
            )
          ],
          bindings = [(name = "ns", durableObjectNamespace = "MyActorClass")],
          durableObjectNamespaces = [
            ( className = "MyActorClass",
              uniqueKey = "mykey",
            )
          ],
          durableObjectStorage = (localDisk = "my-disk")
        )
      ),
      ( name = "my-disk",
        disk = (
          path = "../../var/do-storage",
          writable = true,
        )
      ),
    ],
    sockets = [
      ( name = "main",
        address = "test-addr",
        service = "hello"
      )
    ]
  ))"_kj;

  // Create a directory outside of the test scope which we can use across multiple TestServers.
  auto dir = kj::newInMemoryDirectory(kj::nullClock());
  {
    TestServer test(config);
    // Link our directory into the test filesystem.
    test.root->transfer(kj::Path({"var"_kj, "do-storage"_kj}),
        kj::WriteMode::CREATE | kj::WriteMode::CREATE_PARENT, *dir, nullptr,
        kj::TransferMode::LINK);

    test.start();
    auto conn = test.connect("test-addr");
    // Setup a callback that will run in 15 seconds.
    // This callback should prevent the DO from being evicted.
    conn.httpGet200("/15Seconds", "OK");

    // If we weren't waiting on anything, the DO would be evicted after 10 seconds,
    // however, it will actually be evicted in 25 seconds (15 seconds until setInterval is cleared +
    // 10 seconds for inactivity timer).

    test.wait(15);
    // The `setInterval()` will be cleared around now. Let's verify that we didn't get evicted.

    // Need a new connection because of 5 second HTTP timeout.
    auto connTwo = test.connect("test-addr");
    connTwo.httpGet200("/assertActive", "OK");

    // Force hibernation by waiting at least 10 seconds since we haven't scheduled any new work.
    test.wait(10);

    // Need a new connection because of 5 second HTTP timeout.
    auto connThree = test.connect("test-addr");
    connThree.httpGet200("/assertEvicted", "OK");

    // Now we know we aren't evicting DOs early if they have future work scheduled. Next, let's
    // ensure we ARE evicting DOs if there are no connected clients for 70 seconds.
    // Note that the `/20seconds` path calls setInterval to run every 20 seconds, and never clears.
    auto connFour = test.connect("test-addr");
    connFour.httpGet200("/20Seconds", "OK");
    // It's unlikely, but the worst case is the cleanupLoop checks just before the 70 sec expiration,
    // and has to wait another 70 seconds before trying to remove again. We'll wait for 142 seconds
    // to account for this.
    test.wait(142);

    auto connFive = test.connect("test-addr");
    connFive.httpGet200("/assertEvictedAndCount", "OK");
  }
}

KJ_TEST("Server: Durable Objects websocket") {
  TestServer test(R"((
    services = [
      ( name = "hello",
        worker = (
          compatibilityDate = "2023-08-17",
          modules = [
            ( name = "main.js",
              esModule =
                `export default {
                `  async fetch(request, env) {
                `    let id = env.ns.idFromName("59002eb8cf872e541722977a258a12d6a93bbe8192b502e1c0cb250aa91af234");
                `    let obj = env.ns.get(id)
                `    return await obj.fetch(request);
                `  }
                `}
                `
                `export class MyActorClass {
                `  constructor(state) {}
                `
                `  async fetch(request) {
                `    let pair = new WebSocketPair();
                `    let ws = pair[1]
                `    ws.accept();
                `
                `    ws.addEventListener("message", (m) => {
                `      ws.send(m.data);
                `    });
                `    ws.addEventListener("close", (c) => {
                `      ws.close(c.code, c.reason);
                `    });
                `
                `    return new Response(null, {status: 101, statusText: "Switching Protocols", webSocket: pair[0]});
                `  }
                `}
            )
          ],
          bindings = [(name = "ns", durableObjectNamespace = "MyActorClass")],
          durableObjectNamespaces = [
            ( className = "MyActorClass",
              uniqueKey = "mykey",
            )
          ],
          durableObjectStorage = (inMemory = void)
        )
      ),
    ],
    sockets = [
      ( name = "main",
        address = "test-addr",
        service = "hello"
      )
    ]
  ))"_kj);

  test.start();
  auto wsConn = test.connect("test-addr");
  wsConn.upgradeToWebSocket();
  constexpr kj::StringPtr expectedOne = "Hello"_kj;
  constexpr kj::StringPtr expectedTwo = "There"_kj;
  // \x81\x05 are part of the websocket frame.
  // \x81 is 10000001 -- leftmost bit implies this is the final frame, rightmost implies text data.
  // \x05 says the payload length is 5.
  wsConn.send(kj::str("\x81\x05", expectedOne));
  wsConn.send(kj::str("\x81\x05", expectedTwo));
  wsConn.recvWebSocket(expectedOne);
  wsConn.recvWebSocket(expectedTwo);

  // Force hibernation by waiting 10 seconds.
  test.wait(10);
  wsConn.send(kj::str("\x81\x05", expectedOne));
  wsConn.send(kj::str("\x81\x05", expectedTwo));
  wsConn.recvWebSocket(expectedOne);
  wsConn.recvWebSocket(expectedTwo);
}

KJ_TEST("Server: Durable Objects websocket hibernation") {
  TestServer test(R"((
    services = [
      ( name = "hello",
        worker = (
          compatibilityDate = "2023-08-17",
          modules = [
            ( name = "main.js",
              esModule =
                `export default {
                `  async fetch(request, env) {
                `    let id = env.ns.idFromName("59002eb8cf872e541722977a258a12d6a93bbe8192b502e1c0cb250aa91af234");
                `    let obj = env.ns.get(id)
                `
                `    // 1. Create a websocket (request 1)
                `    // 2. Use websocket once
                `    // 3. Let actor hibernate
                `    // 4. Wake actor by sending new request (request 2)
                `    //  - This confirms we get back hibernation manager.
                `    //    5. Use websocket once
                `    // 6. Let actor hibernate
                `    // 7. Wake actor by using websocket
                `    //  - This confirms we get back hibernation manager.
                `    //    8. Use websocket once
                `    return await obj.fetch(request);
                `  }
                `}
                `
                `export class MyActorClass {
                `  constructor(state) {
                `    this.state = state;
                `    // If reqCount is 0, then the actor's constructor has run.
                `    // This implies we're starting up, so either this is the first request or we were evicted.
                `    this.reqCount = 0;
                `  }
                `
                `  async fetch(request) {
                `    if (request.url.endsWith("/")) {
                `      // Request 1, accept a websocket.
                `      let pair = new WebSocketPair(true);
                `      let ws = pair[1];
                `      this.state.acceptWebSocket(ws);
                `
                `      this.reqCount += 1;
                `      if (this.reqCount != 1) {
                `        throw new Error(`Expected request count of 1 but got ${this.reqCount}`);
                `      }
                `      return new Response(null, {status: 101, statusText: "Switching Protocols", webSocket: pair[0]});
                `    } else if (request.url.endsWith("/wakeUpAndCheckWS")) {
                `      // Request 2, wake actor and check if WS available.
                `      let allWebsockets = this.state.getWebSockets();
                `      for (const ws of allWebsockets) {
                `        ws.send("Hello! Just woke up from a nap.");
                `      }
                `
                `      this.reqCount += 1;
                `      if (this.reqCount != 1) {
                `        throw new Error(`Expected request count of 1 but got ${this.reqCount}`);
                `      }
                `
                `      return new Response("OK");
                `    }
                `    return new Error("Unknown path!");
                `  }
                `
                `  async webSocketMessage(ws, msg) {
                `    if (msg == "Regular message.") {
                `      ws.send("Regular response.");
                `    } else if (msg == "Confirm actor was evicted.") {
                `      // Called when waking from hibernation due to inbound websocket message.
                `      if (this.reqCount == 0) {
                `        ws.send("OK")
                `      } else {
                `        ws.send(`[ FAILURE ] - reqCount was ${this.reqCount} so actor wasn't evicted`);
                `      }
                `    }
                `  }
                `
                `  async webSocketClose(ws, code, reason, wasClean) {
                `    if (code == 1006) {
                `      if (reason != "WebSocket disconnected without sending Close frame.") {
                `        throw new Error(`Got abnormal closure with unexpected reason: ${reason}`);
                `      }
                `      if (wasClean) {
                `        throw new Error("Got abnormal closure but wasClean was true!");
                `      }
                `    } else if (code != 1234) {
                `      throw new Error(`Expected close code 1234, got ${code}`);
                `    } else if (reason != "OK") {
                `      throw new Error(`Expected close reason "OK", got ${reason}`);
                `    } else {
                `      ws.close(4321, "KO");
                `    }
                `  }
                `
                `  async webSocketError(ws, error) {
                `    console.log(`Encountered error: ${error}`);
                `    throw new Error(error);
                `  }
                `}

            )
          ],
          bindings = [(name = "ns", durableObjectNamespace = "MyActorClass")],
          durableObjectNamespaces = [
            ( className = "MyActorClass",
              uniqueKey = "mykey",
            )
          ],
          durableObjectStorage = (inMemory = void)
        )
      ),
    ],
    sockets = [
      ( name = "main",
        address = "test-addr",
        service = "hello"
      )
    ]
  ))"_kj);

  test.start();
  auto wsConn = test.connect("test-addr");
  wsConn.upgradeToWebSocket();
  // 1. Make hibernatable ws and use it.
  constexpr kj::StringPtr message = "Regular message."_kj;
  constexpr kj::StringPtr response = "Regular response."_kj;
  wsConn.send(kj::str("\x81\x10", message));
  wsConn.recvWebSocket(response);

  // 2. Hibernate
  test.wait(10);
  // 3. Use normal connection and read from ws.
  auto conn = test.connect("test-addr");
  conn.httpGet200("/wakeUpAndCheckWS", "OK"_kj);
  constexpr kj::StringPtr unpromptedResponse = "Hello! Just woke up from a nap."_kj;
  wsConn.recvWebSocket(unpromptedResponse);

  // 4. Hibernate again
  test.wait(10);

  // 5. Wake up by sending a message.
  constexpr kj::StringPtr confirmEviction = "Confirm actor was evicted."_kj;
  constexpr kj::StringPtr evicted = "OK"_kj;
  wsConn.send(kj::str("\x81\x1a", confirmEviction));
  wsConn.recvWebSocket(evicted);
}

KJ_TEST("Server: tail workers") {
  TestServer test(R"((
    services = [
      ( name = "hello",
        worker = (
          compatibilityDate = "2024-11-01",
          modules = [
            ( name = "main.js",
              esModule =
                `export default {
                `  async fetch(req, env, ctx) {
                `    console.log("foo", "bar");
                `    console.log("baz");
                `    return new Response("OK");
                `  }
                `}
            )
          ],
          tails = ["tail", "tail2"],
        )
      ),
      ( name = "tail",
        worker = (
          compatibilityDate = "2024-11-01",
          modules = [
            ( name = "main.js",
              esModule =
                `export default {
                `  async tail(req, env, ctx) {
                `    await fetch("http://tail", {
                `      method: "POST",
                `      body: JSON.stringify(req[0].logs.map(log => log.message))
                `    });
                `  }
                `}
            )
          ],
        )
      ),
      ( name = "tail2",
        worker = (
          compatibilityDate = "2024-11-01",
          modules = [
            ( name = "main.js",
              esModule =
                `export default {
                `  async tail(req, env, ctx) {
                `    await fetch("http://tail2/" + req[0].logs.length);
                `  }
                `}
            )
          ],
        )
      ),
    ],
    sockets = [
      ( name = "main",
        address = "test-addr",
        service = "hello"
      )
    ]
  ))"_kj);

  test.start();
  auto conn = test.connect("test-addr");
  conn.sendHttpGet("/");
  conn.recvHttp200("OK");

  auto subreq = test.receiveInternetSubrequest("tail");
  subreq.recv(R"(
    POST / HTTP/1.1
    Content-Length: 23
    Host: tail
    Content-Type: text/plain;charset=UTF-8

    [["foo","bar"],["baz"]])"_blockquote);

  auto subreq2 = test.receiveInternetSubrequest("tail2");
  subreq2.recv(R"(
    GET /2 HTTP/1.1
    Host: tail2

    )"_blockquote);

  subreq.send(R"(
    HTTP/1.1 200 OK
    Content-Length: 0

  )"_blockquote);

  subreq2.send(R"(
    HTTP/1.1 200 OK
    Content-Length: 0

  )"_blockquote);
}

// =======================================================================================
// Test HttpOptions on receive

KJ_TEST("Server: serve proxy requests") {
  TestServer test(R"((
    services = [
      ( name = "hello",
        worker = (
          compatibilityDate = "2022-08-17",
          serviceWorkerScript =
              `addEventListener("fetch", event => {
              `  event.respondWith(new Response("Hello: " + event.request.url + "\n"));
              `})
        )
      )
    ],
    sockets = [
      ( name = "main",
        address = "test-addr",
        service = "hello",
        http = (style = proxy)
      )
    ]
  ))"_kj);

  test.start();

  auto conn = test.connect("test-addr");

  // Send a proxy-style request. No `Host:` header!
  conn.send(R"(
    GET http://foo/bar HTTP/1.1

  )"_blockquote);
  conn.recv(R"(
    HTTP/1.1 200 OK
    Content-Length: 22
    Content-Type: text/plain;charset=UTF-8

    Hello: http://foo/bar
  )"_blockquote);
}

KJ_TEST("Server: forwardedProtoHeader") {
  TestServer test(R"((
    services = [
      ( name = "hello",
        worker = (
          compatibilityDate = "2022-08-17",
          serviceWorkerScript =
              `addEventListener("fetch", event => {
              `  event.respondWith(new Response("Hello: " + event.request.url + "\n"));
              `})
        )
      )
    ],
    sockets = [
      ( name = "main",
        address = "test-addr",
        service = "hello",
        http = (forwardedProtoHeader = "Test-Proto")
      )
    ]
  ))"_kj);

  test.start();

  auto conn = test.connect("test-addr");

  // Send a request with a forwarded proto header.
  conn.send(R"(
    GET /bar HTTP/1.1
    Host: foo
    tEsT-pRoTo: baz

  )"_blockquote);
  conn.recv(R"(
    HTTP/1.1 200 OK
    Content-Length: 21
    Content-Type: text/plain;charset=UTF-8

    Hello: baz://foo/bar
  )"_blockquote);

  // Send a request without one.
  conn.send(R"(
    GET /bar HTTP/1.1
    Host: foo

  )"_blockquote);
  conn.recv(R"(
    HTTP/1.1 200 OK
    Content-Length: 22
    Content-Type: text/plain;charset=UTF-8

    Hello: http://foo/bar
  )"_blockquote);
}

KJ_TEST("Server: cfBlobHeader") {
  TestServer test(R"((
    services = [
      ( name = "hello",
        worker = (
          compatibilityDate = "2022-08-17",
          serviceWorkerScript =
              `addEventListener("fetch", event => {
              `  if (event.request.cf) {
              `    event.respondWith(new Response("cf.foo = " + event.request.cf.foo + "\n"));
              `  } else {
              `    event.respondWith(new Response("cf is null\n"));
              `  }
              `})
        )
      )
    ],
    sockets = [
      ( name = "main",
        address = "test-addr",
        service = "hello",
        http = (cfBlobHeader = "CF-Blob")
      )
    ]
  ))"_kj);

  test.start();

  auto conn = test.connect("test-addr");

  // Send a request with a CF blob.
  conn.send(R"(
    GET / HTTP/1.1
    Host: bar
    cF-bLoB: {"foo": "hello"}

  )"_blockquote);
  conn.recv(R"(
    HTTP/1.1 200 OK
    Content-Length: 15
    Content-Type: text/plain;charset=UTF-8

    cf.foo = hello
  )"_blockquote);

  // Send a request without one
  conn.send(R"(
    GET / HTTP/1.1
    Host: bar

  )"_blockquote);
  conn.recv(R"(
    HTTP/1.1 200 OK
    Content-Length: 11
    Content-Type: text/plain;charset=UTF-8

    cf is null
  )"_blockquote);
}

KJ_TEST("Server: inject headers on incoming request/response") {
  TestServer test(R"((
    services = [
      ( name = "hello",
        worker = (
          compatibilityDate = "2022-08-17",
          serviceWorkerScript =
              `addEventListener("fetch", event => {
              `  let text = [...event.request.headers]
              `      .map(([k,v]) => { return `${k}: ${v}\n` }).join("");
              `  event.respondWith(new Response(text));
              `})
        )
      )
    ],
    sockets = [
      ( name = "main",
        address = "test-addr",
        service = "hello",
        http = (
          injectRequestHeaders = [
            (name = "Foo", value = "oof"),
            (name = "Bar", value = "rab"),
          ],
          injectResponseHeaders = [
            (name = "Baz", value = "zab"),
            (name = "Qux", value = "xuq"),
          ]
        )
      )
    ]
  ))"_kj);

  test.start();

  auto conn = test.connect("test-addr");

  // Send a request, check headers.
  conn.send(R"(
    GET / HTTP/1.1
    Host: example.com

  )"_blockquote);
  conn.recv(R"(
    HTTP/1.1 200 OK
    Content-Length: 36
    Content-Type: text/plain;charset=UTF-8
    Baz: zab
    Qux: xuq

    bar: rab
    foo: oof
    host: example.com
  )"_blockquote);
}

KJ_TEST("Server: drain incoming HTTP connections") {
  TestServer test(singleWorker(R"((
    compatibilityDate = "2022-08-17",
    serviceWorkerScript =
        `addEventListener("fetch", event => {
        `  event.respondWith(new Response("hello"));
        `})
  ))"_kj));

  auto paf = kj::newPromiseAndFulfiller<void>();

  test.start(kj::mv(paf.promise));

  auto conn = test.connect("test-addr");
  auto conn2 = test.connect("test-addr");

  // Send a request on each connection, get a response.
  conn.httpGet200("/", "hello");
  conn2.httpGet200("/", "hello");

  // Send a partial request on conn2.
  conn2.send("GET");

  // No EOF yet.
  KJ_EXPECT(!conn.isEof());
  KJ_EXPECT(!conn2.isEof());

  // Drain the server.
  paf.fulfiller->fulfill();

  // Now we get EOF on conn.
  KJ_EXPECT(conn.isEof());

  // But conn2 is still open.
  KJ_EXPECT(!conn2.isEof());

  // New connections shouldn't be accepted at this point.
  KJ_EXPECT(test.connectHangs("test-addr"));

  // Finish the request on conn2.
  conn2.send(" / HTTP/1.1\nHost: foo\n\n");

  // We receive a response with Connection: close
  conn2.recv(R"(
    HTTP/1.1 200 OK
    Connection: close
    Content-Length: 5
    Content-Type: text/plain;charset=UTF-8

    hello)"_blockquote);

  // And then the connection is, in fact, closed.
  KJ_EXPECT(conn2.isEof());
}

// =======================================================================================
// Test alternate service types
//
// We're going to stop using JavaScript here because it's not really helping. We can directly
// connect a socket to a non-Worker service.

KJ_TEST("Server: network outbound with allow/deny") {
  TestServer test(R"((
    services = [
      (name = "hello", network = (allow = ["foo", "bar"], deny = ["baz", "qux"]))
    ],
    sockets = [
      (name = "main", address = "test-addr", service = "hello")
    ]
  ))"_kj);

  test.start();

  auto conn = test.connect("test-addr");

  conn.sendHttpGet("/path");

  {
    auto subreq = test.receiveSubrequest("foo", {"foo", "bar"}, {"baz", "qux"});
    subreq.recv(R"(
      GET /path HTTP/1.1
      Host: foo

    )"_blockquote);
    subreq.send(R"(
      HTTP/1.1 200 OK
      Content-Length: 2
      Content-Type: text/plain;charset=UTF-8

      OK)"_blockquote);
  }

  conn.recvHttp200("OK");
}

KJ_TEST("Server: external server") {
  TestServer test(R"((
    services = [
      (name = "hello", external = "ext-addr")
    ],
    sockets = [
      (name = "main", address = "test-addr", service = "hello")
    ]
  ))"_kj);

  test.start();

  auto conn = test.connect("test-addr");

  conn.sendHttpGet("/path");

  {
    auto subreq = test.receiveSubrequest("ext-addr");
    subreq.recv(R"(
      GET /path HTTP/1.1
      Host: foo

    )"_blockquote);
    subreq.send(R"(
      HTTP/1.1 200 OK
      Content-Length: 2
      Content-Type: text/plain;charset=UTF-8

      OK)"_blockquote);
  }

  conn.recvHttp200("OK");
}

KJ_TEST("Server: external server proxy style") {
  TestServer test(R"((
    services = [
      (name = "hello", external = (address = "ext-addr", http = (style = proxy)))
    ],
    sockets = [
      (name = "main", address = "test-addr", service = "hello")
    ]
  ))"_kj);

  test.start();

  auto conn = test.connect("test-addr");

  conn.sendHttpGet("/path");

  {
    auto subreq = test.receiveSubrequest("ext-addr");
    subreq.recv(R"(
      GET http://foo/path HTTP/1.1
      Host: foo

    )"_blockquote);
    subreq.send(R"(
      HTTP/1.1 200 OK
      Content-Length: 2
      Content-Type: text/plain;charset=UTF-8

      OK)"_blockquote);
  }

  conn.recvHttp200("OK");
}

KJ_TEST("Server: external server forwarded-proto") {
  TestServer test(R"((
    services = [
      (name = "hello", external = (address = "ext-addr", http = (forwardedProtoHeader = "X-Proto")))
    ],
    sockets = [
      (name = "main", address = "test-addr", service = "hello", http = (style = proxy))
    ]
  ))"_kj);

  test.start();

  auto conn = test.connect("test-addr");

  conn.send(R"(
    GET https://foo/path HTTP/1.1

  )"_blockquote);

  {
    auto subreq = test.receiveSubrequest("ext-addr");
    subreq.recv(R"(
      GET /path HTTP/1.1
      Host: foo
      X-Proto: https

    )"_blockquote);
    subreq.send(R"(
      HTTP/1.1 200 OK
      Content-Length: 2
      Content-Type: text/plain;charset=UTF-8

      OK)"_blockquote);
  }

  conn.recvHttp200("OK");
}

KJ_TEST("Server: external server inject headers") {
  TestServer test(R"((
    services = [
      ( name = "hello",
        external = (
          address = "ext-addr",
          http = (
            injectRequestHeaders = [
              (name = "Foo", value = "oof"),
              (name = "Bar", value = "rab"),
            ],
            injectResponseHeaders = [
              (name = "Baz", value = "zab"),
              (name = "Qux", value = "xuq"),
            ]
          )
        )
      )
    ],
    sockets = [
      (name = "main", address = "test-addr", service = "hello")
    ]
  ))"_kj);

  test.start();

  auto conn = test.connect("test-addr");

  conn.sendHttpGet("/path");

  {
    auto subreq = test.receiveSubrequest("ext-addr");
    subreq.recv(R"(
      GET /path HTTP/1.1
      Host: foo
      Foo: oof
      Bar: rab

    )"_blockquote);
    subreq.send(R"(
      HTTP/1.1 200 OK
      Content-Length: 2
      Content-Type: text/plain;charset=UTF-8

      OK)"_blockquote);
  }

  conn.recv(R"(
    HTTP/1.1 200 OK
    Content-Length: 2
    Content-Type: text/plain;charset=UTF-8
    Baz: zab
    Qux: xuq

    OK)"_blockquote);
}

KJ_TEST("Server: external server cf blob header") {
  TestServer test(R"((
    services = [
      ( name = "hello",
        worker = (
          compatibilityDate = "2022-08-17",
          modules = [
            ( name = "main.js",
              esModule =
                `export default {
                `  async fetch(request, env) {
                `    return env.ext.fetch("http://ext/path2", {cf: {hello: "world"}});
                `  }
                `}
            )
          ],
          bindings = [(name = "ext", service = "ext")]
        )
      ),
      (name = "ext", external = (address = "ext-addr", http = (cfBlobHeader = "CF-Blob")))
    ],
    sockets = [
      (name = "main", address = "test-addr", service = "hello")
    ]
  ))"_kj);

  test.start();

  auto conn = test.connect("test-addr");

  conn.sendHttpGet("/path");

  {
    auto subreq = test.receiveSubrequest("ext-addr");
    subreq.recv(R"(
      GET /path2 HTTP/1.1
      Host: ext
      CF-Blob: {"hello":"world"}

    )"_blockquote);
    subreq.send(R"(
      HTTP/1.1 200 OK
      Content-Length: 2
      Content-Type: text/plain;charset=UTF-8

      OK)"_blockquote);
  }

  conn.recv(R"(
    HTTP/1.1 200 OK
    Content-Length: 2
    Content-Type: text/plain;charset=UTF-8

    OK)"_blockquote);
}

KJ_TEST("Server: disk service") {
  TestServer test(R"((
    services = [
      (name = "hello", disk = "../../frob/blah")
    ],
    sockets = [
      (name = "main", address = "test-addr", service = "hello")
    ]
  ))"_kj);

  auto mode = kj::WriteMode::CREATE | kj::WriteMode::CREATE_PARENT;
  auto dir = test.root->openSubdir(kj::Path({"frob"_kj, "blah"_kj}), mode);
  test.fakeDate =
      kj::UNIX_EPOCH + 2 * kj::DAYS + 5 * kj::HOURS + 18 * kj::MINUTES + 23 * kj::SECONDS;
  dir->openFile(kj::Path({"foo.txt"}), mode)->writeAll("hello from foo.txt\n");
  dir->openFile(kj::Path({"numbers.txt"}), mode)->writeAll("0123456789\n");
  test.fakeDate = kj::UNIX_EPOCH + 400 * kj::DAYS + 2 * kj::HOURS + 52 * kj::MINUTES +
      9 * kj::SECONDS + 163 * kj::MILLISECONDS;
  dir->openFile(kj::Path({"bar.txt"}), mode)->writeAll("hello from bar.txt\n");
  test.fakeDate = kj::UNIX_EPOCH;
  dir->openFile(kj::Path({"baz", "qux.txt"}), mode)->writeAll("hello from qux.txt\n");
  dir->openFile(kj::Path({".dot"}), mode)->writeAll("this is a dotfile\n");
  dir->openFile(kj::Path({".dotdir", "foo"}), mode)->writeAll("this is a dotfile\n");

  test.start();

  auto conn = test.connect("test-addr");

  conn.sendHttpGet("/foo.txt");
  conn.recv(R"(
    HTTP/1.1 200 OK
    Content-Length: 19
    Content-Type: application/octet-stream
    Last-Modified: Sat, 03 Jan 1970 05:18:23 GMT

    hello from foo.txt
  )"_blockquote);

  conn.sendHttpGet("/bar.txt");
  conn.recv(R"(
    HTTP/1.1 200 OK
    Content-Length: 19
    Content-Type: application/octet-stream
    Last-Modified: Fri, 05 Feb 1971 02:52:09 GMT

    hello from bar.txt
  )"_blockquote);

  conn.sendHttpGet("/baz/qux.txt");
  conn.recv(R"(
    HTTP/1.1 200 OK
    Content-Length: 19
    Content-Type: application/octet-stream
    Last-Modified: Thu, 01 Jan 1970 00:00:00 GMT

    hello from qux.txt
  )"_blockquote);

  // TODO(beta): Test listing a directory. Unfortunately it doesn't work against the in-memory
  //   filesystem right now.
  //
  // conn.sendHttpGet("/");

  // HEAD returns no content.
  conn.send(R"(
    HEAD /numbers.txt HTTP/1.1
    Host: foo

  )"_blockquote);
  conn.recv(R"(
    HTTP/1.1 200 OK
    Content-Length: 11
    Content-Type: application/octet-stream
    Last-Modified: Sat, 03 Jan 1970 05:18:23 GMT

  )"_blockquote);

  // GET with single range returns partial content.
  conn.send(R"(
    GET /numbers.txt HTTP/1.1
    Host: foo
    Range: bytes=3-5

  )"_blockquote);
  conn.recv(R"(
    HTTP/1.1 206 Partial Content
    Content-Length: 3
    Content-Type: application/octet-stream
    Content-Range: bytes 3-5/11
    Last-Modified: Sat, 03 Jan 1970 05:18:23 GMT

    345)"_blockquote);

  // GET with single covering range returns full content.
  conn.send(R"(
    GET /numbers.txt HTTP/1.1
    Host: foo
    Range: bytes=-50

  )"_blockquote);
  conn.recv(R"(
    HTTP/1.1 200 OK
    Content-Length: 11
    Content-Type: application/octet-stream
    Last-Modified: Sat, 03 Jan 1970 05:18:23 GMT

    0123456789
  )"_blockquote);

  // GET with many ranges returns full content.
  conn.send(R"(
    GET /numbers.txt HTTP/1.1
    Host: foo
    Range: bytes=1-3, 6-8

  )"_blockquote);
  conn.recv(R"(
    HTTP/1.1 200 OK
    Content-Length: 11
    Content-Type: application/octet-stream
    Last-Modified: Sat, 03 Jan 1970 05:18:23 GMT

    0123456789
  )"_blockquote);

  // GET with unsatisfiable range.
  conn.send(R"(
    GET /numbers.txt HTTP/1.1
    Host: foo
    Range: bytes=20-30

  )"_blockquote);
  conn.recv(R"(
    HTTP/1.1 416 Range Not Satisfiable
    Content-Length: 21
    Content-Range: bytes */11

    Range Not Satisfiable)"_blockquote);

  // File not found...
  conn.sendHttpGet("/no-such-file.txt");
  conn.recv(R"(
    HTTP/1.1 404 Not Found
    Content-Length: 9

    Not Found)"_blockquote);

  // Directory not found...
  conn.sendHttpGet("/no-such-dir/file.txt");
  conn.recv(R"(
    HTTP/1.1 404 Not Found
    Content-Length: 9

    Not Found)"_blockquote);

  // PUT is denied because not writable.
  conn.send(R"(
    PUT /corge.txt HTTP/1.1
    Host: foo
    Content-Length: 6

    corge
  )"_blockquote);
  conn.recv(R"(
    HTTP/1.1 405 Method Not Allowed
    Content-Length: 18

    Method Not Allowed)"_blockquote);

  // DELETE is denied because not writable.
  conn.send(R"(
    DELETE /corge.txt HTTP/1.1
    Host: foo

  )"_blockquote);
  conn.recv(R"(
    HTTP/1.1 405 Method Not Allowed
    Content-Length: 18

    Method Not Allowed)"_blockquote);

  // POST is denied because invalid method.
  conn.send(R"(
    POST /corge.txt HTTP/1.1
    Host: foo
    Content-Length: 6

    corge
  )"_blockquote);
  conn.recv(R"(
    HTTP/1.1 501 Not Implemented
    Content-Length: 15

    Not Implemented)"_blockquote);

  // Dotfile access is denied.
  conn.sendHttpGet("/.dot");
  conn.recv(R"(
    HTTP/1.1 404 Not Found
    Content-Length: 9

    Not Found)"_blockquote);

  // Dotfile directory access is denied.
  conn.sendHttpGet("/.dotdir/foo");
  conn.recv(R"(
    HTTP/1.1 404 Not Found
    Content-Length: 9

    Not Found)"_blockquote);
}

KJ_TEST("Server: disk service writable") {
  TestServer test(R"((
    services = [
      (name = "hello", disk = (path = "../../frob/blah", writable = true))
    ],
    sockets = [
      (name = "main", address = "test-addr", service = "hello")
    ]
  ))"_kj);

  auto mode = kj::WriteMode::CREATE | kj::WriteMode::CREATE_PARENT;
  auto dir = test.root->openSubdir(kj::Path({"frob"_kj, "blah"_kj}), mode);
  dir->openFile(kj::Path({"existing.txt"}), mode)->writeAll("replace me!");

  test.start();

  auto conn = test.connect("test-addr");

  // Write a file.
  conn.send(R"(
    PUT /newfile.txt HTTP/1.1
    Host: foo
    Content-Length: 6

    corge
  )"_blockquote);
  conn.recv(R"(
    HTTP/1.1 204 No Content

    )"_blockquote);

  // Read it back.
  KJ_EXPECT(dir->openFile(kj::Path({"newfile.txt"}))->readAllText() == "corge\n");

  // Delete it.
  conn.send(R"(
    DELETE /newfile.txt HTTP/1.1
    Host: foo

  )"_blockquote);
  conn.recv(R"(
    HTTP/1.1 204 No Content

    )"_blockquote);
  KJ_EXPECT(!dir->exists(kj::Path({"newfile.txt"})));

  // Delete a non-existent file.
  conn.send(R"(
    DELETE /notfound.txt HTTP/1.1
    Host: foo

  )"_blockquote);
  conn.recv(R"(
    HTTP/1.1 404 Not Found
    Content-Length: 9

    Not Found)"_blockquote);

  // Replace a file.
  conn.send(R"(
    PUT /existing.txt HTTP/1.1
    Host: foo
    Content-Length: 7

    grault
  )"_blockquote);
  conn.recv(R"(
    HTTP/1.1 204 No Content

    )"_blockquote);

  // Read it back.
  KJ_EXPECT(dir->openFile(kj::Path({"existing.txt"}))->readAllText() == "grault\n");

  // Write a file to a new directory.
  conn.send(R"(
    PUT /newdir/newfile.txt HTTP/1.1
    Host: foo
    Content-Length: 7

    garply
  )"_blockquote);
  conn.recv(R"(
    HTTP/1.1 204 No Content

    )"_blockquote);

  // Read it back.
  KJ_EXPECT(dir->openFile(kj::Path({"newdir", "newfile.txt"}))->readAllText() == "garply\n");

  // Delete the new directory.
  conn.send(R"(
    DELETE /newdir/ HTTP/1.1
    Host: foo

  )"_blockquote);
  conn.recv(R"(
    HTTP/1.1 204 No Content

    )"_blockquote);
  KJ_EXPECT(!dir->exists(kj::Path({"newdir"})));

  // POST is denied because invalid method.
  conn.send(R"(
    POST /corge.txt HTTP/1.1
    Host: foo
    Content-Length: 6

    waldo
  )"_blockquote);
  conn.recv(R"(
    HTTP/1.1 501 Not Implemented
    Content-Length: 15

    Not Implemented)"_blockquote);

  // Dotfile write access is denied.
  conn.send(R"(
    PUT /.dot HTTP/1.1
    Host: foo
    Content-Length: 6

    waldo
  )"_blockquote);
  conn.recv(R"(
    HTTP/1.1 403 Unauthorized
    Content-Length: 12

    Unauthorized)"_blockquote);

  // Dotfile directory write access is denied.
  conn.send(R"(
    PUT /.dotdir/foo HTTP/1.1
    Host: foo
    Content-Length: 6

    waldo
  )"_blockquote);
  conn.recv(R"(
    HTTP/1.1 403 Unauthorized
    Content-Length: 12

    Unauthorized)"_blockquote);

  // Dotfile delete access is denied.
  conn.send(R"(
    DELETE /.dot HTTP/1.1
    Host: foo

  )"_blockquote);
  conn.recv(R"(
    HTTP/1.1 403 Unauthorized
    Content-Length: 12

    Unauthorized)"_blockquote);

  // Root write is denied.
  conn.send(R"(
    PUT / HTTP/1.1
    Host: foo
    Content-Length: 6

    corge
  )"_blockquote);
  conn.recv(R"(
    HTTP/1.1 403 Unauthorized
    Content-Length: 12

    Unauthorized)"_blockquote);

  // Root delete is denied.
  conn.send(R"(
    DELETE / HTTP/1.1
    Host: foo

  )"_blockquote);
  conn.recv(R"(
    HTTP/1.1 403 Unauthorized
    Content-Length: 12

    Unauthorized)"_blockquote);
}

KJ_TEST("Server: disk service allow dotfiles") {
  TestServer test(R"((
    services = [
      (name = "hello", disk = (path = "../../frob", writable = true, allowDotfiles = true))
    ],
    sockets = [
      (name = "main", address = "test-addr", service = "hello")
    ]
  ))"_kj);

  auto mode = kj::WriteMode::CREATE | kj::WriteMode::CREATE_PARENT;
  auto dir = test.root->openSubdir(kj::Path({"frob"_kj}), mode);

  // Put a file at root that shouldn't be accessible.
  test.root->openFile(kj::Path({"secret"}), mode)->writeAll("this is super-secret");

  test.start();

  auto conn = test.connect("test-addr");

  conn.send(R"(
    PUT /.dot HTTP/1.1
    Host: foo
    Content-Length: 6

    waldo
  )"_blockquote);
  conn.recv(R"(
    HTTP/1.1 204 No Content

    )"_blockquote);

  KJ_EXPECT(dir->openFile(kj::Path({".dot"}))->readAllText() == "waldo\n");

  conn.sendHttpGet("/.dot");
  conn.recv(R"(
    HTTP/1.1 200 OK
    Content-Length: 6
    Content-Type: application/octet-stream
    Last-Modified: Thu, 01 Jan 1970 00:00:00 GMT

    waldo
  )"_blockquote);

  conn.sendHttpGet("/../secret");
  conn.recv(R"(
    HTTP/1.1 404 Not Found
    Content-Length: 9

    Not Found)"_blockquote);
  conn.sendHttpGet("/%2e%2e/secret");
  conn.recv(R"(
    HTTP/1.1 404 Not Found
    Content-Length: 9

    Not Found)"_blockquote);

  conn.send(R"(
    PUT /../secret HTTP/1.1
    Host: foo
    Content-Length: 5

    evil
  )"_blockquote);
  conn.recv(R"(
    HTTP/1.1 204 No Content

    )"_blockquote);
  // This actually wrote to /secret, because URL parsing simply ignores leading "../".
  KJ_EXPECT(dir->openFile(kj::Path({"secret"}))->readAllText() == "evil\n");
  KJ_EXPECT(test.root->openFile(kj::Path({"secret"}))->readAllText() == "this is super-secret");

  conn.send(R"(
    PUT /%2e%2e/secret HTTP/1.1
    Host: foo
    Content-Length: 5

    evil
  )"_blockquote);
  conn.recv(R"(
    HTTP/1.1 403 Unauthorized
    Content-Length: 12

    Unauthorized)"_blockquote);
  // This didn't work.
  KJ_EXPECT(test.root->openFile(kj::Path({"secret"}))->readAllText() == "this is super-secret");
}

// =======================================================================================
// Test Cache API

KJ_TEST("Server: If no cache service is defined, access to the cache API should error") {
  TestServer test(singleWorker(R"((
    compatibilityDate = "2022-08-17",
    modules = [
      ( name = "test.js",
        esModule =
          `export default {
          `  async fetch(request) {
          `    try {
          `      return new Response(await caches.default.match(request))
          `    } catch (e) {return new Response(e.message)}
          `
          `  }
          `}
      )
    ]
  ))"_kj));

  test.start();
  auto conn = test.connect("test-addr");
  conn.httpGet200("/", "No Cache was configured");
}

KJ_TEST("Server: cached response") {
  TestServer test(R"((
    services = [
      ( name = "hello",
        worker = (
          cacheApiOutbound = "cache-outbound",
          compatibilityDate = "2022-08-17",
          modules = [
            ( name = "main.js",
              esModule =
                `export default {
                `  async fetch(request, env, ctx) {
                `    const cache = caches.default;
                `    let response = await cache.match(request);
                `    return response ?? new Response('not cached');
                `  }
                `}
            )
          ]
        )
      ),
      ( name = "cache-outbound", external = "cache-host" ),
    ],
    sockets = [
      ( name = "main",
        address = "test-addr",
        service = "hello"
      )
    ]
  ))"_kj);

  test.start();
  auto conn = test.connect("test-addr");
  conn.sendHttpGet("/");

  {
    auto subreq = test.receiveSubrequest("cache-host");
    subreq.recv(R"(
      GET / HTTP/1.1
      Host: foo
      Cache-Control: only-if-cached

    )"_blockquote);
    subreq.send(R"(
      HTTP/1.1 200 OK
      CF-Cache-Status: HIT
      Content-Length: 6

      cached)"_blockquote);
  }

  conn.recv(R"(
    HTTP/1.1 200 OK
    Content-Length: 6
    CF-Cache-Status: HIT

    cached)"_blockquote);
}

KJ_TEST("Server: cache name is passed through to service") {
  TestServer test(R"((
    services = [
      ( name = "hello",
        worker = (
          cacheApiOutbound = "cache-outbound",
          compatibilityDate = "2022-08-17",
          modules = [
            ( name = "main.js",
              esModule =
                `export default {
                `  async fetch(request, env, ctx) {
                `    const cache = await caches.open('test-cache');
                `    let response = await cache.match(request);
                `    return response ?? new Response('not cached');
                `  }
                `}
            )
          ]
        )
      ),
      ( name = "cache-outbound", external = "cache-host" ),
    ],
    sockets = [
      ( name = "main",
        address = "test-addr",
        service = "hello"
      )
    ]
  ))"_kj);

  test.start();
  auto conn = test.connect("test-addr");
  conn.sendHttpGet("/");

  {
    auto subreq = test.receiveSubrequest("cache-host");
    subreq.recv(R"(
      GET / HTTP/1.1
      Host: foo
      Cache-Control: only-if-cached
      CF-Cache-Namespace: test-cache

    )"_blockquote);
    subreq.send(R"(
      HTTP/1.1 200 OK
      CF-Cache-Status: HIT
      Content-Length: 6

      cached)"_blockquote);
  }

  conn.recv(R"(
    HTTP/1.1 200 OK
    Content-Length: 6
    CF-Cache-Status: HIT

    cached)"_blockquote);
}

// =======================================================================================
// Test the test command

KJ_TEST("Server: cache name is passed through to service") {
  kj::StringPtr config = R"((
    services = [
      ( name = "hello",
        worker = (
          compatibilityDate = "2022-08-17",
          modules = [
            ( name = "main.js",
              esModule =
                `export default {
                `  async test(controller, env, ctx) {}
                `}
                `export let fail = {
                `  async test(controller, env, ctx) {
                `    throw new Error("ded");
                `  }
                `}
                `export let nonTest = {
                `  async fetch(req, env, ctx) {
                `    return new Response("ok");
                `  }
                `}
            )
          ]
        )
      ),
      ( name = "another",
        worker = (
          compatibilityDate = "2022-08-17",
          modules = [
            ( name = "main.js",
              esModule =
                `export default {
                `  async test(controller, env, ctx) {
                `    console.log(env.MESSAGE);
                `  }
                `}
            )
          ],
          bindings = [
            ( name = "MESSAGE", text = "other test" ),
          ]
        )
      ),
    ],
    sockets = [
      ( name = "main",
        address = "test-addr",
        service = "hello"
      )
    ]
  ))"_kj;

  {
    TestServer test(config);
    KJ_EXPECT_LOG(DBG, "[ TEST ] hello");
    KJ_EXPECT_LOG(DBG, "[ PASS ] hello");
    KJ_EXPECT(test.server.test(v8System, *test.config, "hello", "default").wait(test.ws));
  }

  {
    TestServer test(config);
    KJ_EXPECT_LOG(DBG, "[ TEST ] hello:fail");
    KJ_EXPECT_LOG(INFO, "Error: ded");
    KJ_EXPECT_LOG(DBG, "[ FAIL ] hello:fail");
    KJ_EXPECT(!test.server.test(v8System, *test.config, "hello", "fail").wait(test.ws));
  }

  {
    TestServer test(config);
    KJ_EXPECT_LOG(DBG, "[ TEST ] hello");
    KJ_EXPECT_LOG(DBG, "[ PASS ] hello");
    KJ_EXPECT_LOG(DBG, "[ TEST ] hello:fail");
    KJ_EXPECT_LOG(INFO, "Error: ded");
    KJ_EXPECT_LOG(DBG, "[ FAIL ] hello:fail");
    KJ_EXPECT(!test.server.test(v8System, *test.config, "hello", "*").wait(test.ws));
  }

  {
    TestServer test(config);
    KJ_EXPECT_LOG(DBG, "[ TEST ] hello");
    KJ_EXPECT_LOG(DBG, "[ PASS ] hello");
    KJ_EXPECT_LOG(DBG, "[ TEST ] another");
    KJ_EXPECT_LOG(INFO, "other test");
    KJ_EXPECT_LOG(DBG, "[ PASS ] another");
    KJ_EXPECT(test.server.test(v8System, *test.config, "*", "default").wait(test.ws));
  }

  {
    TestServer test(config);
    KJ_EXPECT_LOG(DBG, "[ TEST ] hello");
    KJ_EXPECT_LOG(DBG, "[ PASS ] hello");
    KJ_EXPECT_LOG(DBG, "[ TEST ] hello:fail");
    KJ_EXPECT_LOG(INFO, "Error: ded");
    KJ_EXPECT_LOG(DBG, "[ FAIL ] hello:fail");
    KJ_EXPECT_LOG(DBG, "[ TEST ] another");
    KJ_EXPECT_LOG(INFO, "other test");
    KJ_EXPECT_LOG(DBG, "[ PASS ] another");
    KJ_EXPECT(!test.server.test(v8System, *test.config, "*", "*").wait(test.ws));
  }
}

// =======================================================================================

KJ_TEST("Server: JS RPC over HTTP connections") {
  // Test that we can send RPC over an ExternalServer pointing back to our own loopback socket,
  // as long as both are configured with a `capnpConnectHost`.

  TestServer test(R"((
    services = [
      ( name = "hello",
        worker = (
          compatibilityDate = "2024-02-23",
          compatibilityFlags = ["experimental"],
          modules = [
            ( name = "main.js",
              esModule =
                `import {WorkerEntrypoint} from "cloudflare:workers";
                `export default {
                `  async fetch(request, env) {
                `    return new Response("got: " + await env.OUT.frob(3, 11));
                `  }
                `}
                `export class MyRpc extends WorkerEntrypoint {
                `  async frob(a, b) { return a * b + 2; }
                `}
            )
          ],
          bindings = [( name = "OUT", service = "outbound")]
        )
      ),
      (name = "outbound", external = (address = "loopback", http = (capnpConnectHost = "cappy")))
    ],
    sockets = [
      ( name = "main", address = "test-addr", service = "hello" ),
      ( name = "alt1", address = "loopback",
        service = (name = "hello", entrypoint = "MyRpc"),
        http = (capnpConnectHost = "cappy")),
    ]
  ))"_kj);

  test.server.allowExperimental();
  test.start();

  auto conn = test.connect("test-addr");
  conn.httpGet200("/", "got: 35");
}

KJ_TEST("Server: Entrypoint binding with props") {
  TestServer test(R"((
    services = [
      ( name = "hello",
        worker = (
          compatibilityDate = "2024-02-23",
          compatibilityFlags = ["experimental"],
          modules = [
            ( name = "main.js",
              esModule =
                `import {WorkerEntrypoint} from "cloudflare:workers";
                `export default {
                `  async fetch(request, env) {
                `    return new Response("got: " + await env.MyRpc.getProps());
                `  }
                `}
                `export class MyRpc extends WorkerEntrypoint {
                `  getProps() { return this.ctx.props.foo; }
                `}
            )
          ],
          bindings = [
            ( name = "MyRpc",
              service = (
                name = "hello",
                entrypoint = "MyRpc",
                props = (
                  json = `{"foo": 123}
                )
              )
            )
          ]
        )
      ),
    ],
    sockets = [
      ( name = "main", address = "test-addr", service = "hello" ),
    ]
  ))"_kj);

  test.server.allowExperimental();
  test.start();

  auto conn = test.connect("test-addr");
  conn.httpGet200("/", "got: 123");
}

// =======================================================================================

// TODO(beta): Test TLS (send and receive)
// TODO(beta): Test CLI overrides

}  // namespace
}  // namespace workerd::server
