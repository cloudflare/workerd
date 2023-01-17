#include "test-fixture.h"
#include <workerd/io/actor-cache.h>
#include "workerd/io/io-channels.h"
#include "workerd/io/limit-enforcer.h"
#include "workerd/io/observer.h"
#include <workerd/io/worker-entrypoint.h>
#include <workerd/jsg/modules.h>
#include <workerd/server/workerd-api.h>

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
      kj::Maybe<kj::String> locationHint) override {
    KJ_FAIL_REQUIRE("no actor channels");
  }

  kj::Own<ActorChannel> getColoLocalActor(uint channel, kj::StringPtr id) override {
    KJ_FAIL_REQUIRE("no actor channels");
  }

  TimerChannel& timer;
};

static constexpr kj::StringPtr script = R"SCRIPT(
  addEventListener("fetch", event => {
    event.respondWith(new Response("OK"));
  });
)SCRIPT"_kj;

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
  kj::Own<void> enterJs(jsg::Lock& lock) override { return {}; }
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
        .softLimit = 16ull << 20,
        .hardLimit = 128ull << 20,
        .staleTimeout = 30 * kj::SECONDS,
        .dirtyKeySoftLimit = 64,
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

} // namespace


TestFixture::TestFixture(SetupParams params)
  : params(params),
    io(params.waitScope == nullptr ? kj::Maybe(kj::setupAsyncIo()) : kj::Maybe<kj::AsyncIoContext>(nullptr)),
    timer(kj::heap<MockTimer>()),
    timerChannel(kj::heap<MockTimerChannel>()),
    entropySource(kj::heap<MockEntropySource>()),
    threadContextHeaderBundle(headerTableBuilder),
    httpOverCapnpFactory(byteStreamFactory,
      capnp::HttpOverCapnpFactory::HeaderIdBundle(headerTableBuilder)),
    threadContext(*timer, *entropySource, threadContextHeaderBundle, httpOverCapnpFactory, byteStreamFactory, false),
    isolateLimitEnforcer(kj::heap<MockIsolateLimitEnforcer>()),
    apiIsolate(kj::heap<server::WorkerdApiIsolate>(
      testV8System,
      params.featureFlags.orDefault(CompatibilityFlags::Reader()),
      *isolateLimitEnforcer)),
    workerIsolate(kj::atomicRefcounted<Worker::Isolate>(
      kj::mv(apiIsolate),
      kj::atomicRefcounted<IsolateObserver>(),
      scriptId,
      kj::mv(isolateLimitEnforcer),
      false)),
    workerScript(kj::atomicRefcounted<Worker::Script>(
      kj::atomicAddRef(*workerIsolate),
      scriptId,
      Worker::Script::ScriptSource {
        script,
        "main.js"_kj,
        [](jsg::Lock& lock, const Worker::ApiIsolate& apiIsolate)
            -> kj::Array<Worker::Script::CompiledGlobal> {
          return nullptr;
        }
      },
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
    waitUntilTasks(*errorHandler) { }

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
