// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "server.h"
#include <kj/test.h>
#include <workerd/util/capnp-mock.h>
#include <capnp/serialize-text.h>
#include <workerd/jsg/setup.h>
#include <kj/async-queue.h>

namespace workerd::server {
namespace {

#define KJ_FAIL_EXPECT_AT(location, ...) \
  KJ_LOG_AT(ERROR, location, ##__VA_ARGS__);
#define KJ_EXPECT_AT(cond, location, ...) \
  if (auto _kjCondition = ::kj::_::MAGIC_ASSERT << cond); \
  else KJ_FAIL_EXPECT_AT(location, "failed: expected " #cond, _kjCondition, ##__VA_ARGS__)

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
  KJ_IF_MAYBE(exception, kj::runCatchingExceptions([&]() {
    TEXT_CODEC.decode(text, root);
  })) {
    KJ_FAIL_REQUIRE_AT(loc, *exception);
  }

  return capnp::clone(root.asReader());
}

kj::String operator "" _blockquote(const char* str, size_t n) {
  // Accept an indented block of text and remove the indentation. From each line of text, this will
  // remove a number of spaces up to the indentation of the first line.
  //
  // This is intended to allow multi-line raw text to be specified conveniently using C++11
  // `R"(blah)"` literal syntax, without the need to mess up indentation relative to the
  // surrounding code.

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
    result.addAll(text.slice(0, nl));
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
      : ws(ws), stream(kj::mv(stream)) {}

  void send(kj::StringPtr data, kj::SourceLocation loc = {}) {
    stream->write(data.begin(), data.size()).wait(ws);
  }
  void recv(kj::StringPtr expected, kj::SourceLocation loc = {}) {
    auto actual = readAllAvailable();
    if (actual == nullptr) {
      KJ_FAIL_EXPECT_AT(loc, "message never received");
    } else {
      KJ_EXPECT_AT(actual == expected, loc);
    }
  }

  void sendHttpGet(kj::StringPtr path, kj::SourceLocation loc = {}) {
    send(kj::str(
        "GET ", path, " HTTP/1.1\n"
        "Host: foo\n"
        "\n"), loc);
  }

  void recvHttp200(kj::StringPtr expectedResponse, kj::SourceLocation loc = {}) {
    recv(kj::str(
        "HTTP/1.1 200 OK\n"
        "Content-Length: ", expectedResponse.size(), "\n"
        "Content-Type: text/plain;charset=UTF-8\n"
        "\n",
        expectedResponse), loc);
  }

  void httpGet200(kj::StringPtr path, kj::StringPtr expectedResponse, kj::SourceLocation loc = {}) {
    sendHttpGet(path, loc);
    recvHttp200(expectedResponse, loc);
  }

  bool isEof() {
    // Return true if the stream is at EOF.

    if (premature != nullptr) {
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

private:
  kj::WaitScope& ws;
  kj::Own<kj::AsyncIoStream> stream;

  kj::Maybe<char> premature;
  // isEof() may prematurely read a character. Keep it off to the side for the next actual read.

  kj::String readAllAvailable() {
    kj::Vector<char> buffer(256);
    KJ_IF_MAYBE(p, premature) {
      buffer.add(*p);
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
        server(*this, timer, mockNetwork, *this, [this](kj::String error) {
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
      KJ_IF_MAYBE(t, runTask) {
        t->poll(ws);
      }
    }
  }

  void start(kj::Promise<void> drainWhen = kj::NEVER_DONE) {
    // Start the server. Call before connect().

    KJ_REQUIRE(runTask == nullptr);
    auto task = server.run(v8System, *config, kj::mv(drainWhen))
        .eagerlyEvaluate([](kj::Exception&& e) {
      KJ_FAIL_EXPECT(e);
    });
    KJ_EXPECT(!task.poll(ws));
    runTask = kj::mv(task);
  }

  void expectErrors(kj::StringPtr expected) {
    // Call instead of `start()` when the config is expected to produce errors. The parameter is
    // the expected list of errors messages, one per line.

    expectedErrors = expected;
    server.run(v8System, *config).poll(ws);
    KJ_EXPECT(expectedErrors == nullptr, "some expected errors weren't seen");
  }

  TestStream connect(kj::StringPtr addr) {
    // Connect to the server on the given address. The string just has to match what is in the
    // config; the actual connection is in-memory with no network involved.

    return TestStream(ws, KJ_REQUIRE_NONNULL(sockets.find(addr), addr)->connect().wait(ws));
  }

  TestStream receiveSubrequest(kj::StringPtr addr,
      kj::ArrayPtr<const kj::StringPtr> allowedPeers = nullptr,
      kj::ArrayPtr<const kj::StringPtr> deniedPeers = nullptr,
      kj::SourceLocation loc = {}) {
    // Expect an incoming connection on the given address and from a network with the given
    // allowed / denied peer list.

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

  TestStream receiveInternetSubrequest(kj::StringPtr addr,
      kj::SourceLocation loc = {}) {
    return receiveSubrequest(addr, {"public"_kj}, {}, loc);
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
  // implements Filesytem

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

  kj::HashMap<kj::String, kj::Own<kj::NetworkAddress>> sockets;
  // Addresses that the server is listening on.

  class MockNetwork;

  struct SubrequestInfo {
    kj::Own<kj::PromiseFulfiller<kj::Own<kj::AsyncIoStream>>> fulfiller;
    kj::StringPtr peerFilter;
  };
  using SubrequestQueue = kj::ProducerConsumerQueue<SubrequestInfo>;
  kj::HashMap<kj::String, kj::Own<SubrequestQueue>> subrequests;
  // Expected incoming connections and callbacks that should be used to handle them.

  SubrequestQueue& getSubrequestQueue(kj::StringPtr addr) {
    return *subrequests.findOrCreate(addr, [&]() -> decltype(subrequests)::Entry {
      return { kj::str(addr), kj::heap<SubrequestQueue>() };
    });
  }

  static kj::String peerFilterToString(kj::ArrayPtr<const kj::StringPtr> allow,
                                       kj::ArrayPtr<const kj::StringPtr> deny) {
    if (allow == nullptr && deny == nullptr) {
      return kj::str("(none)");
    } else {
      return kj::str(
          "allow: [", kj::strArray(allow, ", "), "], "
          "deny: [", kj::strArray(deny, ", "), "]");
    }
  }

  class MockAddress final: public kj::NetworkAddress {
  public:
    MockAddress(TestServer& test, kj::StringPtr peerFilter, kj::String address)
        : test(test), peerFilter(peerFilter), address(kj::mv(address)) {}

    kj::Promise<kj::Own<kj::AsyncIoStream>> connect() override {
      auto [promise, fulfiller] = kj::newPromiseAndFulfiller<kj::Own<kj::AsyncIoStream>>();

      test.getSubrequestQueue(address).push({
        kj::mv(fulfiller), peerFilter
      });

      return kj::mv(promise);
    }
    kj::Own<kj::ConnectionReceiver> listen() override {
      auto pipe = kj::newCapabilityPipe();
      auto receiver = kj::heap<kj::CapabilityStreamConnectionReceiver>(*pipe.ends[0])
          .attach(kj::mv(pipe.ends[0]));
      auto sender = kj::heap<kj::CapabilityStreamNetworkAddress>(nullptr, *pipe.ends[1])
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
        kj::ArrayPtr<const kj::StringPtr> allow,
        kj::ArrayPtr<const kj::StringPtr> deny) override {
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
    memset(buffer.begin(), random, buffer.size());
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
        worker = )"_kj, def, R"(
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
      )", compatProperties, R"(,
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
    service hello: Worker must specify compatibiltyDate.
  )"_blockquote);
}

KJ_TEST("Server: value bindings") {
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
      )
    ]
  ))"_kj));

  test.start();
  auto conn = test.connect("test-addr");
  conn.httpGet200("/",
      "Hello from text binding\n"
      "Hello from data binding\n"
      "wasm says square(5) = 25\n"
      "Hello from json binding");
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
            )
          ]
        )
      ),
      ( name = "service-outbound", external = "service-host" ),
      ( name = "kv-outbound", external = "kv-host" ),
      ( name = "r2-outbound", external = "r2-host" ),
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

  conn.recvHttp200(R"(
    Hello from HTTP
    Hello from KV
    Hello from R2
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
            )
          ]
        )
      ),
    ],
    sockets = [
      ( name = "main", address = "test-addr", service = "hello" ),
      ( name = "alt1", address = "foo-addr", service = (name = "hello", entrypoint = "foo")),
      ( name = "alt2", address = "bar-addr", service = (name = "hello", entrypoint = "bar"))
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
  conn.httpGet200("/",
      "59002eb8cf872e541722977a258a12d6a93bbe8192b502e1c0cb250aa91af234: http://foo/ 0");
  conn.httpGet200("/",
      "59002eb8cf872e541722977a258a12d6a93bbe8192b502e1c0cb250aa91af234: http://foo/ 1");
  conn.httpGet200("/",
      "59002eb8cf872e541722977a258a12d6a93bbe8192b502e1c0cb250aa91af234: http://foo/ 2");
  conn.httpGet200("/bar",
      "02b496f65dd35cbac90e3e72dc5a398ee93926ea4a3821e26677082d2e6f9b79: http://foo/bar 0");
  conn.httpGet200("/bar",
      "02b496f65dd35cbac90e3e72dc5a398ee93926ea4a3821e26677082d2e6f9b79: http://foo/bar 1");
  conn.httpGet200("/",
      "59002eb8cf872e541722977a258a12d6a93bbe8192b502e1c0cb250aa91af234: http://foo/ 3");
  conn.httpGet200("/bar",
      "02b496f65dd35cbac90e3e72dc5a398ee93926ea4a3821e26677082d2e6f9b79: http://foo/bar 2");
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
    test.root->transfer(
        kj::Path({"var"_kj, "do-storage"_kj}), kj::WriteMode::CREATE | kj::WriteMode::CREATE_PARENT,
        *dir, nullptr, kj::TransferMode::LINK);

    test.start();
    auto conn = test.connect("test-addr");
    conn.httpGet200("/",
        "59002eb8cf872e541722977a258a12d6a93bbe8192b502e1c0cb250aa91af234: http://foo/ 0");
    conn.httpGet200("/",
        "59002eb8cf872e541722977a258a12d6a93bbe8192b502e1c0cb250aa91af234: http://foo/ 1");
    conn.httpGet200("/",
        "59002eb8cf872e541722977a258a12d6a93bbe8192b502e1c0cb250aa91af234: http://foo/ 2");
    conn.httpGet200("/bar",
        "02b496f65dd35cbac90e3e72dc5a398ee93926ea4a3821e26677082d2e6f9b79: http://foo/bar 0");
    conn.httpGet200("/bar",
        "02b496f65dd35cbac90e3e72dc5a398ee93926ea4a3821e26677082d2e6f9b79: http://foo/bar 1");
    conn.httpGet200("/",
        "59002eb8cf872e541722977a258a12d6a93bbe8192b502e1c0cb250aa91af234: http://foo/ 3");
    conn.httpGet200("/bar",
        "02b496f65dd35cbac90e3e72dc5a398ee93926ea4a3821e26677082d2e6f9b79: http://foo/bar 2");

    // The storage directory contains .sqlite and .sqlite-wal files for both objects. Note that
    // the `-shm` files are missing because SQLite doesn't actually tell the VFS to create these
    // as separate files, it leaves it up to the VFS to decide how shared memory works, and our
    // KJ-wrapping VFS currently doesn't put this in SHM files. If we were using a real disk
    // directory, though, they would be there.
    KJ_EXPECT(dir->openSubdir(kj::Path({"mykey"}))->listNames().size() == 4);
    KJ_EXPECT(dir->exists(kj::Path({"mykey",
      "02b496f65dd35cbac90e3e72dc5a398ee93926ea4a3821e26677082d2e6f9b79.sqlite"})));
    KJ_EXPECT(dir->exists(kj::Path({"mykey",
      "02b496f65dd35cbac90e3e72dc5a398ee93926ea4a3821e26677082d2e6f9b79.sqlite-wal"})));
    KJ_EXPECT(dir->exists(kj::Path({"mykey",
      "59002eb8cf872e541722977a258a12d6a93bbe8192b502e1c0cb250aa91af234.sqlite"})));
    KJ_EXPECT(dir->exists(kj::Path({"mykey",
      "59002eb8cf872e541722977a258a12d6a93bbe8192b502e1c0cb250aa91af234.sqlite-wal"})));
  }

  // Having torn everything down, the WAL files should be gone.
  KJ_EXPECT(dir->openSubdir(kj::Path({"mykey"}))->listNames().size() == 2);
  KJ_EXPECT(dir->exists(kj::Path({"mykey",
    "02b496f65dd35cbac90e3e72dc5a398ee93926ea4a3821e26677082d2e6f9b79.sqlite"})));
  KJ_EXPECT(dir->exists(kj::Path({"mykey",
    "59002eb8cf872e541722977a258a12d6a93bbe8192b502e1c0cb250aa91af234.sqlite"})));

  // Let's start a new server and verify it can load the files from disk.
  {
    TestServer test(config);

    // Link our directory into the test filesystem.
    test.root->transfer(
        kj::Path({"var"_kj, "do-storage"_kj}), kj::WriteMode::CREATE | kj::WriteMode::CREATE_PARENT,
        *dir, nullptr, kj::TransferMode::LINK);

    test.start();
    auto conn = test.connect("test-addr");
    conn.httpGet200("/",
        "59002eb8cf872e541722977a258a12d6a93bbe8192b502e1c0cb250aa91af234: http://foo/ 4");
    conn.httpGet200("/",
        "59002eb8cf872e541722977a258a12d6a93bbe8192b502e1c0cb250aa91af234: http://foo/ 5");
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
  conn.httpGet200("/",
      "http://foo/: http://foo/ 0");
  conn.httpGet200("/",
      "http://foo/: http://foo/ 1");
  conn.httpGet200("/",
      "http://foo/: http://foo/ 2");
  conn.httpGet200("/bar",
      "http://foo/bar: http://foo/bar 0");
  conn.httpGet200("/bar",
      "http://foo/bar: http://foo/bar 1");
  conn.httpGet200("/",
      "http://foo/: http://foo/ 3");
  conn.httpGet200("/bar",
      "http://foo/bar: http://foo/bar 2");
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
  test.fakeDate = kj::UNIX_EPOCH + 2 * kj::DAYS + 5 * kj::HOURS +
                  18 * kj::MINUTES + 23 * kj::SECONDS;
  dir->openFile(kj::Path({"foo.txt"}), mode)->writeAll("hello from foo.txt\n");
  test.fakeDate = kj::UNIX_EPOCH + 400 * kj::DAYS + 2 * kj::HOURS +
                  52 * kj::MINUTES + 9 * kj::SECONDS + 163 * kj::MILLISECONDS;
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

  // Write a file to a new direcotry.
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
  conn.httpGet200("/",
      "No Cache was configured");

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
    KJ_EXPECT_LOG(INFO, "[ TEST ] hello");
    KJ_EXPECT_LOG(INFO, "[ PASS ] hello");
    KJ_EXPECT(test.server.test(v8System, *test.config, "hello", "default").wait(test.ws));
  }

  {
    TestServer test(config);
    KJ_EXPECT_LOG(INFO, "[ TEST ] hello:fail");
    KJ_EXPECT_LOG(INFO, "Error: ded");
    KJ_EXPECT_LOG(INFO, "[ FAIL ] hello:fail");
    KJ_EXPECT(!test.server.test(v8System, *test.config, "hello", "fail").wait(test.ws));
  }

  {
    TestServer test(config);
    KJ_EXPECT_LOG(INFO, "[ TEST ] hello");
    KJ_EXPECT_LOG(INFO, "[ PASS ] hello");
    KJ_EXPECT_LOG(INFO, "[ TEST ] hello:fail");
    KJ_EXPECT_LOG(INFO, "Error: ded");
    KJ_EXPECT_LOG(INFO, "[ FAIL ] hello:fail");
    KJ_EXPECT(!test.server.test(v8System, *test.config, "hello", "*").wait(test.ws));
  }

  {
    TestServer test(config);
    KJ_EXPECT_LOG(INFO, "[ TEST ] hello");
    KJ_EXPECT_LOG(INFO, "[ PASS ] hello");
    KJ_EXPECT_LOG(INFO, "[ TEST ] another");
    KJ_EXPECT_LOG(INFO, "other test");
    KJ_EXPECT_LOG(INFO, "[ PASS ] another");
    KJ_EXPECT(test.server.test(v8System, *test.config, "*", "default").wait(test.ws));
  }

  {
    TestServer test(config);
    KJ_EXPECT_LOG(INFO, "[ TEST ] hello");
    KJ_EXPECT_LOG(INFO, "[ PASS ] hello");
    KJ_EXPECT_LOG(INFO, "[ TEST ] hello:fail");
    KJ_EXPECT_LOG(INFO, "Error: ded");
    KJ_EXPECT_LOG(INFO, "[ FAIL ] hello:fail");
    KJ_EXPECT_LOG(INFO, "[ TEST ] another");
    KJ_EXPECT_LOG(INFO, "other test");
    KJ_EXPECT_LOG(INFO, "[ PASS ] another");
    KJ_EXPECT(!test.server.test(v8System, *test.config, "*", "*").wait(test.ws));
  }
}

// =======================================================================================

// TODO(beta): Test TLS (send and receive)
// TODO(beta): Test CLI overrides

}  // namespace
}  // namespace workerd::server
