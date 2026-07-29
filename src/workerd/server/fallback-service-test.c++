// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "fallback-service.h"

#include <capnp/compat/json.h>
#include <capnp/message.h>
#include <kj/async-io.h>
#include <kj/compat/http.h>
#include <kj/compat/url.h>
#include <kj/debug.h>
#include <kj/mutex.h>
#include <kj/test.h>
#include <kj/thread.h>

namespace workerd::fallback {
namespace {

struct ExpectedErrorState {
  kj::MutexGuarded<unsigned int> count{0};
};

class ExpectedErrorLogger final: public kj::ExceptionCallback {
 public:
  explicit ExpectedErrorLogger(ExpectedErrorState& state): state(state) {}

  void logMessage(kj::LogSeverity severity,
      const char* file,
      int line,
      int contextDepth,
      kj::String&& text) override {
    if (severity == kj::LogSeverity::ERROR &&
        text.contains("Fallback service failed to fetch module"_kj)) {
      auto count = state.count.lockExclusive();
      ++*count;
      return;
    }
    kj::ExceptionCallback::logMessage(severity, file, line, contextDepth, kj::mv(text));
  }

  kj::Function<void(kj::Function<void()>)> getThreadInitializer() override {
    auto nextInitializer = kj::ExceptionCallback::getThreadInitializer();
    auto* statePtr = &state;
    return [nextInitializer = kj::mv(nextInitializer), statePtr](
               kj::Function<void()> function) mutable {
      nextInitializer([statePtr, function = kj::mv(function)]() mutable {
        ExpectedErrorLogger logger(*statePtr);
        function();
      });
    };
  }

 private:
  ExpectedErrorState& state;
};

class TestFallbackService final: public kj::HttpService {
 public:
  TestFallbackService(kj::HttpHeaderTable& headerTable,
      kj::HttpHeaderId resolveMethodHeader,
      kj::Own<kj::PromiseFulfiller<void>> doneFulfiller)
      : headerTable(headerTable),
        resolveMethodHeader(resolveMethodHeader),
        doneFulfiller(kj::mv(doneFulfiller)) {}

  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      Response& response) override {
    auto body = co_await requestBody.readAllText();
    kj::HttpHeaders responseHeaders(headerTable);
    kj::StringPtr responseBody;
    unsigned int statusCode = 200;
    kj::StringPtr statusText = "OK"_kj;

    switch (requestCount++) {
      case 0: {
        KJ_ASSERT(method == kj::HttpMethod::GET);
        KJ_ASSERT(body.size() == 0);
        KJ_ASSERT(KJ_ASSERT_NONNULL(headers.get(resolveMethodHeader)) == "require"_kj);

        auto parsed = kj::Url::parse(url, kj::Url::HTTP_REQUEST);
        KJ_ASSERT(parsed.query.size() == 3);
        KJ_ASSERT(parsed.query[0].name == "specifier"_kj);
        KJ_ASSERT(parsed.query[0].value == "/module.js"_kj);
        KJ_ASSERT(parsed.query[1].name == "referrer"_kj);
        KJ_ASSERT(parsed.query[1].value == "/worker.js"_kj);
        KJ_ASSERT(parsed.query[2].name == "rawSpecifier"_kj);
        KJ_ASSERT(parsed.query[2].value == "./module.js"_kj);
        responseBody = R"({"esModule":"export default 1;"})"_kj;
        break;
      }
      case 1: {
        KJ_ASSERT(method == kj::HttpMethod::POST);
        capnp::JsonCodec json;
        capnp::MallocMessageBuilder message;
        auto request = message.initRoot<server::config::FallbackServiceRequest>();
        json.decode(body, request);
        auto reader = request.asReader();
        KJ_ASSERT(reader.getType() == "internal"_kj);
        KJ_ASSERT(reader.getSpecifier() == "file:///bundle/text.txt"_kj);
        KJ_ASSERT(reader.getRawSpecifier() == "./text.txt"_kj);
        KJ_ASSERT(reader.getReferrer() == "file:///bundle/worker.js"_kj);
        KJ_ASSERT(reader.getAttributes().size() == 1);
        KJ_ASSERT(reader.getAttributes()[0].getName() == "type"_kj);
        KJ_ASSERT(reader.getAttributes()[0].getValue() == "json"_kj);
        responseBody = R"({"name":"file:///bundle/text.txt","text":"fallback text"})"_kj;
        break;
      }
      case 2:
        KJ_ASSERT(method == kj::HttpMethod::POST);
        statusCode = 301;
        statusText = "Moved Permanently"_kj;
        responseHeaders.setPtr(kj::HttpHeaderId::LOCATION, "file:///bundle/redirected.js"_kj);
        responseBody = ""_kj;
        break;
      case 3:
        KJ_ASSERT(method == kj::HttpMethod::POST);
        statusCode = 404;
        statusText = "Not Found"_kj;
        responseBody = "not found"_kj;
        break;
      case 4:
        KJ_ASSERT(method == kj::HttpMethod::POST);
        responseBody = "{"_kj;
        break;
      case 5:
        KJ_ASSERT(method == kj::HttpMethod::POST);
        responseBody = R"({"data":[1,2,3]})"_kj;
        break;
      default:
        KJ_FAIL_ASSERT("unexpected fallback request");
    }

    auto output =
        response.send(statusCode, statusText, kj::mv(responseHeaders), responseBody.size());
    co_await output->write(responseBody.asBytes());
    if (requestCount == EXPECTED_REQUEST_COUNT) {
      doneFulfiller->fulfill();
    }
  }

 private:
  static constexpr unsigned int EXPECTED_REQUEST_COUNT = 6;

  kj::HttpHeaderTable& headerTable;
  kj::HttpHeaderId resolveMethodHeader;
  kj::Own<kj::PromiseFulfiller<void>> doneFulfiller;
  unsigned int requestCount = 0;
};

KJ_TEST("Fallback service client implements the V1 and V2 protocols") {
  kj::MutexGuarded<uint16_t> serverPort(0);
  kj::Thread serverThread([&serverPort]() {
    auto io = kj::setupAsyncIo();
    kj::HttpHeaderTable::Builder headerTableBuilder;
    auto resolveMethodHeader = headerTableBuilder.add("x-resolve-method");
    auto headerTable = headerTableBuilder.build();
    auto listener =
        io.provider->getNetwork().parseAddress("127.0.0.1", 0).wait(io.waitScope)->listen();
    {
      auto lock = serverPort.lockExclusive();
      *lock = listener->getPort();
    }

    auto done = kj::newPromiseAndFulfiller<void>();
    TestFallbackService service(*headerTable, resolveMethodHeader, kj::mv(done.fulfiller));
    kj::HttpServer server(io.provider->getTimer(), *headerTable, service);
    auto listenTask = server.listenHttp(*listener).eagerlyEvaluate(nullptr);
    done.promise.wait(io.waitScope);
  });

  auto address = serverPort.when([](uint16_t port) { return port != 0; },
      [](uint16_t& port) { return kj::str("127.0.0.1:", port); });

  kj::HashMap<kj::StringPtr, kj::StringPtr> attributes;
  attributes.insert("type"_kj, "json"_kj);

  ExpectedErrorState expectedErrors;
  ExpectedErrorLogger expectedErrorLogger(expectedErrors);
  FallbackServiceClient client(kj::mv(address));
  {
    auto result = KJ_ASSERT_NONNULL(client.tryResolve(
        Version::V1, ImportType::REQUIRE, "/module.js", "./module.js", "/worker.js", attributes));
    KJ_SWITCH_ONEOF(result) {
      KJ_CASE_ONEOF(redirect, kj::String) {
        KJ_FAIL_ASSERT("expected a module", redirect);
      }
      KJ_CASE_ONEOF(module, kj::Own<server::config::Worker::Module::Reader>) {
        KJ_EXPECT(module->getName() == "module.js"_kj);
        KJ_EXPECT(module->getEsModule() == "export default 1;"_kj);
      }
    }
  }
  {
    auto result = KJ_ASSERT_NONNULL(client.tryResolve(Version::V2, ImportType::INTERNAL,
        "file:///bundle/text.txt", "./text.txt", "file:///bundle/worker.js", attributes));
    KJ_SWITCH_ONEOF(result) {
      KJ_CASE_ONEOF(redirect, kj::String) {
        KJ_FAIL_ASSERT("expected a module", redirect);
      }
      KJ_CASE_ONEOF(module, kj::Own<server::config::Worker::Module::Reader>) {
        KJ_EXPECT(module->getName() == "file:///bundle/text.txt"_kj);
        KJ_EXPECT(module->getText() == "fallback text"_kj);
      }
    }
  }
  {
    auto result = KJ_ASSERT_NONNULL(client.tryResolve(Version::V2, ImportType::IMPORT,
        "file:///bundle/redirect.js", "./redirect.js", "file:///bundle/worker.js", attributes));
    KJ_SWITCH_ONEOF(result) {
      KJ_CASE_ONEOF(redirect, kj::String) {
        KJ_EXPECT(redirect == "file:///bundle/redirected.js"_kj);
      }
      KJ_CASE_ONEOF(module, kj::Own<server::config::Worker::Module::Reader>) {
        KJ_FAIL_ASSERT("expected a redirect", module->getName());
      }
    }
  }

  KJ_EXPECT(client.tryResolve(Version::V2, ImportType::IMPORT, "file:///bundle/missing.js",
                "./missing.js", "file:///bundle/worker.js", attributes) == kj::none);
  KJ_EXPECT(client.tryResolve(Version::V2, ImportType::IMPORT, "file:///bundle/malformed.js",
                "./malformed.js", "file:///bundle/worker.js", attributes) == kj::none);

  auto result = KJ_ASSERT_NONNULL(client.tryResolve(Version::V2, ImportType::IMPORT,
      "file:///bundle/data.bin", "./data.bin", "file:///bundle/worker.js", attributes));
  KJ_SWITCH_ONEOF(result) {
    KJ_CASE_ONEOF(redirect, kj::String) {
      KJ_FAIL_ASSERT("expected a module", redirect);
    }
    KJ_CASE_ONEOF(module, kj::Own<server::config::Worker::Module::Reader>) {
      KJ_EXPECT(module->getName() == "file:///bundle/data.bin"_kj);
      KJ_EXPECT(module->getData() == kj::ArrayPtr<const kj::byte>({1, 2, 3}));
    }
  }

  auto errorCount = expectedErrors.count.lockShared();
  KJ_EXPECT(*errorCount == 2);
}

}  // namespace
}  // namespace workerd::fallback
