// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "worker-factory-impl.h"

#include <workerd/api/analytics-engine.capnp.h>
#include <workerd/api/trace.h>
#include <workerd/io/access-info.h>
#include <workerd/io/bundle-fs.h>
#include <workerd/io/compatibility-date.h>
#include <workerd/io/features.h>
#include <workerd/io/io-context.h>
#include <workerd/io/limit-enforcer.h>
#include <workerd/io/trace-stream.h>
#include <workerd/io/tracer.h>
#include <workerd/io/worker-entrypoint.h>
#include <workerd/io/worker-fs.h>
#include <workerd/jsg/snapshot.h>
#include <workerd/server/actor-id-impl.h>
#include <workerd/server/compiled-bindings.capnp.h>
#include <workerd/server/fallback-service.h>
#include <workerd/server/pyodide.h>
#include <workerd/server/workerd-api.h>
#include <workerd/util/autogate.h>
#include <workerd/util/strings.h>
#include <workerd/util/use-perfetto-categories.h>

#include <capnp/compat/json.h>
#include <kj/encoding.h>

namespace workerd::server {

// Builds the header table once the headers the config's HttpOptions name (injected,
// forwarded-proto and cf blob headers: external servers' first, then sockets') are in it. kj
// writes a table's headers ahead of the others, in the table's order and spelling, so these reach
// the wire as the config spells them. The names point into the config message.
static kj::Own<kj::HttpHeaderTable> buildHeaderTable(
    kj::HttpHeaderTable::Builder& builder, config::Config::Reader config) {
  auto add = [&](config::HttpOptions::Reader options) {
    for (auto header: options.getInjectRequestHeaders()) builder.add(header.getName());
    for (auto header: options.getInjectResponseHeaders()) builder.add(header.getName());
    if (options.hasForwardedProtoHeader()) builder.add(options.getForwardedProtoHeader());
    if (options.hasCfBlobHeader()) builder.add(options.getCfBlobHeader());
  };
  for (auto service: config.getServices()) {
    if (!service.isExternal()) continue;
    auto external = service.getExternal();
    if (external.isHttp()) add(external.getHttp());
    if (external.isHttps()) add(external.getHttps().getOptions());
  }
  for (auto socket: config.getSockets()) {
    if (socket.isHttp()) add(socket.getHttp());
    if (socket.isHttps()) add(socket.getHttps().getOptions());
  }
  return builder.build();
}

// =======================================================================================
// WorkerFactory

WorkerFactory::Impl::Impl(WorkerFactory& factory,
    jsg::V8System& v8System,
    kj::Timer& timer,
    const kj::MonotonicClock& monotonicClock,
    kj::Network& network,
    kj::EntropySource& entropySource,
    kj::Filesystem& fs,
    kj::Own<Options> options,
    ::rust::Vec<uint64_t> config)
    : v8System(v8System),
      timer(timer),
      monotonicClock(monotonicClock),
      network(network),
      entropySource(entropySource),
      fs(fs),
      options(kj::mv(options)),
      configMessage(kj::mv(config)),
      configReader(asWords(kj::from<Rust>(configMessage)), CONFIG_READER_OPTIONS),
      config(configReader.getRoot<config::Config>()),
      httpOverCapnpFactory(
          byteStreamFactory, headerTableBuilder, capnp::HttpOverCapnpFactory::LEVEL_2),
      threadContext(
          timer, entropySource, headerTableBuilder, httpOverCapnpFactory, byteStreamFactory),
      headerTable(buildHeaderTable(headerTableBuilder, this->config)),
      memoryCacheProvider(kj::heap<api::MemoryCacheProvider>()),
      channelTokenHandler(factory),
      tasks(*this) {
  // The config's logging settings go on top of the caller's.
  auto& logging = this->options->loggingOptions;
  auto& root = this->config;
  if (root.hasLogging()) {
    auto conf = root.getLogging();
    logging.structuredLogging = StructuredLogging(conf.getStructuredLogging());
    if (conf.hasStdoutPrefix()) {
      logging.stdoutPrefix = kj::ConstString(kj::str(conf.getStdoutPrefix()));
    }
    if (conf.hasStderrPrefix()) {
      logging.stderrPrefix = kj::ConstString(kj::str(conf.getStderrPrefix()));
    }
  } else {
    logging.structuredLogging = StructuredLogging(root.getStructuredLogging());
  }
}

const ServerHandle& WorkerFactory::Impl::getServer() const {
  return *KJ_REQUIRE_NONNULL(server, "the server has not registered with the worker factory");
}

void WorkerFactory::Impl::taskFailed(kj::Exception&& exception) {
  KJ_LOG(ERROR, exception);
}

WorkerFactory::WorkerFactory(jsg::V8System& v8System,
    kj::Timer& timer,
    const kj::MonotonicClock& monotonicClock,
    kj::Network& network,
    kj::EntropySource& entropySource,
    kj::Filesystem& fs,
    kj::Own<Options> options,
    ::rust::Vec<uint64_t> config)
    : impl(kj::heap<Impl>(*this,
          v8System,
          timer,
          monotonicClock,
          network,
          entropySource,
          fs,
          kj::mv(options),
          kj::mv(config))) {}

WorkerFactory::~WorkerFactory() noexcept(false) = default;

void factory_spawn(const WorkerFactory& factory, ::rust::Box<SpawnedTask> task) {
  auto& impl = factory.getImpl();
  ++impl.spawnCount;
  // The task starts on the next turn of the event loop, not inside the spawn: a stub's destructor
  // spawns the unlink of its worker precisely because that cannot run where the stub is dropped.
  impl.tasks.add(kj::evalLater(
      [task = kj::mv(task)]() mutable -> kj::Promise<void> { return task_run(kj::mv(task)); }));
}
void factory_clear_tasks(const WorkerFactory& factory) {
  factory.getImpl().tasks.clear();
}
kj::Promise<void> factory_settle_tasks(const WorkerFactory& factory) {
  auto& impl = factory.getImpl();
  // Dropping the server spawns tasks that do deferred work (a dropped stub unlinks its worker on
  // the next turn), which may spawn more; every turn's spawns run before the tasks are dropped.
  uint64_t seen;
  do {
    seen = impl.spawnCount;
    co_await kj::evalLater([]() {});
  } while (impl.spawnCount != seen);
  impl.tasks.clear();
}
uint64_t factory_timer_now(const WorkerFactory& factory) {
  return (factory.getImpl().timer.now() - kj::origin<kj::TimePoint>()) / kj::NANOSECONDS;
}
kj::Promise<void> factory_sleep(const WorkerFactory& factory, uint64_t nanos) {
  return factory.getImpl().timer.afterDelay(nanos * kj::NANOSECONDS);
}
WorkerFactory::Impl& WorkerFactory::getImpl() const {
  return *impl;
}

kj::Rc<IoChannelFactory::SubrequestChannel> WorkerFactory::resolveEntrypoint(
    kj::StringPtr serviceName,
    kj::Maybe<kj::StringPtr> entrypoint,
    Frankenvalue props,
    Persistent persistent) {
  return subrequest_channel_into_kj(impl->getServer().resolve_entrypoint(
      toRust(serviceName), toRust(entrypoint), toOwn(kj::mv(props)), persistent.toBool()));
}

kj::Rc<IoChannelFactory::ActorClassChannel> WorkerFactory::resolveActorClass(
    kj::StringPtr serviceName,
    kj::Maybe<kj::StringPtr> entrypoint,
    Frankenvalue props,
    Persistent persistent) {
  return actor_class_channel_into_kj(impl->getServer().resolve_actor_class(
      toRust(serviceName), toRust(entrypoint), toOwn(kj::mv(props)), persistent.toBool()));
}

kj::Rc<IoChannelFactory::ActorChannel> WorkerFactory::resolveActor(kj::StringPtr namespaceKey,
    kj::ArrayPtr<const byte> id,
    kj::Maybe<kj::StringPtr> name,
    Persistent persistent) {
  auto idObj = kj::heap<ActorIdHandle>(
      normalizeActorId(ActorIdFactoryImpl(namespaceKey).idFromRaw(id, name.clone())));
  return subrequest_channel_into_kj(
      impl->getServer().resolve_actor(toRust(namespaceKey), kj::mv(idObj), persistent.toBool()));
}

::rust::Slice<const uint8_t> factory_config(const WorkerFactory& factory) {
  return kj::from<Rust>(factory.getImpl().configMessage).asBytes().as<Rust>();
}
const kj::HttpHeaderTable& factory_header_table(const WorkerFactory& factory) {
  return *factory.getImpl().headerTable;
}

namespace {
// The encoders take props by mutable reference for the sake of channels they refcount; the
// props are otherwise unchanged.
Frankenvalue& propsRef(kj::Maybe<const Frankenvalue&> props) {
  static Frankenvalue EMPTY_PROPS;
  KJ_IF_SOME(p, props) return const_cast<Frankenvalue&>(p);
  return EMPTY_PROPS;
}
}  // namespace

kj::Own<PendingToken> factory_encode_subrequest_token(const WorkerFactory& factory,
    ::rust::Str serviceName,
    kj::Maybe<::rust::Str> entrypoint,
    kj::Maybe<const Frankenvalue&> props,
    bool persistent,
    TokenUsage usage) {
  auto service = kj::str(serviceName);
  auto ep = toKj(entrypoint);
  return kj::heap<PendingToken>(factory.getImpl().channelTokenHandler.encodeSubrequestChannelToken(
      usage, service, ep.map([](kj::String& e) -> kj::StringPtr { return e; }), propsRef(props),
      Persistent(persistent)));
}

kj::Own<PendingToken> factory_encode_actor_class_token(const WorkerFactory& factory,
    ::rust::Str serviceName,
    ::rust::Str className,
    kj::Maybe<const Frankenvalue&> props,
    bool persistent,
    TokenUsage usage) {
  auto service = kj::str(serviceName);
  auto cls = kj::str(className);
  return kj::heap<PendingToken>(factory.getImpl().channelTokenHandler.encodeActorClassChannelToken(
      usage, service, cls.asPtr(), propsRef(props), Persistent(persistent)));
}

kj::Own<PendingToken> factory_encode_actor_token(const WorkerFactory& factory,
    ::rust::Str uniqueKey,
    const ActorIdHandle& id,
    bool persistent,
    TokenUsage usage) {
  auto& abstractId = *KJ_REQUIRE_NONNULL(
      id.tryGet<kj::Own<ActorIdFactory::ActorId>>(), "only durable actors have channel tokens");
  auto& idImpl =
      KJ_ASSERT_NONNULL(kj::tryDowncast<const ActorIdFactoryImpl::ActorIdImpl>(abstractId));
  return kj::heap<PendingToken>(factory.getImpl().channelTokenHandler.encodeActorChannelToken(
      usage, kj::str(uniqueKey), idImpl.getRaw(), idImpl.getName(), Persistent(persistent)));
}

void factory_set_server(const WorkerFactory& factory, ::rust::Box<ServerHandle> server) {
  factory.getImpl().server = kj::mv(server);
}

bool factory_experimental(const WorkerFactory& factory) {
  return factory.getImpl().options->experimental;
}

Worker::Actor::Id normalizeActorId(Worker::Actor::Id id) {
  KJ_IF_SOME(doId, id.tryGet<kj::Own<ActorIdFactory::ActorId>>()) {
    KJ_IF_SOME(name, doId->getName()) {
      if (name.size() > 1024) {
        KJ_ASSERT_NONNULL(kj::tryDowncast<ActorIdFactoryImpl::ActorIdImpl>(*doId)).clearName();
      }
    }
  }
  return kj::mv(id);
}

// =======================================================================================
// Single-tenant policy: no limits, and tracing that feeds tail workers directly.

namespace {

class NullIsolateLimitEnforcer final: public IsolateLimitEnforcer {
 public:
  v8::Isolate::CreateParams getCreateParams() override {
    return {};
  }
  void customizeIsolate(v8::Isolate* isolate) override {}
  ActorCacheSharedLruOptions getActorCacheLruOptions() override {
    return {.softLimit = 16 * (1ull << 20),
      .hardLimit = 128 * (1ull << 20),
      .staleTimeout = 30 * kj::SECONDS,
      .dirtyListByteLimit = 8 * (1ull << 20),
      .maxKeysPerRpc = 128,
      // In-memory-only actors: the cache never flushes to its (empty) backing storage.
      .neverFlush = true};
  }
  kj::Own<void> enterStartupJs(jsg::Lock&, kj::OneOf<kj::Exception, kj::Duration>&) const override {
    return {};
  }
  kj::Own<void> enterStartupPython(
      jsg::Lock&, kj::OneOf<kj::Exception, kj::Duration>&) const override {
    return {};
  }
  kj::Own<void> enterDynamicImportJs(
      jsg::Lock&, kj::OneOf<kj::Exception, kj::Duration>&) const override {
    return {};
  }
  kj::Own<void> enterLoggingJs(jsg::Lock&, kj::OneOf<kj::Exception, kj::Duration>&) const override {
    return {};
  }
  kj::Own<void> enterInspectorJs(
      jsg::Lock&, kj::OneOf<kj::Exception, kj::Duration>&) const override {
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
  const TrackedWasmInstanceList& getTrackedWasmInstances() const override {
    return trackedWasmInstances;
  }

 private:
  TrackedWasmInstanceList trackedWasmInstances;
};

class NullLimitEnforcer final: public LimitEnforcer, public kj::Refcounted {
 public:
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
  size_t getSqliteMemoryUsage() const override {
    return 0;
  }
};

// Records a request's outcome on its tracer, wrapping the WorkerInterface to observe failures.
class RequestObserverWithTracer final: public RequestObserver, public WorkerInterface {
 public:
  explicit RequestObserverWithTracer(kj::Maybe<kj::Rc<WorkerTracer>> tracer)
      : tracer(kj::mv(tracer)) {}

  ~RequestObserverWithTracer() noexcept(false) {
    KJ_IF_SOME(t, tracer) {
      KJ_IF_SOME(ioContext, IoContext::tryCurrent()) {
        t->recordTimestamp(ioContext.now());
      }
      t->setOutcome(outcome, 0 * kj::MILLISECONDS, 0 * kj::MILLISECONDS);
    }
  }

  WorkerInterface& wrapWorkerInterface(WorkerInterface& worker) override {
    if (tracer != kj::none) {
      inner = worker;
      return *this;
    }
    return worker;
  }

  void reportFailure(
      const kj::Exception& exception, FailureSource source = FailureSource::OTHER) override {
    if (outcome == EventOutcome::OK) {
      outcome = RequestObserver::outcomeFromException(exception, source);
    }
  }

  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      kj::HttpService::Response& response) override {
    co_return co_await observe(
        KJ_ASSERT_NONNULL(inner).request(method, url, headers, requestBody, response));
  }
  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::HttpConnectSettings settings) override {
    co_return co_await observe(
        KJ_ASSERT_NONNULL(inner).connect(host, headers, connection, response, settings));
  }
  kj::Promise<void> prewarm(kj::StringPtr url) override {
    co_return co_await observe(KJ_ASSERT_NONNULL(inner).prewarm(url));
  }
  kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override {
    auto result = co_await observe(KJ_ASSERT_NONNULL(inner).runScheduled(scheduledTime, cron));
    if (outcome == EventOutcome::OK) outcome = result.outcome;
    co_return result;
  }
  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime, uint32_t retryCount) override {
    auto result = co_await observe(KJ_ASSERT_NONNULL(inner).runAlarm(scheduledTime, retryCount));
    if (outcome == EventOutcome::OK) outcome = result.outcome;
    co_return result;
  }
  kj::Promise<bool> test() override {
    co_return co_await observe(KJ_ASSERT_NONNULL(inner).test());
  }
  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override {
    auto result = co_await observe(KJ_ASSERT_NONNULL(inner).customEvent(kj::mv(event)));
    if (outcome == EventOutcome::OK) outcome = result.outcome;
    co_return result;
  }
  kj::Promise<kj::Maybe<kj::Date>> abandonAlarm(kj::Date scheduledTime) override {
    co_return co_await KJ_ASSERT_NONNULL(inner).abandonAlarm(scheduledTime);
  }

 private:
  kj::Maybe<kj::Rc<WorkerTracer>> tracer;
  kj::Maybe<WorkerInterface&> inner;
  EventOutcome outcome = EventOutcome::OK;

  // Reports the promise's failure, if any, before propagating it.
  template <typename T>
  kj::Promise<T> observe(kj::Promise<T> promise) {
    try {
      co_return co_await promise;
    } catch (...) {
      auto exception = kj::getCaughtExceptionAsKj();
      reportFailure(exception);
      kj::throwFatalException(kj::mv(exception));
    }
  }
};

class SequentialSpanSubmitter final: public SpanSubmitter {
 public:
  SequentialSpanSubmitter(kj::Own<BaseTracer::WeakRef> weakTracer, kj::EntropySource& entropySource)
      : weakTracer(kj::mv(weakTracer)),
        entropySource(entropySource) {}
  KJ_DISALLOW_COPY_AND_MOVE(SequentialSpanSubmitter);

  void submitSpanClose(
      tracing::SpanId spanId, kj::Date startTime, kj::Date endTime, Span::TagMap&& tags) override {
    weakTracer->runIfAlive([&](BaseTracer& tracer) {
      tracing::SpanEndData spanEnd(spanId, endTime, kj::mv(tags));
      if (isPredictableModeForTest()) {
        startTime = spanEnd.endTime = kj::UNIX_EPOCH;
      }
      tracer.addSpanClose(kj::mv(spanEnd), startTime);
    });
  }

  void submitSpanUpdate(tracing::SpanId spanId, tracing::SpanUpdate&& update) override {
    weakTracer->runIfAlive(
        [&](BaseTracer& tracer) { tracer.addSpanUpdate(spanId, kj::mv(update)); });
  }

  void submitSpanException(tracing::SpanId spanId,
      kj::Date timestamp,
      kj::Maybe<tracing::Exception::Code> code,
      kj::String name,
      kj::String message,
      kj::Maybe<kj::String> stack) override {
    weakTracer->runIfAlive([&](BaseTracer& tracer) {
      if (isPredictableModeForTest()) {
        timestamp = kj::UNIX_EPOCH;
      }
      tracer.addSpanException(
          spanId, timestamp, kj::mv(code), kj::mv(name), kj::mv(message), kj::mv(stack));
    });
  }

  bool submitSpanOpen(tracing::SpanId spanId,
      tracing::SpanId parentSpanId,
      kj::ConstString operationName,
      kj::Date startTime) override {
    bool submitted = false;
    weakTracer->runIfAlive([&](BaseTracer& tracer) {
      if (isPredictableModeForTest()) {
        startTime = kj::UNIX_EPOCH;
      }
      tracer.addSpanOpen(spanId, parentSpanId, kj::mv(operationName), startTime);
      submitted = true;
    });
    return submitted;
  }

  tracing::SpanId makeSpanId() override {
    if (isPredictableModeForTest()) {
      return tracing::SpanId(nextSpanId++);
    }
    return tracing::SpanId::fromEntropy(entropySource);
  }

 private:
  uint64_t nextSpanId = 1;
  kj::Own<BaseTracer::WeakRef> weakTracer;
  kj::EntropySource& entropySource;
};

}  // namespace

// =======================================================================================
// The server's channels as KJ channels

SubrequestChannelHandle::SubrequestChannelHandle(::rust::Box<server::SubrequestChannel> channel)
    : channel(kj::mv(channel)) {}

kj::Own<WorkerInterface> SubrequestChannelHandle::startRequest(
    IoChannelFactory::SubrequestMetadata metadata) {
  return channel->start_request(kj::heap<RequestMetadata>(kj::mv(metadata)));
}
kj::Promise<void> SubrequestChannelHandle::evictForTest(IoChannelFactory::EvictWebSocketMode mode) {
  return channel->evict_for_test(mode == IoChannelFactory::EvictWebSocketMode::HIBERNATE);
}
void SubrequestChannelHandle::requireAllowsTransfer() {
  channel->require_allows_transfer();
}
kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> SubrequestChannelHandle::getTokenMaybeSync(
    IoChannelFactory::ChannelTokenUsage usage) {
  return kj::mv(*channel->token(usage));
}

ActorClassChannelHandle::ActorClassChannelHandle(::rust::Box<server::ActorClassChannel> channel)
    : channel(kj::mv(channel)) {}

kj::Maybe<::rust::Box<server::ActorClassChannel>> ActorClassChannelHandle::tryUnwrap(
    IoChannelFactory::ActorClassChannel& channel) {
  KJ_IF_SOME(handle, kj::tryDowncast<ActorClassChannelHandle>(channel)) {
    return handle.channel->actor_class_channel_clone();
  }
  return kj::none;
}

void ActorClassChannelHandle::requireAllowsTransfer() {
  channel->require_allows_transfer();
}
kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> ActorClassChannelHandle::getTokenMaybeSync(
    IoChannelFactory::ChannelTokenUsage usage) {
  return kj::mv(*channel->token(usage));
}

kj::Rc<SubrequestChannelHandle> subrequest_channel_into_kj(::rust::Box<SubrequestChannel> channel) {
  return kj::rc<SubrequestChannelHandle>(kj::mv(channel));
}
kj::Rc<ActorClassChannelHandle> actor_class_channel_into_kj(
    ::rust::Box<ActorClassChannel> channel) {
  return kj::rc<ActorClassChannelHandle>(kj::mv(channel));
}

// =======================================================================================
// Requests

namespace {

// A dynamic worker's stub as the runtime's `WorkerStubChannel`.
class WorkerStubChannelHandle final: public WorkerStubChannel {
 public:
  explicit WorkerStubChannelHandle(::rust::Box<WorkerStub> stub): stub(kj::mv(stub)) {}

  kj::Rc<IoChannelFactory::SubrequestChannel> getEntrypointResolved(
      kj::Maybe<kj::String> name, Frankenvalue props, kj::Maybe<ResourceLimits> limits) override {
    return subrequest_channel_into_kj(stub->entrypoint(toRust(name), toOwn(kj::mv(props))));
  }
  kj::Rc<IoChannelFactory::ActorClassChannel> getActorClassResolved(
      kj::Maybe<kj::String> name, Frankenvalue props, kj::Maybe<ResourceLimits> limits) override {
    return actor_class_channel_into_kj(stub->actor_class(toRust(name), toOwn(kj::mv(props))));
  }

 private:
  ::rust::Box<WorkerStub> stub;

  static kj::Maybe<::rust::Str> toRust(kj::Maybe<kj::String>& name) {
    return name.map([](kj::String& n) { return server::toRust(n); });
  }
};

class CacheHttpClientImpl final: public kj::HttpClient {
 public:
  CacheHttpClientImpl(IoChannelFactory::SubrequestChannel& parent,
      kj::HttpHeaderId cacheNamespaceHeader,
      kj::Maybe<kj::String> cacheName,
      kj::Maybe<kj::String> cfBlobJson,
      SpanParent parentSpan)
      : client(asHttpClient(parent.startRequest({kj::mv(cfBlobJson), kj::mv(parentSpan)}))),
        cacheName(kj::mv(cacheName)),
        cacheNamespaceHeader(cacheNamespaceHeader) {}

  Request request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize = kj::none) override {
    auto headersCopy = headers.cloneShallow();
    KJ_IF_SOME(name, cacheName) {
      headersCopy.setPtr(cacheNamespaceHeader, name);
    }
    return client->request(method, url, headersCopy, expectedBodySize);
  }

 private:
  kj::Own<kj::HttpClient> client;
  kj::Maybe<kj::String> cacheName;
  kj::HttpHeaderId cacheNamespaceHeader;
};

class CacheClientImpl final: public CacheClient {
 public:
  CacheClientImpl(kj::Rc<IoChannelFactory::SubrequestChannel> cacheService,
      kj::HttpHeaderId cacheNamespaceHeader)
      : cacheService(kj::mv(cacheService)),
        cacheNamespaceHeader(cacheNamespaceHeader) {}

  kj::Own<kj::HttpClient> getDefault(CacheClient::SubrequestMetadata metadata) override {
    return kj::heap<CacheHttpClientImpl>(*cacheService, cacheNamespaceHeader, kj::none,
        kj::mv(metadata.cfBlobJson), kj::mv(metadata.parentSpan));
  }
  kj::Own<kj::HttpClient> getNamespace(
      kj::StringPtr cacheName, CacheClient::SubrequestMetadata metadata) override {
    return kj::heap<CacheHttpClientImpl>(*cacheService, cacheNamespaceHeader,
        kj::encodeUriComponent(cacheName), kj::mv(metadata.cfBlobJson),
        kj::mv(metadata.parentSpan));
  }

 private:
  kj::Rc<IoChannelFactory::SubrequestChannel> cacheService;
  kj::HttpHeaderId cacheNamespaceHeader;
};

// Access info from the JSON blob of the worker's `accessBlobHeader`:
// `{"app_aud": "...", "jwt_claims": {...}}`.
class BlobAccessInfo final: public AccessInfo {
 public:
  BlobAccessInfo(kj::String audience,
      kj::Maybe<kj::String> jwtClaimsJson,
      kj::Maybe<kj::uint> identityServiceChannel)
      : audience(kj::mv(audience)),
        jwtClaimsJson(kj::mv(jwtClaimsJson)),
        identityServiceChannel(identityServiceChannel) {}

  kj::StringPtr getAudience() override {
    return audience;
  }
  kj::Maybe<kj::uint> getIdentityServiceChannel() override {
    return identityServiceChannel;
  }
  // The props of the identity binding: the audience and the claims.
  kj::String getPropsJson() {
    capnp::JsonCodec codec;
    auto escapedAud = codec.encode(capnp::Text::Reader(audience));
    KJ_IF_SOME(claims, jwtClaimsJson) {
      return kj::str("{\"aud\":", escapedAud, ",\"jwtClaims\":", claims, "}");
    }
    return kj::str("{\"aud\":", escapedAud, "}");
  }

 private:
  kj::String audience;
  kj::Maybe<kj::String> jwtClaimsJson;
  kj::Maybe<kj::uint> identityServiceChannel;
};

kj::Own<AccessInfo> parseAccessBlob(
    kj::StringPtr json, kj::Maybe<kj::uint> identityServiceChannel) {
  capnp::JsonCodec jsonCodec;
  capnp::MallocMessageBuilder arena;
  auto root = arena.initRoot<capnp::JsonValue>();
  jsonCodec.decodeRaw(json, root);
  KJ_REQUIRE(root.isObject(), "accessBlobHeader value must be a JSON object");

  kj::Maybe<kj::String> appAud;
  kj::Maybe<kj::String> jwtClaimsJson;
  for (auto field: root.getObject()) {
    auto name = field.getName();
    if (name == "app_aud") {
      KJ_REQUIRE(field.getValue().isString(), "access blob `app_aud` must be a string");
      appAud = kj::str(field.getValue().getString());
    } else if (name == "jwt_claims") {
      KJ_REQUIRE(field.getValue().isObject(), "access blob `jwt_claims` must be a JSON object");
      jwtClaimsJson = jsonCodec.encodeRaw(field.getValue());
    }
  }
  auto audience =
      KJ_REQUIRE_NONNULL(kj::mv(appAud), "accessBlobHeader JSON must contain an `app_aud` field");
  return kj::refcounted<BlobAccessInfo>(
      kj::mv(audience), kj::mv(jwtClaimsJson), identityServiceChannel);
}

// The `IoChannelFactory` of one request: the worker's Rust channel table, plus what stays in
// C++ (a dynamic worker's env tables, channel tokens, the debug port).
class RustIoChannelFactory final: public IoChannelFactory {
 public:
  RustIoChannelFactory(kj::Own<CompiledWorker::Impl> worker, ::rust::Box<ChannelFactory> channels)
      : worker(kj::mv(worker)),
        channels(kj::mv(channels)) {}

  ~RustIoChannelFactory() noexcept(false) {
    // A request's drain task runs in the worker's own task set and may hold the worker's last
    // references (the server dropped the worker meanwhile; `channels` may hold the Rust side's,
    // and drops it before this member), so destroying the worker here would destroy the task set
    // from inside one of its tasks. The factory's tasks drop this reference on a later turn.
    worker->factory.getImpl().tasks.add(kj::evalLater([worker = kj::mv(worker)]() {}));
  }

  kj::Own<WorkerInterface> startSubrequest(uint channel, SubrequestMetadata metadata) override {
    KJ_IF_SOME(env, envSubrequestChannel(channel)) return env.startRequest(kj::mv(metadata));
    // The access binding gets the request's identity as props, as the production embedder does
    // through channel tokens.
    kj::Maybe<kj::Own<Frankenvalue>> props;
    KJ_IF_SOME(accessChannel, channels->access_binding_channel()) {
      if (channel == accessChannel) {
        KJ_IF_SOME(info, IoContext::current().getAccessInfo()) {
          props = toOwn(Frankenvalue::fromJson(kj::downcast<BlobAccessInfo>(info).getPropsJson()));
        }
      }
    }
    return channels->subrequest_channel(channel, kj::mv(props), false)
        ->start_request(kj::heap<RequestMetadata>(kj::mv(metadata)));
  }

  capnp::Capability::Client getCapability(uint channel) override {
    KJ_FAIL_REQUIRE("no capability channels");
  }

  kj::Own<CacheClient> getCache() override {
    return kj::heap<CacheClientImpl>(subrequest_channel_into_kj(channels->cache_channel()),
        worker->factory.getImpl().threadContext.getHeaderIds().cfCacheNamespace);
  }

  TimerChannel& getTimer() override {
    return worker->timerChannel;
  }

  kj::Promise<void> writeLogfwdr(
      uint channel, kj::FunctionParam<void(capnp::AnyPointer::Builder)> buildMessage) override {
    auto& context = IoContext::current();
    auto headers = kj::HttpHeaders(context.getHeaderTable());
    auto client = context.getHttpClient(channel, true, kj::none, "writeLogfwdr"_kjc);
    auto urlStr = kj::str("https://fake-host");

    capnp::MallocMessageBuilder requestMessage;
    auto requestBuilder = requestMessage.initRoot<capnp::AnyPointer>();
    buildMessage(requestBuilder);
    capnp::JsonCodec json;
    auto requestJson = json.encode(requestBuilder.getAs<api::AnalyticsEngineEvent>());

    co_await context.waitForOutputLocks();

    auto innerReq = client->request(kj::HttpMethod::POST, urlStr, headers, requestJson.size());
    auto request = attachToRequest(kj::mv(innerReq), kj::Rc<kj::HttpClient>(kj::mv(client)));
    co_await request.body->write(requestJson.asBytes())
        .attach(kj::mv(requestJson), kj::mv(request.body));
    auto response = co_await request.response;
    KJ_REQUIRE(response.statusCode >= 200 && response.statusCode < 300,
        "writeLogfwdr request returned an error");
    co_await response.body->readAllBytes().attach(kj::mv(response.body));
  }

  kj::Rc<SubrequestChannel> getSubrequestChannelResolved(uint channel,
      kj::Maybe<Frankenvalue> props,
      kj::Maybe<VersionRequest> versionRequest,
      Persistent persistent) override {
    KJ_IF_SOME(env, envSubrequestChannel(channel)) {
      // Only a `ctx.exports` template takes props; an env channel is a concrete one.
      KJ_REQUIRE(props == kj::none, "can't override props for this service");
      return env.addRef();
    }
    return subrequest_channel_into_kj(
        channels->subrequest_channel(channel, toOwn(kj::mv(props)), persistent.toBool()));
  }

  kj::Rc<ActorChannel> getGlobalActor(uint channel,
      const ActorIdFactory::ActorId& id,
      kj::Maybe<kj::String> locationHint,
      ActorGetMode mode,
      bool enableReplicaRouting,
      ActorRoutingMode routingMode,
      SpanParent parentSpan,
      kj::Maybe<ActorVersion> version,
      Persistent persistent) override {
    JSG_REQUIRE(mode == ActorGetMode::GET_OR_CREATE, Error,
        "workerd only supports GET_OR_CREATE mode for getting actor stubs");
    JSG_REQUIRE(!enableReplicaRouting, Error, "workerd does not support replica routing.");
    switch (routingMode) {
      case ActorRoutingMode::PRIMARY_ONLY:
      case ActorRoutingMode::DEFAULT:
        // workerd has only primaries.
        break;
    }
    return subrequest_channel_into_kj(channels->global_actor(
        channel, kj::heap<ActorIdHandle>(normalizeActorId(id.clone())), persistent.toBool()));
  }

  kj::Rc<ActorChannel> getColoLocalActor(
      uint channel, kj::StringPtr id, SpanParent parentSpan) override {
    return subrequest_channel_into_kj(channels->colo_local_actor(channel, toRust(id)));
  }

  kj::Rc<ActorClassChannel> getActorClassResolved(
      uint channel, kj::Maybe<Frankenvalue> props, Persistent persistent) override {
    if (channel < worker->actorClassChannels.size()) {
      KJ_REQUIRE(props == kj::none, "can't override props for this actor class");
      return worker->actorClassChannels[channel]->addRef();
    }
    return actor_class_channel_into_kj(
        channels->actor_class(channel, toOwn(kj::mv(props)), persistent.toBool()));
  }

  kj::Rc<RpcChannel> getRpcChannel(uint channel) override {
    KJ_REQUIRE(channel < worker->rpcChannels.size(), "invalid RPC channel number");
    return worker->rpcChannels[channel]->addRef();
  }

  void abortAllActors(kj::Maybe<kj::Exception&> reason) override {
    channels->abort_all_actors(reason);
  }
  void deleteAllActors(kj::Maybe<kj::Exception&> reason) override {
    channels->delete_all_actors(reason);
  }
  kj::Promise<void> evictAllActorsForTest(EvictWebSocketMode mode) override {
    return channels->evict_all_actors_for_test(mode == EvictWebSocketMode::HIBERNATE)
        .attach(kj::addRef(*this));
  }
  void abortIsolate(kj::StringPtr reason) noexcept override {
    channels->abort_isolate(toRustString(reason));
  }

  kj::Own<WorkerStubChannel> loadIsolate(uint loaderChannel,
      kj::Maybe<kj::String> name,
      kj::Function<kj::Promise<DynamicWorkerSource>()> fetchSource) override {
    auto nameStr = name.map([](kj::String& n) { return toRust(n); });
    return kj::refcounted<WorkerStubChannelHandle>(channels->load_isolate(
        loaderChannel, nameStr, kj::heap<DynamicSource>(kj::mv(fetchSource))));
  }

  kj::Network& getWorkerdDebugPortNetwork() override {
    requireDebugPort();
    return worker->factory.getImpl().network;
  }
  rpc::WorkerdDebugPort::Client getWorkerdDebugPort() override {
    requireDebugPort();
    auto& factory = worker->factory.getImpl();
    return kj::heap<WorkerdDebugPortImpl>(
        factory.getServer().server_clone(), factory.httpOverCapnpFactory);
  }

  kj::Rc<SubrequestChannel> subrequestChannelFromToken(
      ChannelTokenUsage usage, kj::ArrayPtr<const byte> token) override {
    return tokens().decodeSubrequestChannelToken(usage, token);
  }
  kj::Rc<ActorClassChannel> actorClassFromToken(
      ChannelTokenUsage usage, kj::ArrayPtr<const byte> token) override {
    return tokens().decodeActorClassChannelToken(usage, token);
  }
  kj::Rc<RpcChannel> rpcChannelFromToken(
      ChannelTokenUsage usage, kj::ArrayPtr<const byte> token) override {
    return tokens().decodeRpcChannelToken(usage, token);
  }
  kj::Rc<SubrequestChannel> makeRestoredSubrequestChannelResolved(
      kj::Rc<SelfTokenFactory> selfTokenFactory,
      Frankenvalue restoreParams,
      kj::Rc<SubrequestChannel> inner,
      Persistent persistent) override {
    return tokens().makeRestoredSubrequestChannel(
        kj::mv(selfTokenFactory), kj::mv(restoreParams), kj::mv(inner), persistent);
  }
  kj::Rc<RpcChannel> makeRestoredRpcChannelResolved(kj::Rc<SelfTokenFactory> selfTokenFactory,
      Frankenvalue restoreParams,
      Persistent persistent) override {
    return tokens().makeRestoredRpcChannel(
        kj::mv(selfTokenFactory), kj::mv(restoreParams), persistent);
  }

 private:
  kj::Own<CompiledWorker::Impl> worker;
  ::rust::Box<ChannelFactory> channels;

  ChannelTokenHandler& tokens() {
    return worker->factory.getImpl().channelTokenHandler;
  }
  // A dynamic worker's env channel behind `channel`; none when the Rust table serves it.
  kj::Maybe<SubrequestChannel&> envSubrequestChannel(uint channel) {
    if (channel < worker->subrequestChannels.size()) return *worker->subrequestChannels[channel];
    return kj::none;
  }
  void requireDebugPort() {
    KJ_REQUIRE(
        channels->has_debug_port(), "workerdDebugPort binding is not enabled for this worker");
  }
};

// The self-token of a static worker's entrypoint, for `ctx.restore()`.
class StaticServiceSelfTokenFactory final: public ChannelTokenHandler::ServerSelfTokenFactory {
 public:
  StaticServiceSelfTokenFactory(ChannelTokenHandler& tokens,
      kj::String serviceName,
      kj::Maybe<kj::String> entrypoint,
      Persistent persistent,
      Frankenvalue props)
      : tokens(tokens),
        serviceName(kj::mv(serviceName)),
        entrypoint(kj::mv(entrypoint)),
        persistent(persistent),
        props(kj::mv(props)) {}

  kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> getSelfToken(
      IoChannelFactory::ChannelTokenUsage usage) override {
    return tokens.encodeSubrequestChannelToken(usage, serviceName,
        entrypoint.map([](kj::String& e) -> kj::StringPtr { return e; }), props, persistent);
  }

 private:
  ChannelTokenHandler& tokens;
  kj::String serviceName;
  kj::Maybe<kj::String> entrypoint;
  Persistent persistent;
  Frankenvalue props;
};

// Defers creating the entrypoint until the first event, so that an HTTP request's access blob
// header can be parsed into the access info the entrypoint is constructed with. Only `request()`
// carries headers; every other event constructs the entrypoint without access info.
class AccessHeaderExtractor final: public WorkerInterface {
 public:
  AccessHeaderExtractor(kj::String headerName,
      kj::Maybe<kj::uint> identityServiceChannel,
      kj::Function<kj::Own<WorkerInterface>(kj::Maybe<kj::Own<AccessInfo>>)> factory)
      : headerName(kj::mv(headerName)),
        identityServiceChannel(identityServiceChannel),
        factory(kj::mv(factory)) {}

  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      kj::HttpService::Response& response) override {
    kj::Maybe<kj::Own<AccessInfo>> accessInfo;
    headers.forEach([&](kj::StringPtr name, kj::StringPtr value) {
      if (strcaseeq(name, headerName)) accessInfo = parseAccessBlob(value, identityServiceChannel);
    });
    KJ_ASSERT(inner == kj::none, "request() called more than once");
    auto& worker = *inner.emplace(factory(kj::mv(accessInfo)));
    co_await worker.request(method, url, headers, requestBody, response);
  }
  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::HttpConnectSettings settings) override {
    co_await getInner().connect(host, headers, connection, response, kj::mv(settings));
  }
  kj::Promise<void> prewarm(kj::StringPtr url) override {
    co_await getInner().prewarm(url);
  }
  kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override {
    co_return co_await getInner().runScheduled(scheduledTime, cron);
  }
  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime, uint32_t retryCount) override {
    co_return co_await getInner().runAlarm(scheduledTime, retryCount);
  }
  kj::Promise<kj::Maybe<kj::Date>> abandonAlarm(kj::Date scheduledTime) override {
    co_return co_await getInner().abandonAlarm(scheduledTime);
  }
  kj::Promise<bool> test() override {
    co_return co_await getInner().test();
  }
  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override {
    co_return co_await getInner().customEvent(kj::mv(event));
  }

 private:
  kj::String headerName;
  kj::Maybe<kj::uint> identityServiceChannel;
  kj::Function<kj::Own<WorkerInterface>(kj::Maybe<kj::Own<AccessInfo>>)> factory;
  kj::Maybe<kj::Own<WorkerInterface>> inner;

  WorkerInterface& getInner() {
    KJ_IF_SOME(w, inner) return *w;
    return *inner.emplace(factory(kj::none));
  }
};

kj::Own<WorkerInterface> createEntrypoint(CompiledWorker::Impl& worker,
    IoChannelFactory::SubrequestMetadata metadata,
    kj::Maybe<kj::StringPtr> entrypointName,
    Frankenvalue props,
    kj::Maybe<kj::Own<Worker::Actor>> actor,
    ::rust::Box<ChannelFactory> channels,
    ::rust::Box<WorkerInterfaceList> tails,
    kj::Maybe<kj::Own<AccessInfo>> accessInfo) {
  auto& threadContext = worker.factory.getImpl().threadContext;

  // The test event is not traced. A dynamic worker's tails are its source's; the server passes
  // it none.
  kj::Vector<kj::Own<WorkerInterface>> bufferedTailWorkers;
  kj::Vector<kj::Own<WorkerInterface>> streamingTailWorkers;
  if (entrypointName.orDefault("") != "test"_kj) {
    for (size_t i = 0; i < tails->len(); i++) {
      (tails->is_streaming(i) ? streamingTailWorkers : bufferedTailWorkers).add(tails->take(i));
    }
    KJ_IF_SOME(dynamic, worker.dynamicSource) {
      for (auto& tail: dynamic.tails) bufferedTailWorkers.add(tail->startRequest({}));
      for (auto& tail: dynamic.streamingTails) streamingTailWorkers.add(tail->startRequest({}));
    }
  }

  kj::Maybe<kj::Rc<WorkerTracer>> workerTracer;
  if (!bufferedTailWorkers.empty() || !streamingTailWorkers.empty()) {
    auto executionModel =
        actor == kj::none ? ExecutionModel::STATELESS : ExecutionModel::DURABLE_OBJECT;
    kj::Maybe<kj::String> durableObjectId;
    KJ_IF_SOME(a, actor) {
      KJ_SWITCH_ONEOF(a->getId()) {
        KJ_CASE_ONEOF(id, kj::Own<ActorIdFactory::ActorId>) {
          durableObjectId = id->toString();
        }
        KJ_CASE_ONEOF(id, kj::String) {
          durableObjectId = kj::str(id);
        }
      }
    }
    auto tailStreamWriter = tracing::initializeTailStreamWriter(
        streamingTailWorkers.releaseAsArray(), worker.waitUntilTasks);
    auto trace = kj::refcounted<Trace>(kj::none, kj::none, kj::none, kj::none, kj::none, nullptr,
        entrypointName.clone(), executionModel, kj::mv(durableObjectId));
    kj::Rc<WorkerTracer> tracer = kj::rc<WorkerTracer>(
        kj::none, kj::mv(trace), PipelineLogLevel::FULL, kj::none, kj::mv(tailStreamWriter));

    // The buffered tail workers get the trace once it completes. The observer and the IoContext
    // each hold the tracer, so it lives until every event is recorded.
    if (!bufferedTailWorkers.empty()) {
      worker.waitUntilTasks.add(tracer->onComplete().then(
          kj::coCapture([tailWorkers = bufferedTailWorkers.releaseAsArray()](
                            kj::Own<Trace> trace) mutable -> kj::Promise<void> {
        for (auto& tailWorker: tailWorkers) {
          auto event = kj::heap<api::TraceCustomEvent>(
              api::TraceCustomEvent::TYPE, kj::arr(kj::addRef(*trace)));
          co_await tailWorker->customEvent(kj::mv(event));
        }
      })));
    }
    tracer->setMakeUserRequestSpanFunc(
        [&tracerRef = *tracer, &entropySource = threadContext.getEntropySource()](
            tracing::TraceId traceId, kj::Maybe<tracing::TraceFlags> traceFlags) {
      return SpanParent(kj::rc<UserSpanObserver>(
          kj::refcounted<SequentialSpanSubmitter>(tracerRef.getWeakRef(), entropySource),
          kj::mv(traceId), traceFlags));
    });
    workerTracer = kj::mv(tracer);
  }

  kj::Own<RequestObserver> observer =
      kj::refcounted<RequestObserverWithTracer>(workerTracer.clone());

  kj::Maybe<tracing::InvocationSpanContext> triggerContext;
  KJ_IF_SOME(ctx, metadata.userSpanParent.toSpanContext()) {
    KJ_IF_SOME(spanId, ctx.getSpanId()) {
      triggerContext = tracing::InvocationSpanContext(
          ctx.getTraceId(), tracing::TraceId::nullId, spanId, ctx.getTraceFlags());
    }
  }

  auto ioChannelFactory = kj::rc<RustIoChannelFactory>(kj::addRef(worker), kj::mv(channels));
  return newWorkerEntrypoint(threadContext, kj::atomicAddRef(worker.getWorker()),
      entrypointName.clone(), kj::mv(props), kj::mv(actor), kj::refcounted<NullLimitEnforcer>(), {},
      kj::mv(ioChannelFactory), kj::mv(observer), worker.waitUntilTasks, true, kj::mv(workerTracer),
      kj::mv(metadata.cfBlobJson), kj::none, kj::mv(triggerContext), IsDynamicDispatch::NO,
      kj::mv(accessInfo), kj::mv(metadata.restoredSelfTokenFactory), metadata.fromPersistentStub,
      kj::mv(metadata.clientAddress));
}

}  // namespace

kj::Own<WorkerInterface> worker_start_request(const CompiledWorker& worker,
    kj::Maybe<::rust::Str> entrypoint,
    kj::Maybe<kj::Own<Frankenvalue>> props,
    kj::Maybe<const ActorHandle&> actor,
    ::rust::Box<ChannelFactory> channels,
    kj::Own<RequestMetadata> metadata,
    ::rust::Box<WorkerInterfaceList> tails) {
  TRACE_EVENT("workerd", "worker_start_request()");
  auto& impl = worker.getImpl();
  auto entrypointName = toKj(entrypoint);
  Frankenvalue propsValue;
  KJ_IF_SOME(p, props) propsValue = kj::mv(*p);
  auto meta = kj::mv(*metadata);
  // A static worker's entrypoint mints its own self-token: a caller-supplied one could read and
  // manipulate the parameters of the entrypoint's own `[restore]()` method. An actor's requests
  // get theirs from the server instead (the token names the actor, not the class), and nothing
  // could reload a dynamic worker from a token.
  if (!impl.isDynamic && actor == kj::none) {
    meta.restoredSelfTokenFactory = kj::rc<StaticServiceSelfTokenFactory>(
        impl.factory.getImpl().channelTokenHandler, kj::str(impl.name), entrypointName.clone(),
        Persistent(impl.info.persistent_self_tokens), propsValue.clone());
  }
  auto actorRef = actor.map([](const ActorHandle& a) { return a.addRef(); });

  KJ_IF_SOME(headerName, impl.accessBlobHeader) {
    auto identityChannel = channels->access_binding_channel();
    return kj::heap<AccessHeaderExtractor>(kj::str(headerName), identityChannel,
        [&impl, meta = kj::mv(meta), entrypointName = kj::mv(entrypointName),
            props = kj::mv(propsValue), actor = kj::mv(actorRef), channels = kj::mv(channels),
            tails = kj::mv(tails)](kj::Maybe<kj::Own<AccessInfo>> accessInfo) mutable {
      return createEntrypoint(impl, kj::mv(meta), entrypointName, kj::mv(props), kj::mv(actor),
          kj::mv(channels), kj::mv(tails), kj::mv(accessInfo));
    }).attach(kj::addRef(impl));
  }
  return createEntrypoint(impl, kj::mv(meta), entrypointName, kj::mv(propsValue), kj::mv(actorRef),
      kj::mv(channels), kj::mv(tails), kj::none);
}

// =======================================================================================
// Compiling a worker

namespace {

// Sets the globals of an encoded `Globals` message on `target`.
void compileGlobals(jsg::Lock& lock,
    const Worker::Api& api,
    kj::ArrayPtr<const uint64_t> words,
    v8::Local<v8::Object> target) {
  capnp::FlatArrayMessageReader reader(asWords(words), CONFIG_READER_OPTIONS);
  WorkerdApi::from(api).compileGlobals(lock, reader.getRoot<Globals>().getGlobals(), target);
}

}  // namespace

// =======================================================================================
// Value shims

kj::Own<WorkerInterface> worker_interface_attach(
    kj::Own<WorkerInterface> inner, ::rust::Box<KeepAlive> keep) {
  return inner.attach(kj::mv(keep));
}

kj::Own<RequestMetadata> new_request_metadata(
    kj::Maybe<::rust::Str> cfBlobJson, kj::Maybe<::rust::Str> clientAddress) {
  return kj::heap<RequestMetadata>(IoChannelFactory::SubrequestMetadata{
    .cfBlobJson = toKj(cfBlobJson), .clientAddress = toKj(clientAddress)});
}
kj::Maybe<::rust::String> request_metadata_cf_blob_json(const RequestMetadata& metadata) {
  return metadata.cfBlobJson.map([](const kj::String& s) { return toRustString(s); });
}

void request_metadata_set_from_persistent_stub(RequestMetadata& metadata, bool persistent) {
  if (persistent) metadata.fromPersistentStub = Persistent::YES;
}

namespace {

// The self-token of a root actor, for `ctx.restore()`: only the namespace and id, so that
// holding it does not keep the actor from being evicted.
class ActorSelfTokenFactory final: public ChannelTokenHandler::ServerSelfTokenFactory {
 public:
  ActorSelfTokenFactory(ChannelTokenHandler& tokens,
      kj::String uniqueKey,
      kj::Own<ActorIdFactory::ActorId> id,
      Persistent persistent)
      : tokens(tokens),
        uniqueKey(kj::mv(uniqueKey)),
        id(kj::mv(id)),
        persistent(persistent) {}

  kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> getSelfToken(
      IoChannelFactory::ChannelTokenUsage usage) override {
    auto& idImpl = KJ_ASSERT_NONNULL(kj::tryDowncast<ActorIdFactoryImpl::ActorIdImpl>(*id));
    return tokens.encodeActorChannelToken(
        usage, uniqueKey, idImpl.getRaw(), idImpl.getName(), persistent);
  }

 private:
  ChannelTokenHandler& tokens;
  kj::String uniqueKey;
  kj::Own<ActorIdFactory::ActorId> id;
  Persistent persistent;
};

}  // namespace

void request_metadata_set_actor_self_token(const WorkerFactory& factory,
    RequestMetadata& metadata,
    ::rust::Str uniqueKey,
    const ActorIdHandle& id,
    bool persistent) {
  // Ephemeral actors are not serializable, so they have no self-token.
  auto& doId = KJ_UNWRAP_OR(id.tryGet<kj::Own<ActorIdFactory::ActorId>>(), return);
  metadata.restoredSelfTokenFactory =
      kj::rc<ActorSelfTokenFactory>(factory.getImpl().channelTokenHandler, kj::str(uniqueKey),
          doId->clone(), Persistent(persistent));
}

void exception_throw(const kj::Exception& exception) {
  kj::throwFatalException(exception.clone());
}

::rust::String exception_text(const AbortReason& error) {
  // Raising the error across the bridge is the bridge's own KjError -> kj::Exception conversion.
  return kj::str(KJ_ASSERT_NONNULL(kj::runCatchingExceptions([&]() {
    error.raise();
  }))).as<kj_rs::RustCopyUncheckedUtf8>();
}

kj::Own<Frankenvalue> frankenvalue_from_json(::rust::Str json) {
  return kj::heap<Frankenvalue>(Frankenvalue::fromJson(kj::str(json)));
}
kj::Own<Frankenvalue> frankenvalue_clone(const Frankenvalue& value) {
  return kj::heap<Frankenvalue>(const_cast<Frankenvalue&>(value).clone());
}
bool frankenvalue_is_empty(const Frankenvalue& value) {
  return value.empty();
}
kj::Own<Frankenvalue> frankenvalue_new() {
  return kj::heap<Frankenvalue>();
}
void frankenvalue_set_service_stub(
    Frankenvalue& value, ::rust::Str name, ::rust::Box<SubrequestChannel> channel) {
  value.setProperty(kj::str(name),
      Frankenvalue::fromCapability(static_cast<uint16_t>(rpc::SerializationTag::SERVICE_STUB),
          subrequest_channel_into_kj(kj::mv(channel)).toOwn()));
}

kj::Own<ActorIdHandle> actor_id_clone(const ActorIdHandle& id) {
  return kj::heap<ActorIdHandle>(Worker::Actor::cloneId(const_cast<ActorIdHandle&>(id)));
}
::rust::String actor_id_key(const ActorIdHandle& id) {
  KJ_SWITCH_ONEOF(id) {
    KJ_CASE_ONEOF(obj, kj::Own<ActorIdFactory::ActorId>) {
      return toRustString(obj->toString());
    }
    KJ_CASE_ONEOF(str, kj::String) {
      return toRustString(str);
    }
  }
  KJ_UNREACHABLE;
}
kj::Own<ActorIdHandle> actor_id_from_name(::rust::Str name) {
  return kj::heap<ActorIdHandle>(kj::str(name));
}
kj::Own<ActorIdHandle> actor_id_from_hex(::rust::Str hex) {
  auto decoded = kj::decodeHex(kj::str(hex));
  KJ_REQUIRE(decoded.size() == SHA256_DIGEST_LENGTH,
      "Invalid Durable Object ID: expected 64 hex characters (32 bytes)", decoded.size());
  return kj::heap<ActorIdHandle>(kj::Own<ActorIdFactory::ActorId>(
      kj::heap<ActorIdFactoryImpl::ActorIdImpl>(decoded.begin(), kj::none)));
}

// =======================================================================================
// CompiledWorker

CompiledWorker::Impl::Impl(const WorkerFactory& factory, kj::String name)
    : factory(factory),
      name(kj::mv(name)),
      timerChannel(factory.getImpl().timer, factory.getImpl().monotonicClock),
      waitUntilTasks(*this) {}

void CompiledWorker::Impl::taskFailed(kj::Exception&& exception) {
  KJ_LOG(ERROR, exception);
}

CompiledWorker::CompiledWorker(kj::Own<Impl> impl): impl(kj::mv(impl)) {}
CompiledWorker::~CompiledWorker() noexcept(false) = default;
CompiledWorker::Impl& CompiledWorker::getImpl() const {
  return *impl;
}

namespace {

// Collects what validation reveals about a worker: its entrypoints and classes, and its errors.
struct ErrorReporter final: public Worker::ValidationErrorReporter {
  kj::Vector<kj::String> errors;
  kj::Vector<kj::String> warnings;
  kj::Vector<EntrypointInfo> entrypoints;
  kj::Vector<::rust::String> actorClasses;
  kj::Vector<::rust::String> workflowClasses;

  void addError(kj::String error) override {
    errors.add(kj::mv(error));
  }
  void addWarning(kj::String warning) override {
    warnings.add(kj::mv(warning));
  }
  void addEntrypoint(kj::Maybe<kj::StringPtr> exportName, kj::Array<kj::String> methods) override {
    entrypoints.add(EntrypointInfo{
      .name = toRustString(exportName.orDefault(""_kj)),
      .is_default = exportName == kj::none,
      .handlers = toRust(methods),
    });
  }
  void addActorClass(kj::StringPtr exportName) override {
    actorClasses.add(toRustString(exportName));
  }
  void addWorkflowClass(kj::StringPtr exportName, kj::Array<kj::String> methods) override {
    // A workflow class is a stateless entrypoint at runtime.
    entrypoints.add(EntrypointInfo{
      .name = toRustString(exportName), .is_default = false, .handlers = toRust(methods)});
    workflowClasses.add(toRustString(exportName));
  }

  static ::rust::Vec<::rust::String> toRust(kj::ArrayPtr<kj::String> strings) {
    ::rust::Vec<::rust::String> result;
    result.reserve(strings.size());
    for (auto& s: strings) result.push_back(toRustString(s));
    return result;
  }
  static ::rust::Vec<::rust::String> toRust(kj::Vector<kj::String>& strings) {
    return toRust(strings.asPtr());
  }
  template <typename T>
  static ::rust::Vec<T> toRust(kj::Vector<T>& values) {
    ::rust::Vec<T> result;
    result.reserve(values.size());
    for (auto& v: values) result.push_back(kj::mv(v));
    return result;
  }
};

MainModuleIsPython isPythonMainModule(config::Worker::Reader conf) {
  if (!conf.isModules() || conf.getModules().size() == 0) return MainModuleIsPython::NO;
  return conf.getModules()[0].isPythonModule() ? MainModuleIsPython::YES : MainModuleIsPython::NO;
}

// Compiles a config worker's compatibility flags into `arena`, as the worker's errors.
CompatibilityFlags::Reader compileFlags(const WorkerFactory::Options& options,
    config::Worker::Reader conf,
    capnp::MallocMessageBuilder& arena,
    ErrorReporter& errorReporter) {
  auto flags = arena.initRoot<CompatibilityFlags>();
  KJ_IF_SOME(overrideDate, options.testCompatibilityDateOverride) {
    if (conf.hasCompatibilityDate()) {
      errorReporter.addError(
          kj::str("Worker specifies compatibilityDate but --compat-date was provided. "
                  "When using --compat-date, workers must not specify compatibilityDate in the "
                  "config. Use compatibilityFlags to enable/disable specific flags if needed."));
    }
    // FUTURE_FOR_TEST admits any valid date, such as 2999-12-31.
    compileCompatibilityFlags(overrideDate, conf.getCompatibilityFlags(), flags, errorReporter,
        options.experimental, CompatibilityDateValidation::FUTURE_FOR_TEST, nullptr,
        isPythonMainModule(conf));
  } else if (conf.hasCompatibilityDate()) {
    compileCompatibilityFlags(conf.getCompatibilityDate(), conf.getCompatibilityFlags(), flags,
        errorReporter, options.experimental, CompatibilityDateValidation::CODE_VERSION, nullptr,
        isPythonMainModule(conf));
  } else {
    errorReporter.addError(kj::str("Worker must specify compatibilityDate."));
  }
  return flags.asReader();
}

// The global outbound of a dynamic worker whose source gave it none: every request fails.
class NullGlobalOutboundChannel final: public IoChannelFactory::SubrequestChannel {
 public:
  kj::Own<WorkerInterface> startRequest(IoChannelFactory::SubrequestMetadata metadata) override {
    JSG_FAIL_REQUIRE(Error,
        "This worker is not permitted to access the internet via global functions like fetch(). "
        "It must use capabilities (such as bindings in 'env') to talk to the outside world.");
  }

  // The null outbound is hard to reach: nothing normally refers to it, but a `Fetcher` for the
  // `next` outbound pulled off an incoming `Request` points at it. Transfer is refused because
  // nothing needs it; were it allowed, `startRequest()`'s error would mislead once transferred.
  void requireAllowsTransfer() override {
    JSG_FAIL_REQUIRE(DOMDataCloneError, "The null global outbound is not transferrable.");
  }
  kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> getTokenMaybeSync(
      IoChannelFactory::ChannelTokenUsage usage) override {
    JSG_FAIL_REQUIRE(DOMDataCloneError, "The null global outbound is not transferrable.");
  }
};

// Fills a dynamic worker's channel tables from its fetched source: the global outbound slots,
// then the capabilities in `env`, each rewritten into the channel number it was given.
void indexDynamicChannels(CompiledWorker::Impl& impl, DynamicWorkerSource& source) {
  kj::Rc<IoChannelFactory::SubrequestChannel> globalOutbound =
      kj::mv(source.globalOutbound).orDefault([]() -> kj::Rc<IoChannelFactory::SubrequestChannel> {
    return kj::rc<NullGlobalOutboundChannel>();
  });
  for (uint i = 1; i < IoContext::SPECIAL_SUBREQUEST_CHANNEL_COUNT; i++) {
    impl.subrequestChannels.add(globalOutbound.addRef());
  }
  impl.subrequestChannels.add(kj::mv(globalOutbound));

  source.env.rewriteCaps([&](kj::Own<Frankenvalue::CapTableEntry> entry) {
    KJ_IF_SOME(channel, kj::tryDowncast<IoChannelFactory::SubrequestChannel>(*entry)) {
      uint number = impl.subrequestChannels.size();
      impl.subrequestChannels.add(channel.addRef());
      return kj::heap<IoChannelCapTableEntry>(IoChannelCapTableEntry::SUBREQUEST, number);
    } else KJ_IF_SOME(channel, kj::tryDowncast<IoChannelFactory::ActorClassChannel>(*entry)) {
      uint number = impl.actorClassChannels.size();
      impl.actorClassChannels.add(channel.addRef());
      return kj::heap<IoChannelCapTableEntry>(IoChannelCapTableEntry::ACTOR_CLASS, number);
    } else KJ_IF_SOME(channel, kj::tryDowncast<IoChannelFactory::RpcChannel>(*entry)) {
      uint number = impl.rpcChannels.size();
      impl.rpcChannels.add(channel.addRef());
      return kj::heap<IoChannelCapTableEntry>(IoChannelCapTableEntry::RPC, number);
    } else {
      JSG_FAIL_REQUIRE(DOMDataCloneError,
          "Dynamic 'env' contains one or more objects that are not supported for use in "
          "'env', although they would be supported in 'props'.");
    }
  });
}

kj::Promise<void> preloadPython(WorkerFactory::Impl& factory, CompatibilityFlags::Reader flags) {
  if (!flags.getPythonWorkers()) co_return;
  KJ_IF_SOME(release, getPythonSnapshotRelease(flags)) {
    co_await fetchPyodideBundle(factory.options->pythonConfig, getPythonBundleName(release),
        release.getIntegrity(), factory.network, factory.timer);
  }
}

bool hasWasmModules(const WorkerSource& source) {
  KJ_IF_SOME(modules, source.variant.tryGet<WorkerSource::ModulesSource>()) {
    for (auto& module: modules.modules) {
      if (module.content.is<WorkerSource::WasmModule>()) return true;
    }
  }
  return false;
}

bool supportsStartupSnapshot(CompatibilityFlags::Reader featureFlags, const WorkerSource& source) {
  return !featureFlags.getPythonWorkers() && !featureFlags.getNewModuleRegistry() &&
      !source.variant.is<WorkerSource::ScriptSource>() && !hasWasmModules(source);
}

}  // namespace

kj::Promise<kj::Own<CompiledWorker>> factory_new_worker(const WorkerFactory& constFactory,
    const WorkerSpec& spec,
    kj::Maybe<uint32_t> configService,
    kj::Maybe<kj::Own<DynamicSource>> dynamicSource) {
  auto& factory = constFactory.getImpl();
  auto name = kj::str(spec.name);
  TRACE_EVENT("workerd", "factory_new_worker()", "name", name.cStr());
  auto impl = kj::refcounted<CompiledWorker::Impl>(constFactory, kj::mv(name));
  impl->isDynamic = dynamicSource != kj::none;
  ErrorReporter errorReporter;
  auto& options = *factory.options;
  bool experimental = options.experimental;

  // The code, its flags and its `env`, from the config or the dynamic source.
  CompatibilityFlags::Reader featureFlags;
  kj::Maybe<WorkerSource> source;
  kj::Maybe<kj::StringPtr> moduleFallback;
  capnp::List<config::Extension>::Reader extensions;
  kj::Function<void(jsg::Lock&, const Worker::Api&, v8::Local<v8::Object>)> compileBindings;
  KJ_IF_SOME(index, configService) {
    auto conf = factory.config.getServices()[index].getWorker();
    extensions = factory.config.getExtensions();
    impl->flagsArena = kj::heap<capnp::MallocMessageBuilder>();
    featureFlags = compileFlags(options, conf, *impl->flagsArena, errorReporter);
    source = WorkerdApi::extractSource(impl->name, conf, featureFlags, errorReporter);
    if (conf.hasModuleFallback()) moduleFallback = conf.getModuleFallback();
    compileBindings = [globals = kj::heapArray(kj::from<Rust>(spec.globals))](
                          jsg::Lock& lock, const Worker::Api& api, v8::Local<v8::Object> target) {
      compileGlobals(lock, api, globals, target);
    };
  } else {
    auto fetched = co_await (*KJ_ASSERT_NONNULL(dynamicSource))();
    co_await fetched.ensureAllResolved();
    auto& dynamic = impl->dynamicSource.emplace(kj::mv(fetched));
    indexDynamicChannels(*impl, dynamic);
    featureFlags = dynamic.compatibilityFlags;
    source = kj::mv(dynamic.source);
    compileBindings = [&env = dynamic.env](
                          jsg::Lock& js, const Worker::Api&, v8::Local<v8::Object> target) {
      env.populateJsObject(js, jsg::JsObject(target));
    };
  }
  auto& workerSource = KJ_ASSERT_NONNULL(source);
  // The server passes the header only under `--experimental`.
  KJ_IF_SOME(header, spec.access_blob_header) impl->accessBlobHeader = kj::str(header);

  co_await preloadPython(factory, featureFlags);

  // The file system roots use their defaults; the config does not expose mount points.
  auto workerFs = newWorkerFileSystem(kj::heap<FsMap>(), getBundleDirectory(workerSource));

  // Python workers never use the new module registry, whatever their flags say.
  bool usingNewModuleRegistry = isNewModuleRegistryEnabled(featureFlags);
  kj::Maybe<kj::Arc<jsg::modules::ModuleRegistry>> newModuleRegistry;
  if (usingNewModuleRegistry) {
    KJ_REQUIRE(experimental,
        "The new ModuleRegistry implementation is an experimental feature. "
        "You must run workerd with `--experimental` to use this feature.");
    // Module URLs follow the virtual file system: a module at "/foo/bar/baz.js" in the bundle
    // is "file:///foo/bar/baz.js".
    const jsg::Url& bundleBase = workerFs->getBundleRoot();
    using ArtifactBundler = api::pyodide::ArtifactBundler;
    KJ_IF_SOME(exception, kj::runCatchingExceptions([&]() {
      newModuleRegistry = WorkerdApi::newWorkerdModuleRegistry(
          workerSource.variant.tryGet<Worker::Script::ModulesSource>(), featureFlags,
          options.pythonConfig, bundleBase, extensions, moduleFallback.clone(),
          ArtifactBundler::makeDisabledBundler());
    })) {
      // A registry that cannot be built from the source is a config error (duplicate module
      // names, Python modules without the flag). Report it and substitute an inert script and a
      // built-ins-only registry so that this error is the one reported; the worker never runs.
      errorReporter.addError(kj::str(exception.getDescription()));
      workerSource = WorkerSource(Worker::Script::ScriptSource{""_kj, impl->name, nullptr});
      newModuleRegistry = WorkerdApi::newWorkerdModuleRegistry(kj::none, featureFlags,
          options.pythonConfig, bundleBase, capnp::List<config::Extension>::Reader{}, kj::none,
          ArtifactBundler::makeDisabledBundler());
    }
  }

  // The isolate of the Worker, or of the zygote that snapshots its startup.
  auto makeIsolate = [&](kj::StringPtr isolateName, Worker::Isolate::InspectorPolicy policy,
                         kj::Maybe<jsg::SnapshotConfig> snapshotConfig) {
    auto limitEnforcer = kj::refcounted<NullIsolateLimitEnforcer>();
    auto listeners = KJ_MAP(listener, spec.inbound_listeners) {
      return Worker::Api::InboundListener{
        .protocol = kj::str(listener.protocol),
        .address = kj::str(listener.address),
        .port = listener.port,
      };
    };
    auto api = kj::heap<WorkerdApi>(factory.v8System, featureFlags, extensions,
        limitEnforcer->getCreateParams(), jsg::newIsolateGroup(),
        kj::atomicRefcounted<JsgIsolateObserver>(), *factory.memoryCacheProvider,
        options.pythonConfig, kj::mv(listeners), kj::mv(snapshotConfig));
    Worker::LoggingOptions isolateLoggingOptions = options.loggingOptions;
    isolateLoggingOptions.consoleMode =
        workerSource.variant.is<WorkerSource::ScriptSource>() && !usingNewModuleRegistry
        ? Worker::ConsoleMode::INSPECTOR_ONLY
        : options.loggingOptions.consoleMode;
    return kj::atomicRefcounted<Worker::Isolate>(kj::mv(api),
        kj::atomicRefcounted<IsolateObserver>(), isolateName, kj::mv(limitEnforcer), policy,
        kj::mv(isolateLoggingOptions));
  };

  // Behind the STARTUP_SNAPSHOT autogate, a throwaway zygote Worker evaluates the top-level code
  // to produce a V8 startup snapshot of it.
  kj::Maybe<jsg::SnapshotConfig> snapshotConfig;
  if (util::Autogate::isEnabled(util::AutogateKey::STARTUP_SNAPSHOT) &&
      supportsStartupSnapshot(featureFlags, workerSource)) {
    // The zygote reports into a reporter of its own: its failures must never surface as the
    // Worker's.
    ErrorReporter zygoteErrors;
    auto zygoteIsolate =
        makeIsolate(kj::str(impl->name, "-snapshot"), Worker::Isolate::InspectorPolicy::DISALLOW,
            jsg::SnapshotConfig(
                jsg::MutableSnapshot{.artifact = kj::atomicRefcounted<jsg::SnapshotArtifact>()}));
    auto zygoteScript = zygoteIsolate->newScript(impl->name, workerSource,
        IsolateObserver::StartType::COLD, SpanParent(nullptr),
        newWorkerFileSystem(kj::heap<FsMap>(), getBundleDirectory(workerSource)), false,
        zygoteErrors, api::pyodide::ArtifactBundler::makeDisabledBundler());
    // As the Worker's, except that `ctx.exports` is not kept: a v8::Global outliving the
    // zygote isolate breaks snapshot creation.
    auto zygote =
        kj::atomicRefcounted<Worker>(kj::mv(zygoteScript), kj::atomicRefcounted<WorkerObserver>(),
            [&](jsg::Lock& lock, const Worker::Api& api, v8::Local<v8::Object> target,
                v8::Local<v8::Object>) { compileBindings(lock, api, target); },
            IsolateObserver::StartType::COLD, SpanParent(nullptr),
            Worker::Lock::TakeSynchronously(kj::none), zygoteErrors);
    if (zygoteErrors.errors.empty()) {
      kj::Own<jsg::SnapshotArtifact> artifact;
      zygoteIsolate->runInLockScope(
          Worker::Lock::TakeSynchronously(kj::none), [&](jsg::Lock& lock) {
        artifact = jsg::IsolateBase::from(lock.v8Isolate).extractSnapshotArtifact();
      });
      // TODO(soon): start the Worker from the snapshot artifact.
      (void)artifact;
    } else {
      auto errors = kj::strArray(zygoteErrors.errors, "\n");
      KJ_LOG(
          INFO, "startup snapshot skipped: the zygote Worker failed to start", impl->name, errors);
    }
  }

  // With the inspector enabled it is always fully trusted.
  auto& registrar = factory.inspectorRegistrar;
  auto inspectorPolicy = registrar == kj::none
      ? Worker::Isolate::InspectorPolicy::DISALLOW
      : Worker::Isolate::InspectorPolicy::ALLOW_FULLY_TRUSTED;
  auto isolate = makeIsolate(impl->name, inspectorPolicy, kj::mv(snapshotConfig));
  KJ_IF_SOME(r, registrar) {
    r->registerIsolate(impl->name, *isolate);
  }

  if (!usingNewModuleRegistry) {
    KJ_IF_SOME(fallback, moduleFallback) {
      KJ_REQUIRE(experimental,
          "The module fallback service is an experimental feature. "
          "You must run workerd with `--experimental` to use the module fallback service.");
      auto& apiIsolate = isolate->getApi();
      apiIsolate.setModuleFallbackCallback(
          [client = kj::heap<fallback::FallbackServiceClient>(kj::str(fallback)),
              featureFlags = apiIsolate.getFeatureFlags()](jsg::Lock& js, kj::StringPtr specifier,
              kj::Maybe<kj::String> referrer, jsg::CompilationObserver& observer,
              jsg::ModuleRegistry::ResolveMethod method,
              kj::Maybe<kj::StringPtr> rawSpecifier) mutable
          -> kj::Maybe<kj::OneOf<kj::String, jsg::ModuleRegistry::ModuleInfo>> {
        kj::HashMap<kj::StringPtr, kj::StringPtr> attributes;
        KJ_IF_SOME(moduleOrRedirect,
            client->tryResolve(fallback::Version::V1,
                method == jsg::ModuleRegistry::ResolveMethod::IMPORT
                    ? fallback::ImportType::IMPORT
                    : fallback::ImportType::REQUIRE,
                specifier, rawSpecifier.orDefault(nullptr), referrer.orDefault(kj::String()),
                attributes)) {
          KJ_SWITCH_ONEOF(moduleOrRedirect) {
            KJ_CASE_ONEOF(redirect, kj::String) {
              // A 301 from the fallback service: the specifier of the module to load instead.
              return kj::Maybe(kj::mv(redirect));
            }
            KJ_CASE_ONEOF(module, kj::Own<config::Worker::Module::Reader>) {
              KJ_IF_SOME(compiled,
                  WorkerdApi::tryCompileModule(js, *module, observer, featureFlags)) {
                return kj::Maybe(kj::mv(compiled));
              }
              KJ_LOG(ERROR, "Fallback service does not support this module type", module->which());
            }
          }
        }
        return kj::none;
      });
    }
  }

  kj::Maybe<kj::Own<void>> ownContent;
  KJ_IF_SOME(dynamic, impl->dynamicSource) ownContent = kj::mv(dynamic.ownContent);
  auto script = isolate->newScript(impl->name, workerSource, IsolateObserver::StartType::COLD,
      SpanParent(nullptr), workerFs.attach(kj::mv(ownContent)), false, errorReporter,
      api::pyodide::ArtifactBundler::makeDisabledBundler(), kj::mv(newModuleRegistry));

  // `ctx.exports` is filled in once the validator has found the entrypoints, which needs the
  // Worker constructed first; the handle is held until then.
  auto worker = kj::atomicRefcounted<Worker>(kj::mv(script), kj::atomicRefcounted<WorkerObserver>(),
      [&](jsg::Lock& lock, const Worker::Api& api, v8::Local<v8::Object> target,
          v8::Local<v8::Object> ctxExports) {
    impl->ctxExportsHandle = lock.v8Ref(ctxExports);
    compileBindings(lock, api, target);
  },
      IsolateObserver::StartType::COLD, SpanParent(nullptr),
      Worker::Lock::TakeSynchronously(kj::none), errorReporter);

  worker->runInLockScope(Worker::Lock::TakeSynchronously(kj::none),
      [&](Worker::Lock& lock) { lock.validateHandlers(errorReporter); });

  impl->info = WorkerInfo{
    .entrypoints = ErrorReporter::toRust(errorReporter.entrypoints),
    .actor_classes = ErrorReporter::toRust(errorReporter.actorClasses),
    .workflow_classes = ErrorReporter::toRust(errorReporter.workflowClasses),
    .errors = ErrorReporter::toRust(errorReporter.errors),
    .warnings = ErrorReporter::toRust(errorReporter.warnings),
    .persistent_self_tokens = featureFlags.getAllowIrrevocableStubStorage(),
    .env_subrequest_channels = impl->isDynamic
        ? static_cast<uint32_t>(
              impl->subrequestChannels.size() - IoContext::SPECIAL_SUBREQUEST_CHANNEL_COUNT)
        : 0,
    .env_actor_classes = static_cast<uint32_t>(impl->actorClassChannels.size()),
  };
  impl->worker = kj::mv(worker);
  co_return kj::heap<CompiledWorker>(kj::mv(impl));
}

WorkerInfo worker_info(const CompiledWorker& worker) {
  return worker.getImpl().info;
}

void worker_set_ctx_exports(const CompiledWorker& worker, ::rust::Slice<const uint64_t> globals) {
  auto& impl = worker.getImpl();
  impl.getWorker().runInLockScope(
      Worker::Lock::TakeSynchronously(kj::none), [&](Worker::Lock& lock) {
    KJ_IF_SOME(handle, impl.ctxExportsHandle) {
      JSG_WITHIN_CONTEXT_SCOPE(lock, lock.getContext(), [&](jsg::Lock& js) {
        compileGlobals(lock, impl.getWorker().getIsolate().getApi(), kj::from<Rust>(globals),
            handle.getHandle(js));
      });
    }
    // The handle is dropped now, under the lock.
    impl.ctxExportsHandle = kj::none;
  });
}

void worker_unlink(const CompiledWorker& worker) {
  auto& impl = worker.getImpl();
  impl.waitUntilTasks.clear();
  impl.subrequestChannels.clear();
  impl.actorClassChannels.clear();
  impl.rpcChannels.clear();
  KJ_IF_SOME(dynamic, impl.dynamicSource) {
    dynamic.tails = nullptr;
    dynamic.streamingTails = nullptr;
  }
}

}  // namespace workerd::server
