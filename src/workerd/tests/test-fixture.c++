// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <algorithm>

#include <workerd/api/global-scope.h>
#include <workerd/io/actor-cache.h>
#include <workerd/io/io-channels.h>
#include <workerd/io/limit-enforcer.h>
#include <workerd/io/observer.h>
#include <workerd/io/worker-entrypoint.h>
#include <workerd/jsg/modules.h>
#include <workerd/server/workerd-api.h>

#include "test-fixture.h"


namespace workerd {

namespace {

jsg::V8System testV8System;

class MockCacheClient final: public CacheClient {
  kj::Own<kj::HttpClient> getDefault(
      kj::Maybe<kj::String> cfBlobJson, SpanParent parentSpan) override{
    KJ_FAIL_REQUIRE("Not implemented");
  }

  kj::Own<kj::HttpClient> getNamespace(
      kj::StringPtr name, kj::Maybe<kj::String> cfBlobJson,
      SpanParent parentSpan) override {
    return getDefault(kj::mv(cfBlobJson), kj::mv(parentSpan));
  }
};

class MockTimer final: public kj::Timer {
  kj::TimePoint now() const { return kj::systemCoarseMonotonicClock().now(); }
  kj::Promise<void> atTime(kj::TimePoint time) { return kj::NEVER_DONE; }
  kj::Promise<void> afterDelay(kj::Duration delay) { return kj::NEVER_DONE; }
};

class DummyErrorHandler final: public kj::TaskSet::ErrorHandler {
    void taskFailed(kj::Exception&& exception) override {}
};

struct MockTimerChannel final: public TimerChannel {
  void syncTime() override { }

  kj::Date now() override { return kj::systemPreciseCalendarClock().now(); }

  kj::Promise<void> atTime(kj::Date when) override { return kj::NEVER_DONE; }

  kj::Promise<void> afterLimitTimeout(kj::Duration t) override { return kj::NEVER_DONE; }
};

struct DummyIoChannelFactory final: public IoChannelFactory {
  DummyIoChannelFactory(TimerChannel& timer): timer(timer) {}

  kj::Own<WorkerInterface> startSubrequest(uint channel, SubrequestMetadata metadata) override {
    KJ_FAIL_ASSERT("no subrequests");
  }

  capnp::Capability::Client getCapability(uint channel) override {
    KJ_FAIL_ASSERT("no capabilities");
  }

  kj::Own<CacheClient> getCache() override {
    return kj::heap<MockCacheClient>();
  }

  TimerChannel& getTimer() override { return timer; }

  kj::Promise<void> writeLogfwdr(uint channel,
      kj::FunctionParam<void(capnp::AnyPointer::Builder)> buildMessage) override {
    KJ_FAIL_ASSERT("no log channels");
  }

  kj::Own<ActorChannel> getGlobalActor(uint channel, const ActorIdFactory::ActorId& id,
      kj::Maybe<kj::String> locationHint, ActorGetMode mode, SpanParent parentSpan) override {
    KJ_FAIL_REQUIRE("no actor channels");
  }

  kj::Own<ActorChannel> getColoLocalActor(uint channel, kj::StringPtr id,
      SpanParent parentSpan) override {
    KJ_FAIL_REQUIRE("no actor channels");
  }

  TimerChannel& timer;
};

static constexpr kj::StringPtr mainModuleSource = R"SCRIPT(
  export default {
    fetch(request) { return new Response("OK"); },
  };
)SCRIPT"_kj;
static constexpr kj::StringPtr mainModuleName = "main"_kj;

static constexpr kj::StringPtr scriptId = "script"_kj;

class MockEntropySource final: public kj::EntropySource {
public:
  ~MockEntropySource() {}
  void generate(kj::ArrayPtr<kj::byte> buffer) override {
    for (kj::byte& b: buffer) {
     b = counter++;
    }
  }

  template <typename T>
  T rand() {
    T r;
    this->generate(kj::arrayPtr(&r, 1).asBytes());
    return r;
  }

private:
  kj::byte counter = 0;
};

struct MockLimitEnforcer final: public LimitEnforcer {
  kj::Own<void> enterJs(jsg::Lock& lock, IoContext& context) override { return {}; }
  void topUpActor() override {}
  void newSubrequest(bool isInHouse) override {}
  void newKvRequest(KvOpType op) override {}
  void newAnalyticsEngineRequest() override {}
  kj::Promise<void> limitDrain() override { return kj::NEVER_DONE; }
  kj::Promise<void> limitScheduled() override { return kj::NEVER_DONE; }
  size_t getBufferingLimit() override { return kj::maxValue; }
  kj::Maybe<EventOutcome> getLimitsExceeded() override { return nullptr; }
  kj::Promise<void> onLimitsExceeded() override { return kj::NEVER_DONE; }
  void requireLimitsNotExceeded() override {}
  void reportMetrics(RequestObserver& requestMetrics) override {}

};

struct MockIsolateLimitEnforcer final: public IsolateLimitEnforcer {
   v8::Isolate::CreateParams getCreateParams() override { return {}; }
    void customizeIsolate(v8::Isolate* isolate) override {}
    ActorCacheSharedLruOptions getActorCacheLruOptions() override {
      return {
        .softLimit = 16 * (1ull << 20), // 16 MiB
        .hardLimit = 128 * (1ull << 20), // 128 MiB
        .staleTimeout = 30 * kj::SECONDS,
        .dirtyListByteLimit = 8 * (1ull << 20), // 8 MiB
        .maxKeysPerRpc = 128,
        .neverFlush = true
      };
    }
    kj::Own<void> enterStartupJs(
        jsg::Lock& lock, kj::Maybe<kj::Exception>& error) const override {
      return {};
    }
    kj::Own<void> enterDynamicImportJs(
        jsg::Lock& lock, kj::Maybe<kj::Exception>& error) const override {
      return {};
    }
    kj::Own<void> enterLoggingJs(
        jsg::Lock& lock, kj::Maybe<kj::Exception>& error) const override {
      return {};
    }
    kj::Own<void> enterInspectorJs(
        jsg::Lock& loc, kj::Maybe<kj::Exception>& error) const override {
      return {};
    }
    void completedRequest(kj::StringPtr id) const override {}
    bool exitJs(jsg::Lock& lock) const override { return false; }
    void reportMetrics(IsolateObserver& isolateMetrics) const override {}

};

struct MockErrorReporter final: public Worker::ValidationErrorReporter {
  void addError(kj::String error) override {
    KJ_FAIL_REQUIRE("unexpected error", error);
  }

  void addHandler(kj::Maybe<kj::StringPtr> exportName, kj::StringPtr type) override {
    KJ_FAIL_REQUIRE("addHandler not implemented", exportName.orDefault("<empty>"), type);
  }
};

inline server::config::Worker::Reader buildConfig(
    TestFixture::SetupParams& params,
    capnp::MallocMessageBuilder& arena) {
  auto config = arena.initRoot<server::config::Worker>();
  auto modules = config.initModules(1);
  modules[0].setName(mainModuleName);
  modules[0].setEsModule(params.mainModuleSource.orDefault(mainModuleSource));
  return config;
}

struct MemoryOutputStream final: kj::AsyncOutputStream, public kj::Refcounted  {
  kj::Vector<byte> content;

  kj::Promise<void> write(const void* buffer, size_t size) override {
    auto ptr = reinterpret_cast<const byte*>(buffer);
    content.addAll(ptr, ptr + size);
    return kj::READY_NOW;
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
    KJ_FAIL_REQUIRE("NOT IMPLEMENTED");
  }

  kj::Promise<void> whenWriteDisconnected() override {
    return kj::NEVER_DONE;
  }

  kj::String str() {
    return kj::str(content.asPtr().asChars());
  }
};

struct MockResponse final: public kj::HttpService::Response {
  uint statusCode = 0;
  kj::StringPtr statusText;
  kj::Own<MemoryOutputStream> body = kj::refcounted<MemoryOutputStream>();

  kj::Own<kj::AsyncOutputStream> send(uint statusCode, kj::StringPtr statusText,
                                      const kj::HttpHeaders &headers,
                                      kj::Maybe<uint64_t> expectedBodySize = nullptr) override {
    this->statusCode = statusCode;
    this->statusText = statusText;
    return kj::addRef(*body);
  }

  kj::Own<kj::WebSocket> acceptWebSocket(const kj::HttpHeaders &headers) override {
    KJ_FAIL_REQUIRE("NOT SUPPORTED");
  }
};

struct MemoryInputStream final: public kj::AsyncInputStream {
  kj::ArrayPtr<const byte> data;

  MemoryInputStream(kj::ArrayPtr<const byte> data) : data(data) { }

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    auto toRead = kj::min(data.size(), maxBytes);
    memcpy(buffer, data.begin(), toRead);
    data = data.slice(toRead, data.size());
    return toRead;
  }
};

} // namespace


TestFixture::TestFixture(SetupParams params)
  : params(params),
    config(buildConfig(params, configArena)),
    io(params.waitScope == nullptr ? kj::Maybe(kj::setupAsyncIo()) : kj::Maybe<kj::AsyncIoContext>(nullptr)),
    timer(kj::heap<MockTimer>()),
    timerChannel(kj::heap<MockTimerChannel>()),
    entropySource(kj::heap<MockEntropySource>()),
    threadContextHeaderBundle(headerTableBuilder),
    httpOverCapnpFactory(byteStreamFactory,
      capnp::HttpOverCapnpFactory::HeaderIdBundle(headerTableBuilder)),
    threadContext(*timer, *entropySource, threadContextHeaderBundle, httpOverCapnpFactory, byteStreamFactory, false),
    isolateLimitEnforcer(kj::heap<MockIsolateLimitEnforcer>()),
    errorReporter(kj::heap<MockErrorReporter>()),
    apiIsolate(kj::heap<server::WorkerdApiIsolate>(
      testV8System,
      params.featureFlags.orDefault(CompatibilityFlags::Reader()),
      *isolateLimitEnforcer,
      kj::atomicRefcounted<IsolateObserver>())),
    workerIsolate(kj::atomicRefcounted<Worker::Isolate>(
      kj::mv(apiIsolate),
      kj::atomicRefcounted<IsolateObserver>(),
      scriptId,
      kj::mv(isolateLimitEnforcer),
      Worker::Isolate::InspectorPolicy::DISALLOW)),
    workerScript(kj::atomicRefcounted<Worker::Script>(
      kj::atomicAddRef(*workerIsolate),
      scriptId,
      server::WorkerdApiIsolate::extractSource(mainModuleName, config, *errorReporter,
          capnp::List<server::config::Extension>::Reader{}),
      IsolateObserver::StartType::COLD, false, nullptr)),
    worker(kj::atomicRefcounted<Worker>(
      kj::atomicAddRef(*workerScript),
      kj::atomicRefcounted<WorkerObserver>(),
      [](jsg::Lock& lock, const Worker::ApiIsolate& apiIsolate, v8::Local<v8::Object> target) {
        // no bindings, nothing to do
      },
      IsolateObserver::StartType::COLD,
      nullptr /* parentSpan */,
      Worker::LockType(Worker::Lock::TakeSynchronously(nullptr))
    )),
    errorHandler(kj::heap<DummyErrorHandler>()),
    waitUntilTasks(*errorHandler),
    headerTable(headerTableBuilder.build()) { }

void TestFixture::runInIoContext(
    kj::Function<kj::Promise<void>(const Environment&)>&& callback,
    kj::ArrayPtr<kj::StringPtr> errorsToIgnore) {
  auto ignoreDescription = [&errorsToIgnore](kj::StringPtr description) {
    return std::any_of(errorsToIgnore.begin(), errorsToIgnore.end(), [&description](auto error) {
      return strstr(description.cStr(), error.cStr());
    });
  };

  try {
    runInIoContext([callback = kj::mv(callback), &ignoreDescription]
                   (const TestFixture::Environment& env) mutable -> kj::Promise<void> {
      v8::TryCatch tryCatch(env.isolate);
      try {
          return callback(env);
      } catch (jsg::JsExceptionThrown&) {
        if (!tryCatch.CanContinue()) {
          throw;
        }
        if (ignoreDescription(kj::str(tryCatch.Exception()))) {
          return kj::READY_NOW;
        }
        tryCatch.ReThrow();
        throw;
      }
    });
  } catch (kj::Exception& e) {
    if (!ignoreDescription(e.getDescription())) { throw e; }
  }
}

kj::Own<IoContext::IncomingRequest> TestFixture::createIncomingRequest() {
  auto context = kj::refcounted<IoContext>(
      threadContext, kj::atomicAddRef(*worker), nullptr, kj::heap<MockLimitEnforcer>());
  auto incomingRequest = kj::heap<IoContext::IncomingRequest>(
      kj::addRef(*context), kj::heap<DummyIoChannelFactory>(*timerChannel),
      kj::refcounted<RequestObserver>(), nullptr);
  incomingRequest->delivered();
  return incomingRequest;
}

TestFixture::Response TestFixture::runRequest(
    kj::HttpMethod method, kj::StringPtr url, kj::StringPtr body) {
  kj::HttpHeaders requestHeaders(*headerTable);
  MockResponse response;
  MemoryInputStream requestBody(body.asBytes());

  runInIoContext([&](const TestFixture::Environment& env) {
    auto& globalScope = env.lock.getGlobalScope();
    return globalScope.request(
        method,
        url,
        requestHeaders,
        requestBody,
        response,
        nullptr,
        env.lock,
        env.lock.getExportedHandler(nullptr, nullptr));
  });

  return { .statusCode = response.statusCode, .body = response.body->str() };
}

v8::Local<v8::Value> TestFixture::V8Environment::compileAndRunScript(
    kj::StringPtr code) const {
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::String> source = jsg::v8Str(isolate, code);
  v8::Local<v8::Script> script;
  if (!v8::Script::Compile(context, source).ToLocal(&script)) {
    KJ_FAIL_REQUIRE("error parsing code", code);
  }

  v8::TryCatch catcher(isolate);
  v8::Local<v8::Value> result;
  if (script->Run(context).ToLocal(&result)) {
    return result;
  } else {
    KJ_REQUIRE(catcher.HasCaught());
    catcher.ReThrow();
    throw jsg::JsExceptionThrown();
  }
}

v8::Local<v8::Object> TestFixture::V8Environment::compileAndInstantiateModule(
    kj::StringPtr name, kj::ArrayPtr<const char> src) const {
  v8::Local<v8::Module> module;

  v8::ScriptCompiler::Source source(jsg::v8Str(isolate, src),
  v8::ScriptOrigin(isolate, jsg::v8StrIntern(isolate, name),
      false, false, false, -1, {}, false, false, true /* is_module */));

  if (!v8::ScriptCompiler::CompileModule(isolate, &source).ToLocal(&module)) {
    KJ_FAIL_REQUIRE("error parsing code");
  }

  auto& js = jsg::Lock::from(isolate);
  jsg::instantiateModule(js, module);
  return module->GetModuleNamespace()->ToObject(isolate->GetCurrentContext()).ToLocalChecked();
}

}  // namespace workerd
