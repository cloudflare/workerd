// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "repl-worker.h"

#include <workerd/api/memory-cache.h>
#include <workerd/io/bundle-fs.h>
#include <workerd/io/compatibility-date.h>
#include <workerd/io/observer.h>
#include <workerd/io/worker-fs.h>
#include <workerd/rust/jsg-cpp/jsg-cpp.h>
#include <workerd/server/cli/repl/bridge.rs.h>
#include <workerd/server/factory/single-tenant-policy.h>
#include <workerd/server/factory/worker-factory.h>
#include <workerd/server/workerd-api.h>

#include <kj-rs/kj-rs.h>

#include <cstdio>

namespace workerd::server::repl {

namespace {

// The worker's main module, which the REPL never calls into.
constexpr kj::StringPtr MAIN_MODULE_NAME = "repl.js"_kj;
constexpr kj::StringPtr MAIN_MODULE_BODY = "export default {};"_kj;

constexpr kj::StringPtr INSPECT_MODULE = "node-internal:internal_inspect"_kj;

const PythonConfig REPL_PYTHON_CONFIG{.createSnapshot = false, .createBaselineSnapshot = false};

}  // namespace

kj::Own<WorkerFactory> repl_new_factory(
    const ReplFactorySource& source, ::rust::Slice<const uint64_t> config) {
  auto& resources = source.resources;
  auto options = kj::heap<WorkerFactory::Options>();
  options->loggingOptions = Worker::LoggingOptions(Worker::ConsoleMode::STDOUT);
  options->experimental = source.experimental;
  options->hostHeaderTable = source.headerTable;
  return kj::heap<WorkerFactory>(resources.v8System, resources.timer, resources.monotonicClock,
      resources.network, resources.entropySource, resources.fs, kj::mv(options),
      kj::from<kj_rs::Rust>(config).as<kj_rs::RustCopy>());
}

kj::Own<ReplWorker> repl_worker_new(
    const ReplResources& resources, const ReplWorkerOptions& options) {
  return kj::heap<ReplWorker>(kj::heap<ReplWorker::Impl>(resources.resources, options));
}

kj::Own<io_context::IncomingRequest> repl_worker_new_request(ReplWorker& worker) {
  return worker.getImpl().newRequest();
}

::rust::Str repl_worker_input_name(const ReplWorker& worker) {
  auto name = worker.getImpl().getInputName();
  return ::rust::Str(name.begin(), name.size());
}

kj::Rc<ReplFactorySource> repl_worker_factory_source(ReplWorker& worker) {
  return worker.getImpl().getFactorySource();
}

bool repl_inspect(
    v8::Isolate* isolate, ::workerd::rust::jsg::Local value, bool colors, ::rust::String& result) {
  return ::workerd::rust::jsg_cpp::callCpp(isolate, [&](jsg::Lock& js) {
    auto target = jsg::JsValue(::workerd::rust::jsg::local_from_ffi<v8::Value>(kj::mv(value)));
    auto text = [&]() -> kj::String {
      KJ_IF_SOME(module, js.resolveInternalModule(INSPECT_MODULE)) {
        auto inspect = module.get(js, "inspect"_kj);
        KJ_IF_SOME(function, inspect.tryCast<jsg::JsFunction>()) {
          auto inspectOptions = js.obj();
          inspectOptions.set(js, "colors"_kj, js.boolean(colors));
          return function.call(js, js.undefined(), target, inspectOptions).toString(js);
        }
      }
      return target.toString(js);
    }();
    // From a JavaScript string, which converts to valid UTF-8.
    result = text.as<kj_rs::RustUncheckedUtf8>();
  });
}

kj::Rc<IoChannelFactory::SubrequestChannel> repl_channel_into_kj(
    ::rust::Box<SubrequestChannel> channel) {
  return subrequest_channel_into_kj(kj::mv(channel));
}

// The channels of the REPL's own context. Its JavaScript has no bindings and the REPL no network
// of its own, so there is nothing to reach but the clock; the loaded configs are reached through
// the channels their servers hand out.
class ReplWorker::Impl::ChannelFactory final: public IoChannelFactory {
 public:
  explicit ChannelFactory(TimerChannel& timer): timer(timer) {}

  kj::Own<WorkerInterface> startSubrequest(uint channel, SubrequestMetadata metadata) override {
    JSG_FAIL_REQUIRE(Error,
        "The REPL has no network of its own. Load a config with workerd.loadConfig() and fetch() "
        "through one of its services.");
  }
  kj::Rc<SubrequestChannel> getSubrequestChannelResolved(uint channel,
      kj::Maybe<Frankenvalue> props,
      kj::Maybe<VersionRequest> versionRequest,
      Persistent persistent) override {
    JSG_FAIL_REQUIRE(Error, "The REPL's JavaScript has no service bindings.");
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
    JSG_FAIL_REQUIRE(Error, "The REPL's JavaScript has no Durable Object bindings.");
  }
  kj::Rc<ActorChannel> getColoLocalActor(
      uint channel, kj::StringPtr id, SpanParent parentSpan) override {
    JSG_FAIL_REQUIRE(Error, "The REPL's JavaScript has no Durable Object bindings.");
  }
  kj::Rc<ActorClassChannel> getActorClassResolved(
      uint channel, kj::Maybe<Frankenvalue> props, Persistent persistent) override {
    JSG_FAIL_REQUIRE(Error, "The REPL's JavaScript has no Durable Object classes.");
  }
  capnp::Capability::Client getCapability(uint channel) override {
    JSG_FAIL_REQUIRE(Error, "The REPL's JavaScript has no capability bindings.");
  }
  kj::Own<CacheClient> getCache() override {
    JSG_FAIL_REQUIRE(Error, "The REPL has no cache.");
  }
  kj::Promise<void> writeLogfwdr(
      uint channel, kj::FunctionParam<void(capnp::AnyPointer::Builder)> buildMessage) override {
    JSG_FAIL_REQUIRE(Error, "The REPL's JavaScript has no log bindings.");
  }
  TimerChannel& getTimer() override {
    return timer;
  }
  void abortIsolate(kj::StringPtr reason) override {
    KJ_FAIL_ASSERT("the REPL's isolate was aborted", reason);
  }

 private:
  TimerChannel& timer;
};

void ReplWorker::Impl::ErrorReporter::check(kj::StringPtr what) {
  for (auto& warning: warnings) {
    fprintf(stderr, "warning: %s\n", warning.cStr());
  }
  warnings.clear();
  if (!errors.empty()) {
    auto message = kj::strArray(errors, "\n  ");
    errors.clear();
    kj::throwFatalException(KJ_EXCEPTION(FAILED, kj::str("Invalid ", what, ":\n  ", message)));
  }
}

ReplWorker::ReplWorker(kj::Own<Impl> impl): impl(kj::mv(impl)) {}
ReplWorker::~ReplWorker() noexcept(false) {}

ReplWorker::Impl::Impl(const ReplServerResources& resources, const ReplWorkerOptions& options)
    : resources(resources),
      threadContextHeaderBundle(headerTableBuilder),
      httpOverCapnpFactory(byteStreamFactory,
          capnp::HttpOverCapnpFactory::HeaderIdBundle(headerTableBuilder),
          capnp::HttpOverCapnpFactory::LEVEL_2),
      threadContext(resources.timer,
          resources.entropySource,
          threadContextHeaderBundle,
          httpOverCapnpFactory,
          byteStreamFactory),
      timerChannel(kj::heap<WorkerTimerChannel>(resources.timer, resources.monotonicClock)),
      memoryCacheProvider(kj::heap<api::MemoryCacheProvider>()) {
  compileFlags(options);
  compileWorker();
  headerTable = headerTableBuilder.build();
  factorySource = kj::rc<ReplFactorySource>(resources, *headerTable, options.experimental);
}

ReplWorker::Impl::~Impl() noexcept(false) {}

kj::Rc<ReplFactorySource> ReplWorker::Impl::getFactorySource() {
  return KJ_ASSERT_NONNULL(factorySource).addRef();
}

void ReplWorker::Impl::compileFlags(const ReplWorkerOptions& options) {
  ErrorReporter errorReporter;
  auto flags = featureFlagsMessage.initRoot<CompatibilityFlags>();
  auto date = kj::str(kj::from<kj_rs::Rust>(options.compat_date));
  auto names = KJ_MAP(flag, options.compat_flags) { return kj::str(kj::from<kj_rs::Rust>(flag)); };
  compileCompatibilityFlags(date, names, flags, errorReporter, options.experimental,
      CompatibilityDateValidation::FUTURE_FOR_TEST, nullptr);
  // Rendering results needs the new module registry's internal modules.
  flags.setNewModuleRegistry(true);
  featureFlags = flags.asReader();
  errorReporter.check("compatibility flags");
}

void ReplWorker::Impl::compileWorker() {
  ErrorReporter errorReporter;
  auto modules = kj::heapArrayBuilder<WorkerSource::Module>(1);
  modules.add(WorkerSource::Module{
    .name = MAIN_MODULE_NAME,
    .content = WorkerSource::EsModule{.body = MAIN_MODULE_BODY},
  });
  WorkerSource source(WorkerSource::ModulesSource{
    .mainModule = MAIN_MODULE_NAME,
    .modules = modules.finish(),
    .capnpSchemas = {},
    .isPython = false,
  });
  auto vfs = newWorkerFileSystem(kj::heap<FsMap>(), getBundleDirectory(source));
  inputName =
      kj::str(KJ_ASSERT_NONNULL(vfs->getBundleRoot().tryResolve(MAIN_MODULE_NAME)).getHref());

  auto moduleRegistry = WorkerdApi::newWorkerdModuleRegistry(
      source.variant.tryGet<WorkerSource::ModulesSource>(), featureFlags, REPL_PYTHON_CONFIG,
      vfs->getBundleRoot(), capnp::List<config::Extension>::Reader{});

  auto limitEnforcer = kj::refcounted<NullIsolateLimitEnforcer>();
  auto api = kj::heap<WorkerdApi>(resources.v8System, featureFlags,
      capnp::List<config::Extension>::Reader{}, limitEnforcer->getCreateParams(),
      jsg::newIsolateGroup(), kj::atomicRefcounted<JsgIsolateObserver>(), *memoryCacheProvider,
      REPL_PYTHON_CONFIG);
  isolate =
      kj::atomicRefcounted<Worker::Isolate>(kj::mv(api), kj::atomicRefcounted<IsolateObserver>(),
          "repl"_kj, kj::mv(limitEnforcer), Worker::Isolate::InspectorPolicy::DISALLOW,
          Worker::LoggingOptions(Worker::ConsoleMode::STDOUT));
  script = isolate->newScript("repl"_kj, source, IsolateObserver::StartType::COLD,
      SpanParent(nullptr), kj::mv(vfs), errorReporter,
      api::pyodide::ArtifactBundler::makeDisabledBundler(), kj::mv(moduleRegistry));
  errorReporter.check("script");

  // Rust gives the global scope `workerd` in the session's request (cli/repl/environment.rs).
  worker = kj::atomicRefcounted<Worker>(kj::atomicAddRef(*script),
      kj::atomicRefcounted<WorkerObserver>(),
      [](jsg::Lock&, const Worker::Api&, v8::Local<v8::Object>, v8::Local<v8::Object>) {},
      IsolateObserver::StartType::COLD, SpanParent(nullptr),
      Worker::Lock::TakeSynchronously(kj::none), errorReporter);
  errorReporter.check("worker");
}

kj::Own<IoContext::IncomingRequest> ReplWorker::Impl::newRequest() {
  auto context = kj::refcounted<IoContext>(
      threadContext, kj::atomicAddRef(*worker), kj::none, kj::refcounted<NullLimitEnforcer>());
  return kj::heap<IoContext::IncomingRequest>(kj::mv(context),
      kj::rc<ChannelFactory>(*timerChannel), kj::refcounted<RequestObserver>(), kj::none, kj::none);
}

}  // namespace workerd::server::repl
