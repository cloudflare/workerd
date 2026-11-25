// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "repl-environment.h"

#include <workerd/api/memory-cache.h>
#include <workerd/io/bundle-fs.h>
#include <workerd/io/compatibility-date.h>
#include <workerd/io/observer.h>
#include <workerd/io/release-version.embed.h>
#include <workerd/io/worker-fs.h>
#include <workerd/jsg/script.h>
#include <workerd/server/cli/repl/bridge.rs.h>
#include <workerd/server/factory/single-tenant-policy.h>
#include <workerd/server/factory/worker-factory.h>
#include <workerd/server/workerd-api.h>

#include <kj-rs/kj-rs.h>

#include <cstdio>

namespace workerd::server {

namespace {

// The worker's main module, which the REPL never calls into.
constexpr kj::StringPtr MAIN_MODULE_NAME = "repl.js"_kj;
constexpr kj::StringPtr MAIN_MODULE_BODY = "export default {};"_kj;

constexpr kj::StringPtr INSPECT_MODULE = "node-internal:internal_inspect"_kj;

// Evaluates to a function that, given `report`, reports promises that user code rejected and
// nothing handled, such as `Promise.reject(1);`, calling `report` for each. The REPL handles the
// promises it awaits itself.
constexpr kj::StringPtr STARTUP_SCRIPT = R"JS((report) => {
  addEventListener('unhandledrejection', (event) => {
    report();
    console.error('Uncaught (in promise)', event.reason);
  });
})JS"_kj;

// Defines `_` and `_error` on the global scope, as Node's REPL does: the last input's result and
// its last exception. Assigning one keeps the assigned value from then on. Evaluates to the
// function the REPL records each outcome with, which user code can't reach.
constexpr kj::StringPtr OUTCOMES_SCRIPT = R"JS((() => {
  const slots = {
    _: { value: undefined, assigned: false },
    _error: { value: undefined, assigned: false },
  };
  for (const name of ['_', '_error']) {
    const slot = slots[name];
    Object.defineProperty(globalThis, name, {
      configurable: true,
      get() { return slot.value; },
      set(value) {
        if (!slot.assigned) console.warn(`Expression assignment to ${name} now disabled.`);
        slot.assigned = true;
        slot.value = value;
      },
    });
  }
  return (name, value) => {
    if (!slots[name].assigned) slots[name].value = value;
  };
})())JS"_kj;

const PythonConfig REPL_PYTHON_CONFIG{.createSnapshot = false, .createBaselineSnapshot = false};

class WaitUntilErrorHandler final: public kj::TaskSet::ErrorHandler {
 public:
  void taskFailed(kj::Exception&& exception) override {
    KJ_LOG(ERROR, "REPL waitUntil() task failed", exception);
  }
};

bool isIdentifierPart(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
      c == '$';
}

// `a`, `a.b`, `a.b.c`: names separated by single dots. Only such an expression is evaluated to
// complete a property name; never a call, an index or a literal.
bool isIdentifierChain(kj::StringPtr text) {
  bool atNameStart = true;
  for (char c: text) {
    if (atNameStart) {
      if (!isIdentifierPart(c) || (c >= '0' && c <= '9')) return false;
      atNameStart = false;
    } else if (c == '.') {
      atNameStart = true;
    } else if (!isIdentifierPart(c)) {
      return false;
    }
  }
  return !atNameStart;
}

// Whether `source`, a script that compiles, uses `await` as a name outside the async functions in
// it, as `await (x)` does: a script reads that as a call to a function named `await`. In a class
// static block, `await` is neither a name nor an operator except in the functions the block
// holds, so the script does not compile in one if it does. The block is strict mode code, so a
// script only sloppy mode accepts counts as one that does.
bool usesAwaitAsName(jsg::Lock& js, kj::StringPtr source, kj::StringPtr name) {
  return js.tryCatch([&]() -> bool {
    // The line break keeps a line comment that ends the script from swallowing the block's end.
    jsg::NonModuleScript::compile(js, kj::str("(class { static {", source, "\n} })"), name);
    return false;
  }, [](jsg::Value) -> bool { return true; });
}

// The compatibility flags of the REPL's JavaScript:
//
// - The user's.
// - `nodejs_compat` unless they take a position on it, so that `await import('node:assert')`
//   works.
// - `unhandled_rejection_after_microtask_checkpoint` unless they take a position on it. Without
//   it, an RPC call that fails at once hands back a promise already rejected, which the input
//   awaiting it handles only a microtask later, so the failure is reported twice: as the input's,
//   and as an unhandled rejection.
// - With `--experimental`, `service_binding_extra_handlers`, so that a loaded config's services
//   take `scheduled()` and `queue()` calls. That lets JavaScript reach the V8 deserializer through
//   `queue()`'s `serializedBody`, which is why it is experimental; the REPL's user runs any code
//   they like.
//
// Naming a flag the date already enables is harmless.
kj::Array<kj::String> replCompatibilityFlags(const ReplEnvironmentOptions& options) {
  auto userFlags = options.compatibilityFlags.asPtr();
  kj::Vector<kj::String> flags(userFlags.size() + 3);
  bool nodejsCompatChosen = false;
  bool rejectionTimingChosen = false;
  bool extraHandlersChosen = false;
  for (auto& flag: userFlags) {
    nodejsCompatChosen = nodejsCompatChosen || flag == "nodejs_compat"_kj ||
        flag == "no_nodejs_compat"_kj || flag == "nodejs_compat_v2"_kj ||
        flag == "no_nodejs_compat_v2"_kj;
    rejectionTimingChosen = rejectionTimingChosen ||
        flag == "unhandled_rejection_after_microtask_checkpoint"_kj ||
        flag == "no_unhandled_rejection_after_microtask_checkpoint"_kj;
    // Naming a flag twice is an error.
    extraHandlersChosen = extraHandlersChosen || flag == "service_binding_extra_handlers"_kj;
    flags.add(kj::str(flag));
  }
  if (!nodejsCompatChosen) {
    flags.add(kj::str("nodejs_compat"));
  }
  if (!rejectionTimingChosen) {
    flags.add(kj::str("unhandled_rejection_after_microtask_checkpoint"));
  }
  if (options.experimental && !extraHandlersChosen) {
    flags.add(kj::str("service_binding_extra_handlers"));
  }
  return flags.releaseAsArray();
}

::rust::Str toRust(kj::StringPtr str) {
  return ::rust::Str(str.begin(), str.size());
}

kj::Maybe<::rust::Str> toRust(kj::Maybe<kj::StringPtr> str) {
  return str.map([](kj::StringPtr s) { return toRust(s); });
}

}  // namespace

namespace repl {

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

}  // namespace repl

// A config this environment loaded, as the JavaScript API holds it: the Rust server
// (cli/repl/server.rs).
class ReplEnvironment::LoadedServer final: public ReplServerHost, public kj::Refcounted {
 public:
  explicit LoadedServer(::rust::Box<repl::ReplConfigServer> serverParam)
      : server(kj::mv(serverParam)),
        path(kj::str(kj::from<kj_rs::Rust>(server->path()))) {}

  kj::Own<ReplServerHost> addRef() override {
    return kj::addRef(*this);
  }
  kj::StringPtr getPath() const override {
    return path;
  }
  bool isClosed() const override {
    return server->is_closed();
  }
  // Whether the close has finished, or failed; isClosed() is true from when it begins.
  bool isCloseDone() const {
    return server->is_close_done();
  }
  bool isWatching() const override {
    return server->is_watching();
  }
  kj::Array<kj::String> getWarnings() const override {
    return KJ_MAP(warning, server->warnings()) { return kj::str(kj::from<kj_rs::Rust>(warning)); };
  }
  kj::Rc<IoChannelFactory::SubrequestChannel> service(kj::StringPtr name,
      kj::Maybe<kj::StringPtr> entrypoint,
      kj::Maybe<Frankenvalue> props) override {
    auto ownProps = props.map([](Frankenvalue& p) { return kj::heap<Frankenvalue>(kj::mv(p)); });
    return subrequest_channel_into_kj(
        server->service(toRust(name), toRust(entrypoint), kj::mv(ownProps)));
  }
  kj::Rc<IoChannelFactory::SubrequestChannel> socket(kj::StringPtr name) override {
    return subrequest_channel_into_kj(server->socket(toRust(name)));
  }
  kj::Rc<IoChannelFactory::SubrequestChannel> durableObject(
      const ReplActorAddress& address) override {
    return subrequest_channel_into_kj(server->durable_object(toRust(address.service),
        toRust(address.className), toRust(address.name), toRust(address.id)));
  }
  kj::Promise<void> reload() override {
    return repl::repl_reload_wait(server->queue_reload());
  }
  kj::Promise<void> close() override {
    server->begin_close();
    // The handle the wait holds keeps the server until the close is done: the REPL lets go of a
    // server once closing begins, and the JavaScript handles may be collected.
    return repl::repl_server_wait_closed(server->clone_handle());
  }

 private:
  ::rust::Box<repl::ReplConfigServer> server;
  kj::String path;
};

// The channels of the REPL's own context. Its JavaScript has no bindings and the REPL no network
// of its own, so there is nothing to reach but the clock; the loaded configs are reached through
// the channels their servers hand out.
class ReplEnvironment::ChannelFactory final: public IoChannelFactory {
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

void ReplEnvironment::ErrorReporter::check(kj::StringPtr what) {
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

ReplEnvironment::ReplEnvironment(const ReplServerResources& resources,
    repl::ServerRegistry& registry,
    CandidateSource& candidateSource,
    ReplEnvironmentOptions optionsParam)
    : resources(resources),
      registry(registry),
      candidateSource(candidateSource),
      options(kj::mv(optionsParam)),
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
      memoryCacheProvider(kj::heap<api::MemoryCacheProvider>()),
      waitUntilErrorHandler(kj::heap<WaitUntilErrorHandler>()),
      waitUntilTasks(*waitUntilErrorHandler) {
  ErrorReporter errorReporter;
  auto flags = featureFlagsMessage.initRoot<CompatibilityFlags>();
  auto today = currentDateStr();
  auto date = options.compatibilityDate.map([](const kj::String& date) {
    return date.asPtr();
  }).orDefault(today);
  compileCompatibilityFlags(date, replCompatibilityFlags(options), flags, errorReporter,
      options.experimental, CompatibilityDateValidation::FUTURE_FOR_TEST, nullptr);
  // Rendering results needs the new module registry's internal modules.
  flags.setNewModuleRegistry(true);
  featureFlags = flags.asReader();
  errorReporter.check("compatibility flags");

  compileWorker();
  headerTable = headerTableBuilder.build();
  factorySource = kj::rc<repl::ReplFactorySource>(resources, *headerTable, options.experimental);
  startSession();
}

ReplEnvironment::~ReplEnvironment() noexcept(false) {}

void ReplEnvironment::compileWorker() {
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

  worker = kj::atomicRefcounted<Worker>(kj::atomicAddRef(*script),
      kj::atomicRefcounted<WorkerObserver>(),
      [this](jsg::Lock& js, const Worker::Api& api, v8::Local<v8::Object>, v8::Local<v8::Object>) {
    // `target` is the env of a modules worker; `workerd` goes on the global scope instead.
    auto control = js.alloc<ReplControl>(*this, RELEASE_VERSION);
    js.global().set(js, "workerd"_kj, WorkerdApi::from(api).wrapReplControl(js, kj::mv(control)));
    auto startup =
        jsg::NonModuleScript::compile(js, STARTUP_SCRIPT, "repl-startup.js"_kj).runAndReturn(js);
    // This environment owns the isolate, so `report` is never called once it is gone.
    auto report = js.wrapSimpleFunction(
        js.v8Context(), [this](jsg::Lock&, const v8::FunctionCallbackInfo<v8::Value>&) {
      failedInBackground = true;
    });
    KJ_ASSERT_NONNULL(startup.tryCast<jsg::JsFunction>())
        .call(js, js.undefined(), jsg::JsValue(report.As<v8::Value>()));
    auto record =
        jsg::NonModuleScript::compile(js, OUTCOMES_SCRIPT, "repl-outcomes.js"_kj).runAndReturn(js);
    recordOutcome = jsg::JsRef(js, KJ_ASSERT_NONNULL(record.tryCast<jsg::JsFunction>()));
  }, IsolateObserver::StartType::COLD, SpanParent(nullptr),
      Worker::Lock::TakeSynchronously(kj::none), errorReporter);
  errorReporter.check("worker");
}

void ReplEnvironment::startSession() {
  auto context = kj::refcounted<IoContext>(
      threadContext, kj::atomicAddRef(*worker), kj::none, kj::refcounted<NullLimitEnforcer>());
  auto request = kj::heap<IoContext::IncomingRequest>(kj::mv(context),
      kj::rc<ChannelFactory>(*timerChannel), kj::refcounted<RequestObserver>(), kj::none, kj::none);
  request->delivered("repl"_kj);
  waitingForInput = request->getContext().registerPendingEvent();
  incomingRequest = kj::mv(request);
}

kj::Promise<ReplEnvironment::Evaluation> ReplEnvironment::evaluate(
    kj::Array<Candidate> candidates) {
  KJ_REQUIRE(candidates.size() > 0, "nothing to evaluate");
  auto& context = KJ_REQUIRE_NONNULL(incomingRequest, "the REPL session has ended")->getContext();

  kj::Maybe<Evaluation> evaluation;
  kj::Maybe<Awaiting> awaiting;
  co_await context.run([&](Worker::Lock& lock, IoContext& context) {
    auto& js = jsg::Lock::from(lock.getIsolate());
    KJ_SWITCH_ONEOF(run(js, context, candidates)) {
      KJ_CASE_ONEOF(e, Evaluation) {
        evaluation = kj::mv(e);
      }
      KJ_CASE_ONEOF(a, Awaiting) {
        awaiting = kj::mv(a);
      }
    }
  });

  KJ_IF_SOME(a, awaiting) {
    // The event loop runs meanwhile: timers fire, servers serve, output flows.
    co_await a.settled;
    co_await context.run([&](Worker::Lock& lock) {
      auto& js = jsg::Lock::from(lock.getIsolate());
      // Released at the end of the scope, under the lock.
      auto result = kj::mv(a.result);
      auto promise = result.getHandle(js);
      // The input awaited the promise, so a rejection is thrown, as at the top level of a module.
      if (promise.state() == jsg::PromiseState::REJECTED) {
        record(js, "_error"_kj, promise.result());
        evaluation = Evaluation{
          .text = kj::str("Uncaught ", describeException(js, promise.result())),
          .threw = true,
        };
      } else {
        record(js, "_"_kj, promise.result());
        evaluation = Evaluation{.text = render(js, promise.result()), .threw = false};
      }
    });
  }

  co_return KJ_ASSERT_NONNULL(kj::mv(evaluation));
}

kj::OneOf<ReplEnvironment::Evaluation, ReplEnvironment::Awaiting> ReplEnvironment::run(
    jsg::Lock& js, IoContext& context, kj::ArrayPtr<const Candidate> candidates) {
  using Outcome = kj::OneOf<Evaluation, Awaiting>;
  return js.tryCatch([&]() -> Outcome {
    auto compiled = compile(js, candidates);
    auto result = compiled.script.runAndReturn(js);
    if (!compiled.awaited || !result.isPromise()) {
      // A promise the user did not await renders as a promise: awaiting it could hang the REPL on
      // one that never settles.
      record(js, "_"_kj, result);
      return Evaluation{.text = render(js, result), .threw = false};
    }
    auto promise = KJ_ASSERT_NONNULL(result.tryCast<jsg::JsPromise>());
    // Settling is all that matters here; evaluate() reads the outcome off the promise.
    auto settled =
        js.toPromise(result).then(js, [](jsg::Lock&, jsg::Value) {}, [](jsg::Lock&, jsg::Value) {});
    return Awaiting{
      .result = jsg::JsRef(js, promise),
      .settled = context.awaitJs(js, kj::mv(settled)),
    };
  }, [&](jsg::Value exception) -> Outcome {
    auto thrown = jsg::JsValue(exception.getHandle(js));
    record(js, "_error"_kj, thrown);
    return Evaluation{.text = kj::str("Uncaught ", describeException(js, thrown)), .threw = true};
  });
}

void ReplEnvironment::record(jsg::Lock& js, kj::StringPtr name, const jsg::JsValue& value) {
  KJ_IF_SOME(recorder, recordOutcome) {
    // The recorder is the REPL's own, and does not throw.
    recorder.getHandle(js).call(js, js.undefined(), js.str(name), value);
  }
}

ReplEnvironment::Compiled ReplEnvironment::compile(
    jsg::Lock& js, kj::ArrayPtr<const Candidate> candidates) {
  if (candidates.size() > 1) {
    kj::Maybe<Compiled> passedOver;
    for (auto& candidate: candidates) {
      auto compiled = js.tryCatch([&]() -> kj::Maybe<Compiled> {
        return Compiled{.script = jsg::NonModuleScript::compile(js, candidate.source, inputName),
          .awaited = candidate.awaited};
      }, [](jsg::Value) -> kj::Maybe<Compiled> { return kj::none; });
      KJ_IF_SOME(c, compiled) {
        if (c.awaited || !usesAwaitAsName(js, candidate.source, inputName)) {
          return kj::mv(c);
        }
        if (passedOver == kj::none) {
          passedOver = kj::mv(c);
        }
      }
    }
    // Nothing later compiles, as for an input that declares a name `await` and uses it.
    KJ_IF_SOME(c, passedOver) {
      return kj::mv(c);
    }
  }
  // None compiles, so the syntax error of the first that isn't parenthesized is thrown.
  auto& reported = [&]() -> const Candidate& {
    for (auto& candidate: candidates) {
      if (!candidate.parenthesized) return candidate;
    }
    return candidates[0];
  }();
  return {.script = jsg::NonModuleScript::compile(js, reported.source, inputName),
    .awaited = reported.awaited};
}

kj::Promise<kj::Array<kj::String>> ReplEnvironment::propertyNames(kj::String base) {
  auto& context = KJ_REQUIRE_NONNULL(incomingRequest, "the REPL session has ended")->getContext();
  if (base.size() > 0 && !isIdentifierChain(base)) {
    co_return nullptr;
  }

  kj::Vector<kj::String> names;
  co_await context.run([&](Worker::Lock& lock) {
    auto& js = jsg::Lock::from(lock.getIsolate());
    js.tryCatch([&] {
      v8::Local<v8::Object> target;
      if (base.size() == 0) {
        target = js.v8Context()->Global();
      } else {
        auto script = jsg::NonModuleScript::compile(js, kj::str("(", base, ")"), inputName);
        auto value = script.runAndReturn(js);
        if (value.isNullOrUndefined()) {
          return;
        }
        target = jsg::check(v8::Local<v8::Value>(value)->ToObject(js.v8Context()));
      }
      auto keys = jsg::check(
          target->GetPropertyNames(js.v8Context(), v8::KeyCollectionMode::kIncludePrototypes,
              static_cast<v8::PropertyFilter>(v8::ALL_PROPERTIES | v8::SKIP_SYMBOLS),
              v8::IndexFilter::kSkipIndices, v8::KeyConversionMode::kConvertToString));
      for (auto i: kj::zeroTo(keys->Length())) {
        names.add(js.toString(jsg::check(keys->Get(js.v8Context(), i))));
      }
    }, [&](jsg::Value) {
      // `base` does not evaluate (an undefined name, a throwing getter): nothing to complete.
      names.clear();
    });
  });
  co_return names.releaseAsArray();
}

// An environment another, `parent`, started with compatibility settings of its own. What fails
// in its background counts as failing in its parent's. The parent's drain() waits for its close.
class ReplEnvironment::Nested final: public ReplEnvironmentHost, public kj::Refcounted {
 public:
  Nested(ReplEnvironment& parent,
      kj::Own<ReplEnvironment> environment,
      kj::String compatibilityDate,
      kj::Array<kj::String> compatibilityFlags)
      : parent(parent),
        environment(kj::mv(environment)),
        compatibilityDate(kj::mv(compatibilityDate)),
        compatibilityFlags(kj::mv(compatibilityFlags)) {}

  kj::Own<ReplEnvironmentHost> addRef() override {
    return kj::addRef(*this);
  }
  kj::StringPtr getCompatibilityDate() const override {
    return compatibilityDate;
  }
  kj::ArrayPtr<const kj::String> getCompatibilityFlags() const override {
    return compatibilityFlags;
  }
  bool isClosed() const override {
    return closing != kj::none;
  }
  // Whether the close has finished, or failed; isClosed() is true from when it begins.
  bool isCloseDone() const {
    return closeDone;
  }

  kj::Promise<kj::String> evaluate(kj::String input) override {
    auto& open = requireOpen();
    // Closing cancels it, before the environment is taken apart.
    return canceler.wrap(evaluateLater(open, open.candidateSource.candidates(input)));
  }

  kj::Promise<void> close() override {
    // What waits for the close keeps the environment until it is done, as for a server.
    KJ_IF_SOME(c, closing) {
      return c.addBranch().attach(kj::addRef(*this));
    }
    return closing.emplace(closeEnvironment().fork()).addBranch().attach(kj::addRef(*this));
  }

 private:
  ReplEnvironment& parent;
  // None once closing has begun.
  kj::Maybe<kj::Own<ReplEnvironment>> environment;
  kj::String compatibilityDate;
  kj::Array<kj::String> compatibilityFlags;
  // Set by the close as it ends, even if it is destroyed unfinished: declared before `closing`,
  // which holds it.
  bool closeDone = false;
  kj::Maybe<kj::ForkedPromise<void>> closing;
  // Declared after `environment`, so that what it wraps is canceled before that is destroyed.
  kj::Canceler canceler;

  ReplEnvironment& requireOpen() {
    KJ_IF_SOME(e, environment) {
      if (!isClosed()) return *e;
    }
    JSG_FAIL_REQUIRE(Error, "The environment was closed.");
  }

  static kj::Promise<kj::String> evaluateLater(
      ReplEnvironment& open, kj::Array<Candidate> candidates) {
    // Asked for under the REPL's isolate lock; the input runs under the environment's own.
    co_await kj::yield();
    // Only the rendering crosses between environments.
    co_return (co_await open.evaluate(kj::mv(candidates))).text;
  }

  kj::Promise<void> closeEnvironment() {
    KJ_DEFER(closeDone = true);
    // Asked for under the REPL's isolate lock; the environment's isolate is torn down without it.
    co_await kj::yield();
    canceler.cancel(JSG_KJ_EXCEPTION(FAILED, Error, "The environment was closed."));
    auto closed = kj::mv(KJ_ASSERT_NONNULL(environment));
    environment = kj::none;
    if (co_await closed->drain()) {
      parent.failedInBackground = true;
    }
  }
};

namespace {
// Awaits `closing`, keeping its failure in `failure` unless that holds one already.
kj::Promise<void> closeKeepingFailure(
    kj::Promise<void> closing, kj::Maybe<kj::Exception>& failure) {
  KJ_TRY {
    co_await closing;
  }
  KJ_CATCH(exception) {
    if (failure == kj::none) {
      failure = kj::mv(exception);
    }
  }
}
}  // namespace

kj::Own<void> ReplEnvironment::beginStart() {
  JSG_REQUIRE(!drainStarted, Error, "The environment is closing.");
  ++startsUnderWay;
  return kj::heap(kj::defer([this]() {
    if (--startsUnderWay == 0 && drainStarted) {
      startsSettled.signal();
    }
  }));
}

kj::Promise<bool> ReplEnvironment::drain() {
  // A start under way, such as a loadConfig() that wasn't awaited, adds what it started when it
  // finishes. Canceling one could drop a server that is running but not closed.
  drainStarted = true;
  if (startsUnderWay > 0) {
    co_await startsSettled.whenSignaled();
  }

  // What fails to close does not keep the rest open or the request undrained.
  kj::Maybe<kj::Exception> failure;
  auto closingServers = kj::mv(servers);
  for (auto& server: closingServers) {
    co_await closeKeepingFailure(server->close(), failure);
  }
  auto closingEnvironments = kj::mv(environments);
  for (auto& environment: closingEnvironments) {
    co_await closeKeepingFailure(environment->close(), failure);
  }

  KJ_IF_SOME(request, incomingRequest) {
    // Kept past the request, for what its waitUntil() tasks did.
    auto context = kj::addRef(request->getContext());
    auto draining = kj::mv(request);
    incomingRequest = kj::none;
    draining->drain(waitUntilTasks, kj::mv(draining));
    co_await waitUntilTasks.onEmpty();
    // A timer callback that threw counts, as a waitUntil() task that rejected does.
    if (context->waitUntilStatus() != EventOutcome::OK) {
      failedInBackground = true;
    }
  }

  KJ_IF_SOME(exception, failure) {
    kj::throwFatalException(kj::mv(exception));
  }
  co_return failedInBackground;
}

kj::Promise<kj::Own<ReplServerHost>> ReplEnvironment::loadConfig(ReplConfigSource source) {
  auto started = beginStart();
  // JavaScript asked for the load under the REPL's isolate lock; the server's workers lock
  // isolates of their own, so the load runs on the event loop.
  co_await kj::yield();
  auto constName = kj::mv(source.constName).orDefault([]() { return kj::str(); });
  // From JavaScript strings, which convert to valid UTF-8.
  auto server = kj::refcounted<LoadedServer>(co_await repl::repl_server_load(
      registry.clone_handle(), KJ_ASSERT_NONNULL(factorySource).addRef(),
      source.path.as<kj_rs::RustUncheckedUtf8>(), constName.as<kj_rs::RustUncheckedUtf8>(),
      source.watch ? repl::Watch::Watched : repl::Watch::Unwatched));
  // Drop the servers that finished closing since the last load. One still closing stays, so that
  // drain() waits for it.
  kj::Vector<kj::Own<LoadedServer>> open(servers.size() + 1);
  for (auto& s: servers) {
    if (!s->isCloseDone()) open.add(kj::mv(s));
  }
  open.add(kj::addRef(*server));
  servers = kj::mv(open);
  co_return server;
}

kj::Own<ReplEnvironment> ReplEnvironment::startEnvironment(
    kj::StringPtr compatibilityDate, kj::ArrayPtr<const kj::String> compatibilityFlags) {
  KJ_TRY {
    return kj::heap<ReplEnvironment>(resources, registry, candidateSource,
        ReplEnvironmentOptions{
          .compatibilityDate = kj::str(compatibilityDate),
          .compatibilityFlags = KJ_MAP(flag, compatibilityFlags) { return kj::str(flag); },
          .experimental = options.experimental,
          // The rendering is a string in the REPL that started it, where escapes would show.
          .colors = false,
        });
  }
  KJ_CATCH(exception) {
    // The settings' problems, which are the user's.
    JSG_FAIL_REQUIRE(Error, exception.getDescription());
  }
}

kj::Promise<kj::Own<ReplEnvironmentHost>> ReplEnvironment::newEnvironment(
    ReplEnvironmentSettings settings) {
  auto started = beginStart();
  // JavaScript asked for it under the REPL's isolate lock; starting an isolate locks it.
  co_await kj::yield();
  auto date = kj::mv(settings.compatibilityDate).orDefault([]() { return currentDateStr(); });
  auto environment = startEnvironment(date, settings.compatibilityFlags);
  auto nested = kj::refcounted<Nested>(
      *this, kj::mv(environment), kj::mv(date), kj::mv(settings.compatibilityFlags));
  // Drop the environments that finished closing since the last one started, as loadConfig() drops
  // servers.
  kj::Vector<kj::Own<Nested>> open(environments.size() + 1);
  for (auto& e: environments) {
    if (!e->isCloseDone()) open.add(kj::mv(e));
  }
  open.add(kj::addRef(*nested));
  environments = kj::mv(open);
  co_return nested;
}

kj::String ReplEnvironment::render(jsg::Lock& js, const jsg::JsValue& value) {
  return js.tryCatch([&]() -> kj::String {
    KJ_IF_SOME(module, js.resolveInternalModule(INSPECT_MODULE)) {
      auto inspect = module.get(js, "inspect"_kj);
      KJ_IF_SOME(function, inspect.tryCast<jsg::JsFunction>()) {
        auto inspectOptions = js.obj();
        inspectOptions.set(js, "colors"_kj, js.boolean(options.colors));
        return function.call(js, js.undefined(), value, inspectOptions).toString(js);
      }
    }
    return value.toString(js);
  }, [&](jsg::Value) -> kj::String {
    // Inspecting the value threw (a throwing getter, a revoked proxy).
    return js.tryCatch([&]() -> kj::String { return value.toString(js); },
        [&](jsg::Value) -> kj::String { return kj::str("[value cannot be displayed]"); });
  });
}

kj::String ReplEnvironment::describeException(jsg::Lock& js, const jsg::JsValue& exception) {
  return js.tryCatch([&]() -> kj::String {
    KJ_IF_SOME(object, exception.tryCast<jsg::JsObject>()) {
      // An Error's stack starts with its name and message.
      auto stack = object.get(js, "stack"_kj);
      if (stack.isString()) {
        return stack.toString(js);
      }
    }
    return exception.toString(js);
  }, [&](jsg::Value) -> kj::String { return kj::str("[exception cannot be displayed]"); });
}

}  // namespace workerd::server
