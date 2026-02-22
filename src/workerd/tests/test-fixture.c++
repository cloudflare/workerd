// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "test-fixture.h"

#include <workerd/api/actor-state.h>
#include <workerd/api/global-scope.h>
#include <workerd/api/memory-cache.h>
#include <workerd/io/actor-cache.h>
#include <workerd/io/actor-id.h>
#include <workerd/io/io-channels.h>
#include <workerd/io/limit-enforcer.h>
#include <workerd/io/observer.h>
#include <workerd/io/tracer.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/setup.h>
#include <workerd/server/workerd-api.h>
#include <workerd/util/autogate.h>
#include <workerd/util/stream-utils.h>

#include <algorithm>

namespace workerd {

namespace {

jsg::V8System testV8System;

class MockCacheClient final: public CacheClient {
  kj::Own<kj::HttpClient> getDefault(CacheClient::SubrequestMetadata metadata) override {
    KJ_FAIL_REQUIRE("Not implemented");
  }

  kj::Own<kj::HttpClient> getNamespace(
      kj::StringPtr name, CacheClient::SubrequestMetadata metadata) override {
    return getDefault(kj::mv(metadata));
  }
};

class MockTimer final: public kj::Timer {
  kj::TimePoint now() const override {
    return kj::systemCoarseMonotonicClock().now();
  }
  kj::Promise<void> atTime(kj::TimePoint time) override {
    return kj::NEVER_DONE;
  }
  kj::Promise<void> afterDelay(kj::Duration delay) override {
    return kj::NEVER_DONE;
  }
};

class DummyErrorHandler final: public kj::TaskSet::ErrorHandler {
  void taskFailed(kj::Exception&& exception) override {}
};

struct MockTimerChannel final: public TimerChannel {
  void syncTime() override {}

  kj::Date now(kj::Maybe<kj::Date>) override {
    return kj::systemPreciseCalendarClock().now();
  }

  kj::Promise<void> atTime(kj::Date when) override {
    return kj::NEVER_DONE;
  }

  kj::Promise<void> afterLimitTimeout(kj::Duration t) override {
    return kj::NEVER_DONE;
  }
};

// A TimerChannel implementation that uses real timers from the KJ event loop.
// Useful for tests that need actual timer functionality (e.g., benchmarks with
// simulated I/O delays).
struct RealTimerChannel final: public TimerChannel {
  explicit RealTimerChannel(kj::Timer& timer): timer(timer) {}

  void syncTime() override {}

  kj::Date now(kj::Maybe<kj::Date>) override {
    return kj::systemPreciseCalendarClock().now();
  }

  kj::Promise<void> atTime(kj::Date when) override {
    auto nowTime = kj::systemPreciseCalendarClock().now();
    if (when <= nowTime) {
      return kj::READY_NOW;
    }
    return timer.afterDelay(when - nowTime);
  }

  kj::Promise<void> afterLimitTimeout(kj::Duration t) override {
    return timer.afterDelay(t);
  }

  kj::Timer& timer;
};

struct DummyIoChannelFactory final: public IoChannelFactory {
  DummyIoChannelFactory(TimerChannel& timer): timer(timer) {}

  kj::Own<WorkerInterface> startSubrequest(uint channel, SubrequestMetadata metadata) override {
    KJ_FAIL_ASSERT("no subrequests");
  }

  kj::Own<SubrequestChannel> getSubrequestChannel(
      uint channel, kj::Maybe<Frankenvalue> props) override {
    KJ_FAIL_ASSERT("no subrequests");
  }

  capnp::Capability::Client getCapability(uint channel) override {
    KJ_FAIL_ASSERT("no capabilities");
  }

  kj::Own<CacheClient> getCache() override {
    return kj::heap<MockCacheClient>();
  }

  TimerChannel& getTimer() override {
    return timer;
  }

  kj::Promise<void> writeLogfwdr(
      uint channel, kj::FunctionParam<void(capnp::AnyPointer::Builder)> buildMessage) override {
    KJ_FAIL_ASSERT("no log channels");
  }

  kj::Own<ActorChannel> getGlobalActor(uint channel,
      const ActorIdFactory::ActorId& id,
      kj::Maybe<kj::String> locationHint,
      ActorGetMode mode,
      bool enableReplicaRouting,
      ActorRoutingMode routingMode,
      SpanParent parentSpan,
      kj::Maybe<ActorVersion> version) override {
    KJ_FAIL_REQUIRE("no actor channels");
  }

  kj::Own<ActorChannel> getColoLocalActor(
      uint channel, kj::StringPtr id, SpanParent parentSpan) override {
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
  kj::Own<void> enterJs(jsg::Lock& lock, IoContext& context) override {
    return {};
  }
  void topUpActor() override {}
  void newSubrequest(bool isInHouse) override {}
  void newKvRequest(KvOpType op) override {}
  void newAnalyticsEngineRequest() override {}
  kj::Promise<void> limitDrain() override {
    return kj::NEVER_DONE;
  }
  kj::Promise<void> limitScheduled() override {
    return kj::NEVER_DONE;
  }
  kj::Duration getAlarmLimit() override {
    return 15 * kj::MINUTES;
  }
  size_t getBufferingLimit() override {
    return kj::maxValue;
  }
  kj::Maybe<EventOutcome> getLimitsExceeded() override {
    return kj::none;
  }
  kj::Promise<void> onLimitsExceeded() override {
    return kj::NEVER_DONE;
  }
  void setCpuLimitNearlyExceededCallback(kj::Function<void(void)> cb) override {}
  void requireLimitsNotExceeded() override {}
  void reportMetrics(RequestObserver& requestMetrics) override {}
  kj::Duration consumeTimeElapsedForPeriodicLogging() override {
    return 0 * kj::SECONDS;
  }
};

struct MockIsolateLimitEnforcer final: public IsolateLimitEnforcer {
  v8::Isolate::CreateParams getCreateParams() override {
    return {};
  }
  void customizeIsolate(v8::Isolate* isolate) override {}
  ActorCacheSharedLruOptions getActorCacheLruOptions() override {
    return {.softLimit = 16 * (1ull << 20),  // 16 MiB
      .hardLimit = 128 * (1ull << 20),       // 128 MiB
      .staleTimeout = 30 * kj::SECONDS,
      .dirtyListByteLimit = 8 * (1ull << 20),  // 8 MiB
      .maxKeysPerRpc = 128,
      .neverFlush = true};
  }
  kj::Own<void> enterStartupJs(
      jsg::Lock& lock, kj::OneOf<kj::Exception, kj::Duration>&) const override {
    return {};
  }
  kj::Own<void> enterStartupPython(
      jsg::Lock& lock, kj::OneOf<kj::Exception, kj::Duration>&) const override {
    return {};
  }
  kj::Own<void> enterDynamicImportJs(
      jsg::Lock& lock, kj::OneOf<kj::Exception, kj::Duration>&) const override {
    return {};
  }
  kj::Own<void> enterLoggingJs(
      jsg::Lock& lock, kj::OneOf<kj::Exception, kj::Duration>&) const override {
    return {};
  }
  kj::Own<void> enterInspectorJs(
      jsg::Lock& loc, kj::OneOf<kj::Exception, kj::Duration>&) const override {
    return {};
  }
  void completedRequest(kj::StringPtr id) const override {}
  bool exitJs(jsg::Lock& lock) const override {
    return false;
  }
  void reportMetrics(IsolateObserver& isolateMetrics) const override {}
  kj::Maybe<size_t> checkPbkdfIterations(jsg::Lock& lock, size_t iterations) const override {
    return kj::none;
  }
  bool hasExcessivelyExceededHeapLimit() const override {
    return false;
  }
};

struct MockErrorReporter final: public Worker::ValidationErrorReporter {
  void addError(kj::String error) override {
    KJ_FAIL_REQUIRE("unexpected error", error);
  }

  void addEntrypoint(kj::Maybe<kj::StringPtr> exportName, kj::Array<kj::String> methods) override {}
  void addActorClass(kj::StringPtr exportName) override {}
  void addWorkflowClass(kj::StringPtr exportName, kj::Array<kj::String> methods) override {}
};

inline server::config::Worker::Reader buildConfig(
    TestFixture::SetupParams& params, capnp::MallocMessageBuilder& arena) {
  auto config = arena.initRoot<server::config::Worker>();
  auto modules = config.initModules(1);
  modules[0].setName(mainModuleName);
  modules[0].setEsModule(params.mainModuleSource.orDefault(mainModuleSource));

  // Initialize autogates with an empty config. TODO(later): allow TestFixture to accept autogate
  // states and pass them in here.
  //
  // This needs to happen here because `buildConfig` is called early in the construction of
  // `TestFixture`.
  util::Autogate::initAutogate({});

  return config;
}

struct MemoryOutputStream final: kj::AsyncOutputStream, public kj::Refcounted {
  kj::Vector<byte> content;

  kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override {
    content.addAll(buffer);
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

  kj::Own<kj::AsyncOutputStream> send(uint statusCode,
      kj::StringPtr statusText,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize = kj::none) override {
    this->statusCode = statusCode;
    this->statusText = statusText;
    return kj::addRef(*body);
  }

  kj::Own<kj::WebSocket> acceptWebSocket(const kj::HttpHeaders& headers) override {
    KJ_FAIL_REQUIRE("NOT SUPPORTED");
  }
};

class MockActorLoopback: public Worker::Actor::Loopback, public kj::Refcounted {
 public:
  kj::Own<WorkerInterface> getWorker(IoChannelFactory::SubrequestMetadata metadata) override {
    return kj::Own<WorkerInterface>();
  };

  kj::Own<Worker::Actor::Loopback> addRef() override {
    return kj::addRef(*this);
  };
};

}  // namespace

using api::pyodide::PythonConfig;

const PythonConfig defaultPythonConfig{.packageDiskCacheRoot = kj::none,
  .pyodideDiskCacheRoot = kj::none,
  .createSnapshot = false,
  .createBaselineSnapshot = false};

TestFixture::TestFixture(SetupParams&& params)
    : waitScope(params.waitScope),
      config(buildConfig(params, configArena)),
      io(params.waitScope == kj::none ? kj::Maybe(kj::setupAsyncIo())
                                      : kj::Maybe<kj::AsyncIoContext>(kj::none)),
      timer(kj::heap<MockTimer>()),
      timerChannel(params.useRealTimers && io != kj::none
              ? kj::Own<TimerChannel>(
                    kj::heap<RealTimerChannel>(KJ_ASSERT_NONNULL(io).provider->getTimer()))
              : kj::Own<TimerChannel>(kj::heap<MockTimerChannel>())),
      entropySource(kj::heap<MockEntropySource>()),
      threadContextHeaderBundle(headerTableBuilder),
      httpOverCapnpFactory(byteStreamFactory,
          capnp::HttpOverCapnpFactory::HeaderIdBundle(headerTableBuilder),
          capnp::HttpOverCapnpFactory::LEVEL_2),
      threadContext(*timer,
          *entropySource,
          threadContextHeaderBundle,
          httpOverCapnpFactory,
          byteStreamFactory,
          false),
      errorReporter(kj::heap<MockErrorReporter>()),
      memoryCacheProvider(kj::heap<api::MemoryCacheProvider>(*timer)),
      isolateGroup(v8::IsolateGroup::GetDefault()),
      api(kj::heap<server::WorkerdApi>(testV8System,
          params.featureFlags.orDefault(CompatibilityFlags::Reader()),
          capnp::List<server::config::Extension>::Reader{},
          kj::heap<MockIsolateLimitEnforcer>()->getCreateParams(),
          isolateGroup,
          kj::atomicRefcounted<JsgIsolateObserver>(),
          *memoryCacheProvider,
          defaultPythonConfig)),
      workerIsolate(kj::atomicRefcounted<Worker::Isolate>(kj::mv(api),
          kj::atomicRefcounted<IsolateObserver>(),
          scriptId,
          kj::heap<MockIsolateLimitEnforcer>(),
          Worker::Isolate::InspectorPolicy::DISALLOW)),
      workerScript(kj::atomicRefcounted<Worker::Script>(kj::atomicAddRef(*workerIsolate),
          scriptId,
          server::WorkerdApi::extractSource(mainModuleName,
              config,
              params.featureFlags.orDefault(CompatibilityFlags::Reader()),
              *errorReporter),
          IsolateObserver::StartType::COLD,
          false,
          kj::none,
          kj::none,
          SpanParent(nullptr),
          newWorkerFileSystem(kj::heap<FsMap>(), getTmpDirectoryImpl()),
          kj::none /* new module registry */)),
      worker(kj::atomicRefcounted<Worker>(kj::atomicAddRef(*workerScript),
          kj::atomicRefcounted<WorkerObserver>(),
          [](jsg::Lock&, const Worker::Api&, v8::Local<v8::Object>, v8::Local<v8::Object>) {
            // no bindings, nothing to do
          },
          IsolateObserver::StartType::COLD,
          SpanParent(nullptr),
          Worker::LockType(Worker::Lock::TakeSynchronously(kj::none)))),
      errorHandler(kj::heap<DummyErrorHandler>()),
      waitUntilTasks(*errorHandler),
      headerTable(headerTableBuilder.build()) {
  KJ_IF_SOME(id, params.actorId) {
    auto makeActorCache = [](const ActorCache::SharedLru& sharedLru, OutputGate& outputGate,
                              ActorCache::Hooks& hooks, SqliteObserver& sqliteObserver) {
      return kj::heap<ActorCache>(
          server::newEmptyReadOnlyActorStorage(), sharedLru, outputGate, hooks);
    };
    auto makeStorage = [](jsg::Lock& js, const Worker::Api& api,
                           ActorCacheInterface& actorCache) -> jsg::Ref<api::DurableObjectStorage> {
      return js.alloc<api::DurableObjectStorage>(
          js, IoContext::current().addObject(actorCache), /*enableSql=*/false);
    };
    actor = kj::refcounted<Worker::Actor>(*worker, /*tracker=*/kj::none, kj::mv(id),
        /*hasTransient=*/false, makeActorCache,
        /*classname=*/kj::none, /*props=*/Frankenvalue(), makeStorage,
        kj::refcounted<MockActorLoopback>(), *timerChannel, kj::refcounted<ActorObserver>(),
        kj::none, kj::none);
  }
}

void TestFixture::runInIoContext(kj::Function<kj::Promise<void>(const Environment&)>&& callback,
    const kj::ArrayPtr<const kj::StringPtr> errorsToIgnore) {
  auto ignoreDescription = [&errorsToIgnore](kj::StringPtr description) {
    return std::any_of(errorsToIgnore.begin(), errorsToIgnore.end(),
        [&description](auto error) { return description.contains(error); });
  };

  try {
    runInIoContext([callback = kj::mv(callback), &ignoreDescription](
                       const TestFixture::Environment& env) mutable -> kj::Promise<void> {
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
    if (!ignoreDescription(e.getDescription())) {
      throw e;
    }
  }
}

kj::Own<IoContext::IncomingRequest> TestFixture::createIncomingRequest() {
  auto context = kj::refcounted<IoContext>(
      threadContext, kj::atomicAddRef(*worker), actor, kj::heap<MockLimitEnforcer>());
  auto incomingRequest = kj::heap<IoContext::IncomingRequest>(kj::addRef(*context),
      kj::heap<DummyIoChannelFactory>(*timerChannel), kj::refcounted<RequestObserver>(), kj::none,
      kj::none);
  incomingRequest->delivered();
  return incomingRequest;
}

TestFixture::Response TestFixture::runRequest(
    kj::HttpMethod method, kj::StringPtr url, kj::StringPtr body) {
  kj::HttpHeaders requestHeaders(*headerTable);
  MockResponse response;
  auto requestBody = newMemoryInputStream(body);

  runInIoContext([&](const TestFixture::Environment& env) {
    auto& globalScope = env.lock.getGlobalScope();
    return globalScope.request(method, url, requestHeaders, *requestBody, response, "{}"_kj,
        env.lock, env.lock.getExportedHandler(kj::none, {}, kj::none), /* abortSignal */ kj::none);
  });

  return {.statusCode = response.statusCode, .body = response.body->str()};
}

}  // namespace workerd
