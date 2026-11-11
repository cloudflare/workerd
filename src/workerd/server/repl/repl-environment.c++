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
#include <workerd/server/factory/single-tenant-policy.h>
#include <workerd/server/workerd-api.h>

#include <cstdio>

namespace workerd::server {

namespace {

// The worker's main module, which the REPL never calls into.
constexpr kj::StringPtr MAIN_MODULE_NAME = "repl.js"_kj;
constexpr kj::StringPtr MAIN_MODULE_BODY = "export default {};"_kj;

constexpr kj::StringPtr INSPECT_MODULE = "node-internal:internal_inspect"_kj;

// Reports promises that user code rejected and nothing handled, such as `Promise.reject(1);`. The
// REPL handles the promises it awaits itself.
constexpr kj::StringPtr STARTUP_SCRIPT = "addEventListener('unhandledrejection', (event) => {"
                                         " console.error('Uncaught (in promise)', event.reason);"
                                         "});"_kj;

const PythonConfig REPL_PYTHON_CONFIG{
  .packageDiskCacheRoot = kj::none, .createSnapshot = false, .createBaselineSnapshot = false};

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

// The compatibility flags of the REPL's JavaScript: `nodejs_compat` unless the user's flags take a
// position on it, so that `await import('node:assert')` works.
kj::Array<kj::String> replCompatibilityFlags(kj::ArrayPtr<const kj::String> userFlags) {
  kj::Vector<kj::String> flags(userFlags.size() + 1);
  bool nodejsCompatChosen = false;
  for (auto& flag: userFlags) {
    nodejsCompatChosen = nodejsCompatChosen || flag == "nodejs_compat"_kj ||
        flag == "no_nodejs_compat"_kj || flag == "nodejs_compat_v2"_kj ||
        flag == "no_nodejs_compat_v2"_kj;
    flags.add(kj::str(flag));
  }
  if (!nodejsCompatChosen) {
    flags.add(kj::str("nodejs_compat"));
  }
  return flags.releaseAsArray();
}

}  // namespace

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
    ReplConfigLoader loadConfig,
    ReplEnvironmentOptions optionsParam)
    : resources(resources),
      configLoader(kj::mv(loadConfig)),
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
  compileCompatibilityFlags(date, replCompatibilityFlags(options.compatibilityFlags), flags,
      errorReporter, options.experimental, CompatibilityDateValidation::FUTURE_FOR_TEST, nullptr);
  // Rendering results needs the new module registry's internal modules.
  flags.setNewModuleRegistry(true);
  featureFlags = flags.asReader();
  errorReporter.check("compatibility flags");

  compileWorker();
  headerTable = headerTableBuilder.build();
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
    jsg::NonModuleScript::compile(js, STARTUP_SCRIPT, "repl-startup.js"_kj).run(js);
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

kj::Promise<kj::String> ReplEnvironment::evaluate(kj::Array<Candidate> candidates) {
  KJ_REQUIRE(candidates.size() > 0, "nothing to evaluate");
  auto& context = KJ_REQUIRE_NONNULL(incomingRequest, "the REPL session has ended")->getContext();

  kj::Maybe<kj::String> rendered;
  kj::Maybe<Awaiting> awaiting;
  co_await context.run([&](Worker::Lock& lock, IoContext& context) {
    auto& js = jsg::Lock::from(lock.getIsolate());
    KJ_SWITCH_ONEOF(run(js, context, candidates)) {
      KJ_CASE_ONEOF(text, kj::String) {
        rendered = kj::mv(text);
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
      rendered = promise.state() == jsg::PromiseState::REJECTED
          ? kj::str("Uncaught ", describeException(js, promise.result()))
          : render(js, promise.result());
    });
  }

  co_return KJ_ASSERT_NONNULL(kj::mv(rendered));
}

kj::OneOf<kj::String, ReplEnvironment::Awaiting> ReplEnvironment::run(
    jsg::Lock& js, IoContext& context, kj::ArrayPtr<const Candidate> candidates) {
  using Outcome = kj::OneOf<kj::String, Awaiting>;
  return js.tryCatch([&]() -> Outcome {
    auto compiled = compile(js, candidates);
    auto result = compiled.script.runAndReturn(js);
    if (!compiled.awaited || !result.isPromise()) {
      // A promise the user did not await renders as a promise: awaiting it could hang the REPL on
      // one that never settles.
      return render(js, result);
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
    return kj::str("Uncaught ", describeException(js, jsg::JsValue(exception.getHandle(js))));
  });
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
  // None compiles, so the first one's syntax error is thrown.
  return {.script = jsg::NonModuleScript::compile(js, candidates[0].source, inputName),
    .awaited = candidates[0].awaited};
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

kj::Promise<void> ReplEnvironment::drain() {
  for (auto& server: servers) {
    co_await server->close();
  }
  servers.clear();

  KJ_IF_SOME(request, incomingRequest) {
    auto draining = kj::mv(request);
    incomingRequest = kj::none;
    draining->drain(waitUntilTasks, kj::mv(draining));
    co_await waitUntilTasks.onEmpty();
  }
}

kj::Promise<kj::Own<ReplServerHost>> ReplEnvironment::loadConfig(
    kj::String path, kj::Maybe<kj::String> constName) {
  // JavaScript asked for the load under the REPL's isolate lock; the server's workers lock
  // isolates of their own, so the load runs on the event loop.
  co_await kj::yield();
  auto config = configLoader(
      path, constName.map([](kj::String& name) { return name.asPtr(); }), options.experimental);
  auto server = co_await ReplConfigServer::start(
      resources, *headerTable, kj::mv(path), config, options.experimental);
  // Drop the servers that were closed since the last load.
  kj::Vector<kj::Own<ReplConfigServer>> open(servers.size() + 1);
  for (auto& s: servers) {
    if (!s->isClosed()) open.add(kj::mv(s));
  }
  open.add(kj::addRef(*server));
  servers = kj::mv(open);
  co_return server;
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
