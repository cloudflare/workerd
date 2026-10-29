// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/tracing.h>
#include <workerd/io/io-context.h>
#include <workerd/io/trace-stream.h>
#include <workerd/tests/test-fixture.h>

#include <capnp/message.h>

namespace workerd {
namespace {

SpanParent observedParent(uint64_t id) {
  return SpanParent::fromSpanContext(
      tracing::SpanContext(tracing::TraceId(1, 2), tracing::SpanId(id)));
}

kj::Rc<WorkerTracer> makeTracer() {
  auto trace = kj::refcounted<Trace>(kj::none, kj::none, kj::none, kj::none, kj::none,
      kj::Array<kj::String>(), kj::none, ExecutionModel::STATELESS);
  return kj::rc<WorkerTracer>(kj::none, kj::mv(trace), PipelineLogLevel::NONE, kj::none, kj::none);
}

KJ_TEST("legacy request-span factories retain their two-argument source interface") {
  auto tracer = makeTracer();
  bool called = false;
  BaseTracer::MakeUserRequestSpanFunc legacy = [&](tracing::TraceId id,
                                                   kj::Maybe<tracing::TraceFlags> flags) {
    called = true;
    KJ_EXPECT(id == tracing::TraceId(1, 2));
    KJ_EXPECT(flags == tracing::TraceFlags(3));
    return observedParent(42);
  };
  tracer->setMakeUserRequestSpanFunc(kj::mv(legacy));
  auto parent = tracer->makeUserRequestSpan(
      tracing::TraceId(1, 2), tracing::TraceFlags(3), kj::str("vendor=legacy"));
  KJ_EXPECT(called);
  auto identity = KJ_ASSERT_NONNULL(parent.toSpanContext());
  KJ_EXPECT(KJ_ASSERT_NONNULL(identity.getSpanId()) == tracing::SpanId(42));
}

KJ_TEST("legacy request-span factories decline external parents without losing their identity") {
  capnp::MallocMessageBuilder message;
  auto features = message.initRoot<CompatibilityFlags>();
  features.setOpentelemetryApi(true);
  TestFixture fixture({.featureFlags = features.asReader()});
  auto context = fixture.newIoContext();
  auto tracer = makeTracer();
  int calls = 0;
  tracer->setMakeUserRequestSpanFunc([&](tracing::TraceId, kj::Maybe<tracing::TraceFlags>) {
    ++calls;
    return observedParent(42);
  });
  auto request = fixture.newUndeliveredIncomingRequest(*context, kj::mv(tracer));
  request->delivered();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    auto scope = context->makeRequestTraceScope(env.lock);
    auto primitive = env.js.alloc<api::TracingContext>();
    primitive->withActiveSpanContext(env.js,
        api::user_tracing::TracingSpanContext{.traceId = tracing::TraceId(3, 4).toW3C(),
          .spanId = tracing::SpanId(55).toGoString(),
          .traceFlags = 1,
          .traceState = kj::str("vendor=foreign")},
        jsg::Function<jsg::JsRef<jsg::JsValue>()>([&](jsg::Lock& js) {
      auto identity = KJ_ASSERT_NONNULL(context->getCurrentUserTraceSpan().toSpanContext());
      KJ_EXPECT(identity.getTraceId() == tracing::TraceId(3, 4));
      KJ_EXPECT(KJ_ASSERT_NONNULL(identity.getSpanId()) == tracing::SpanId(55));
      KJ_EXPECT(KJ_ASSERT_NONNULL(identity.cloneTraceState()) == "vendor=foreign");
      auto child = context->makeUserTraceSpan("legacy-external-child"_kjc);
      auto childIdentity = KJ_ASSERT_NONNULL(child.getUserSpanParent().toSpanContext());
      KJ_EXPECT(KJ_ASSERT_NONNULL(childIdentity.getSpanId()) == tracing::SpanId(55));
      return jsg::JsRef(js, js.undefined());
    }));
  });
  KJ_EXPECT(calls == 1);
  fixture.drainAndDestroy(kj::mv(request));
}

kj::Rc<BaseTracer> makeObservedTracer() {
  auto tracer = makeTracer();
  tracer->setMakeUserRequestSpanContextFunc([](tracing::SpanContext) { return observedParent(2); });
  return kj::mv(tracer);
}

class ObservedRequest final: public RequestObserver {
 public:
  SpanParent getSpan() override {
    return observedParent(1);
  }
};

uint64_t spanId(SpanParent parent) {
  auto context = parent.toSpanContext();
  return KJ_ASSERT_NONNULL(KJ_ASSERT_NONNULL(context).getSpanId()).getId();
}

jsg::Ref<api::user_tracing::Span> activeSpan(const TestFixture::Environment& env) {
  auto tracing = env.js.alloc<api::Tracing>();
  auto& handler = KJ_ASSERT_NONNULL(env.js.tryGetTypeHandler<jsg::Ref<api::user_tracing::Span>>());
  auto span = tracing->getActiveSpan(env.js, handler);
  return kj::mv(KJ_ASSERT_NONNULL(span));
}

class TraceTaskErrors final: public kj::TaskSet::ErrorHandler {
 public:
  void taskFailed(kj::Exception&& exception) override {
    KJ_FAIL_EXPECT(exception);
  }
};

// Deliver real tail-stream events to a fixture's JS handler.
class FixtureTail final: public WorkerInterface {
 public:
  FixtureTail(TestFixture& fixture, kj::TaskSet& tasks): fixture(fixture), tasks(tasks) {}
  kj::Promise<void> request(kj::HttpMethod,
      kj::StringPtr,
      const kj::HttpHeaders&,
      kj::AsyncInputStream&,
      kj::HttpService::Response&) override {
    KJ_FAIL_ASSERT("unexpected HTTP request");
  }
  kj::Promise<void> connect(kj::StringPtr,
      const kj::HttpHeaders&,
      kj::AsyncIoStream&,
      ConnectResponse&,
      kj::HttpConnectSettings) override {
    KJ_FAIL_ASSERT("unexpected connect");
  }
  kj::Promise<void> prewarm(kj::StringPtr) override {
    return kj::READY_NOW;
  }
  kj::Promise<ScheduledResult> runScheduled(kj::Date, kj::StringPtr) override {
    KJ_FAIL_ASSERT("unexpected scheduled event");
  }
  kj::Promise<AlarmResult> runAlarm(kj::Date, uint32_t) override {
    KJ_FAIL_ASSERT("unexpected alarm");
  }
  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override {
    auto context = fixture.newIoContext();
    return event
        ->run(fixture.newUndeliveredIncomingRequest(*context), kj::none, kj::none, Frankenvalue(),
            tasks)
        .attach(kj::mv(event), kj::mv(context));
  }

 private:
  TestFixture& fixture;
  kj::TaskSet& tasks;
};

class FixtureSpanSubmitter final: public SpanSubmitter {
 public:
  enum class Registration { ENABLED, DISABLED };
  FixtureSpanSubmitter(BaseTracer& tracer,
      tracing::SpanContext context,
      Registration registration = Registration::ENABLED)
      : tracer(tracer.getWeakRef()),
        context(kj::mv(context)),
        registration(registration) {}
  bool submitSpanOpen(
      tracing::SpanId id, tracing::SpanId parent, kj::ConstString name, kj::Date time) override {
    if (name == "rejected") return false;
    return tracer->runIfAlive([&](BaseTracer& tracer) {
      if (registration == Registration::ENABLED)
        tracer.setUserSpanContext(
            id, tracing::SpanContext(context.getTraceId(), id, context.getTraceFlags()));
      tracer.addSpanOpen(id, parent, kj::mv(name), time);
    });
  }
  void submitSpanClose(
      tracing::SpanId id, kj::Date start, kj::Date end, Span::TagMap&& tags) override {
    tracer->runIfAlive([&](BaseTracer& tracer) {
      tracer.addSpanClose(tracing::SpanEndData(id, end, kj::mv(tags)), start);
    });
  }
  void submitSpanUpdate(tracing::SpanId id, tracing::SpanUpdate&& update) override {
    tracer->runIfAlive([&](BaseTracer& tracer) { tracer.addSpanUpdate(id, kj::mv(update)); });
  }
  void submitSpanException(tracing::SpanId id,
      kj::Date time,
      kj::Maybe<tracing::Exception::Code> code,
      kj::String name,
      kj::String message,
      kj::Maybe<kj::String> stack) override {
    tracer->runIfAlive([&](BaseTracer& tracer) {
      tracer.addSpanException(id, time, kj::mv(code), kj::mv(name), kj::mv(message), kj::mv(stack));
    });
  }
  tracing::SpanId makeSpanId() override {
    return tracing::SpanId::fromEntropy();
  }
  void submitSpanEvent(tracing::SpanId id, tracing::SpanEvent event) override {
    tracer->runIfAlive([&](BaseTracer& tracer) { tracer.addSpanEvent(id, kj::mv(event)); });
  }
  void submitSpanLinks(tracing::SpanId id, tracing::SpanLinks links) override {
    tracer->runIfAlive([&](BaseTracer& tracer) { tracer.addSpanLinks(id, kj::mv(links)); });
  }
  void submitSpanExceptionWithTimestamp(tracing::SpanId id, tracing::Exception exception) override {
    tracer->runIfAlive(
        [&](BaseTracer& tracer) { tracer.addSpanExceptionWithTimestamp(id, kj::mv(exception)); });
  }

 private:
  kj::Own<BaseTracer::WeakRef> tracer;
  tracing::SpanContext context;
  Registration registration;
};

KJ_TEST("native children preserve explicit 03 in JS and actual tail-stream span events") {
  capnp::MallocMessageBuilder message;
  auto features = message.initRoot<CompatibilityFlags>();
  features.setOpentelemetryApi(true);
  TestFixture fixture({.featureFlags = features.asReader(), .mainModuleSource = R"JS(
    globalThis.childEvents = 0;
    export default {
      tailStream(onset) {
        if (onset.spanContext.traceFlags !== 3) throw new Error('onset flags');
        return { spanOpen: event => {
          if (event.spanContext.traceFlags !== 3) throw new Error('child flags');
          if (event.event.name === 'forgot-registration' &&
              (event.event.root !== undefined || event.spanContext.spanId !== onset.event.spanId))
            throw new Error('missing registration did not attach');
          if (event.event.name === 'registered-root' &&
              (event.event.root !== true || event.spanContext.spanId !== undefined ||
               event.spanContext.traceId === onset.spanContext.traceId))
            throw new Error('registered root marker');
          if (event.event.type === 'spanOpen') globalThis.childEvents++;
        } };
      }
    };
  )JS"_kj});
  TraceTaskErrors errors;
  kj::TaskSet tasks(errors);
  auto tails = kj::heapArray<kj::Own<WorkerInterface>>(1);
  tails[0] = kj::heap<FixtureTail>(fixture, tasks);
  auto trace = kj::refcounted<Trace>(kj::none, kj::none, kj::none, kj::none, kj::none,
      kj::Array<kj::String>(), kj::none, ExecutionModel::STATELESS);
  auto tracer = kj::rc<WorkerTracer>(kj::none, kj::mv(trace), PipelineLogLevel::FULL, kj::none,
      kj::heap<tracing::TailStreamWriter>(kj::mv(tails), tasks));
  tracer->setMakeUserRequestSpanContextFunc([&](tracing::SpanContext identity) {
    auto submitter =
        kj::refcounted<FixtureSpanSubmitter>(*tracer, tracing::SpanContext::clone(identity));
    return SpanParent(kj::rc<UserSpanObserver>(
        kj::mv(submitter), identity.getTraceId(), identity.getTraceFlags()));
  });
  auto context = fixture.newIoContext();
  auto request = fixture.newUndeliveredIncomingRequest(*context, tracer.clone());
  auto& invocation = request->getInvocationSpanContext();
  invocation = tracing::InvocationSpanContext(invocation.getTraceId(), invocation.getInvocationId(),
      invocation.getSpanId(), tracing::TraceFlags(3));
  request->delivered();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    tracer->setEventInfo(*request, tracing::CustomEventInfo());
    tracer->addSpanOpen(
        tracing::SpanId(900), tracing::SpanId::nullId, "forgot-registration"_kjc, kj::UNIX_EPOCH);
    tracer->addSpanClose(
        tracing::SpanEndData(tracing::SpanId(900), kj::UNIX_EPOCH), kj::UNIX_EPOCH);
    tracer->setUserSpanContext(tracing::SpanId(901),
        tracing::SpanContext(
            tracing::TraceId(17, 18), tracing::SpanId(901), tracing::TraceFlags(3)));
    tracer->addSpanOpen(
        tracing::SpanId(901), tracing::SpanId::nullId, "registered-root"_kjc, kj::UNIX_EPOCH);
    tracer->addSpanClose(
        tracing::SpanEndData(tracing::SpanId(901), kj::UNIX_EPOCH), kj::UNIX_EPOCH);
    auto scope = context->makeRequestTraceScope(env.lock);
    auto tracing = env.js.alloc<api::Tracing>();
    auto child = context->getCurrentUserTraceSpan().newChild("native-child"_kjc);
    auto nested = context->makeUserAsyncTraceScope(env.lock, SpanParent(child));
    KJ_EXPECT(activeSpan(env)->spanContext().traceFlags == 3);
    auto grandchild = tracing->startSpan(env.js, kj::str("native-grandchild"));
    KJ_EXPECT(grandchild->spanContext().traceFlags == 3);
    grandchild->end();
    auto script = jsg::check(v8::Script::Compile(env.js.v8Context(), env.js.str(R"JS(
      const api = globalThis[Symbol.for('opentelemetry.js.api.1')];
      const span = api.trace.getTracer('ignored-marks').startSpan('marked');
      span.addEvent('ignored');
      span.addLink({ context: span.spanContext() });
      span.end();
    )JS"_kj)));
    jsg::check(script->Run(env.js.v8Context()));
    child.end();
  });
  fixture.drainAndDestroy(kj::mv(request));
  tracer = nullptr;
  tasks.onEmpty().wait(fixture.getWaitScope());
  fixture.runInIoContext([](const TestFixture::Environment& env) {
    auto script = jsg::check(v8::Script::Compile(env.js.v8Context(),
        env.js.str(
            "if (globalThis.childEvents !== 5) throw new Error('missing tail child events');"_kj)));
    jsg::check(script->Run(env.js.v8Context()));
  });
}

enum class UserSpanFactory { PRESENT, ABSENT };

KJ_TEST("root factory capability keeps JS and tail identities aligned and rewraps retain flags") {
  enum class Factory { LEGACY, CONTEXT, DETACHED };
  for (auto factory: {Factory::LEGACY, Factory::CONTEXT, Factory::DETACHED}) {
    for (bool explicitFlags: {false, true}) {
      capnp::MallocMessageBuilder message;
      auto features = message.initRoot<CompatibilityFlags>();
      features.setOpentelemetryApi(true);
      TestFixture fixture({.featureFlags = features.asReader(), .mainModuleSource = R"JS(
        import { tracing } from 'cloudflare:workers';
        globalThis.nativeTracing = tracing;
        globalThis.events = [];
        const api = globalThis[Symbol.for('opentelemetry.js.api.1')];
        globalThis.moduleRoot = api.trace.getTracer('module').startSpan('module-root', {root:true});
        export default {
          tailStream(onset) {
            events.push(onset);
            return event => events.push(event);
          }
        };
      )JS"_kj});
      TraceTaskErrors errors;
      kj::TaskSet tasks(errors);
      auto tails = kj::heapArray<kj::Own<WorkerInterface>>(1);
      tails[0] = kj::heap<FixtureTail>(fixture, tasks);
      auto trace = kj::refcounted<Trace>(kj::none, kj::none, kj::none, kj::none, kj::none,
          kj::Array<kj::String>(), kj::none, ExecutionModel::STATELESS);
      auto tracer = kj::rc<WorkerTracer>(kj::none, kj::mv(trace), PipelineLogLevel::FULL, kj::none,
          kj::heap<tracing::TailStreamWriter>(kj::mv(tails), tasks));
      auto makeParent = [&](tracing::SpanContext identity) {
        return SpanParent(kj::rc<UserSpanObserver>(
            kj::refcounted<FixtureSpanSubmitter>(*tracer, tracing::SpanContext::clone(identity),
                factory == Factory::DETACHED ? FixtureSpanSubmitter::Registration::ENABLED
                                             : FixtureSpanSubmitter::Registration::DISABLED),
            tracing::SpanContext::clone(identity)));
      };
      if (factory == Factory::LEGACY) {
        tracer->setMakeUserRequestSpanFunc(
            [&](tracing::TraceId id, kj::Maybe<tracing::TraceFlags> flags) {
          return makeParent(tracing::SpanContext(id, kj::none, flags));
        });
      } else if (factory == Factory::CONTEXT) {
        tracer->setMakeUserRequestSpanContextFunc(makeParent);
      } else {
        tracer->setMakeUserSpanParentFunc([&](tracing::SpanContext identity, DetachedRoot) {
          return makeParent(kj::mv(identity));
        });
      }
      KJ_EXPECT(tracer->supportsDetachedRoots() == (factory == Factory::DETACHED));
      auto context = fixture.newIoContext();
      auto request = fixture.newUndeliveredIncomingRequest(*context, tracer.clone());
      if (explicitFlags) {
        auto& invocation = request->getInvocationSpanContext();
        invocation = tracing::InvocationSpanContext(invocation.getTraceId(),
            invocation.getInvocationId(), invocation.getSpanId(), tracing::TraceFlags(3));
      }
      auto run = [](const TestFixture::Environment& env, kj::StringPtr source) {
        auto script = jsg::check(v8::Script::Compile(env.js.v8Context(), env.js.str(source)));
        jsg::check(script->Run(env.js.v8Context()));
      };
      request->delivered();
      fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
        tracer->setEventInfo(*request, tracing::CustomEventInfo());
        auto scope = context->makeRequestTraceScope(env.lock);
        run(env,
            factory == Factory::DETACHED ? "globalThis.detached = true"_kj
                                         : "globalThis.detached = false"_kj);
        run(env, R"JS(
          const api = globalThis[Symbol.for('opentelemetry.js.api.1')];
          const tracer = api.trace.getTracer('factory');
          const key = Symbol.for('OpenTelemetry Context Key SPAN');
          globalThis.rootIdentity = undefined;
          const root = tracer.startSpan('factory-root', {root:true});
          globalThis.rootIdentity = root.spanContext();
          api.context.with(api.context.active().setValue(key, root), () => {
            console.log('factory-root-log');
            nativeTracing.startSpan('factory-native').end();
          });
          root.end();
          // Replay the serializable identity outside activation, and after the root has closed.
          const rewrapped = { spanContext: () => rootIdentity };
          api.context.with(api.context.active().setValue(key, rewrapped), () => {
            tracer.startSpan('rewrapped-child').end();
            console.log('rewrapped-log');
          });
          // An INVALID root from module evaluation must not suppress this traced request.
          api.context.with(api.context.active().setValue(key, moduleRoot), () => {
            nativeTracing.startSpan('reused-invalid-native').end();
          });
        )JS"_kj);
        auto primitive = env.js.alloc<api::TracingContext>();
        auto foreign = [](uint8_t flags) {
          return api::user_tracing::TracingSpanContext{
            .traceId = kj::str("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
            .spanId = kj::str("bbbbbbbbbbbbbbbb"),
            .traceFlags = flags};
        };
        primitive->withActiveSpanContext(
            env.js, foreign(1), jsg::Function<jsg::JsRef<jsg::JsValue>()>([&](jsg::Lock& js) {
          return primitive->withActiveSpanContext(
              js, foreign(0), jsg::Function<jsg::JsRef<jsg::JsValue>()>([&](jsg::Lock& js) {
            auto identity = KJ_ASSERT_NONNULL(context->getCurrentUserTraceSpan().toSpanContext());
            KJ_EXPECT(identity.getTraceFlags() == tracing::TraceFlags(0));
            return jsg::JsRef(js, js.undefined());
          }));
        }));
      });
      fixture.drainAndDestroy(kj::mv(request));
      tracer = nullptr;
      tasks.onEmpty().wait(fixture.getWaitScope());
      fixture.runInIoContext([&](const TestFixture::Environment& env) {
        run(env, R"JS(
          const onset = events.find(e => e.event.type === 'onset');
          const openedRoot = events.find(e => e.event.name === 'factory-root');
          const reused = events.find(e => e.event.name === 'reused-invalid-native');
          if (!openedRoot || openedRoot.spanContext.traceId !== rootIdentity.traceId) throw new Error('JS/tail root trace');
          if ((openedRoot.event.root === true) !== detached) throw new Error('root capability');
          if ((rootIdentity.traceId !== onset.spanContext.traceId) !== detached) throw new Error('root identity');
          for (const event of events) {
            if (event.spanContext.traceFlags !== onset.spanContext.traceFlags) throw new Error('native flags');
            if (['log','spanClose'].includes(event.event.type) &&
                event.spanContext.spanId !== reused?.event.spanId && event.spanContext.traceId !== rootIdentity.traceId)
              throw new Error('log/close trace');
          }
          for (const name of ['factory-native', ...(detached ? ['rewrapped-child'] : []), 'reused-invalid-native']) {
            if (!events.some(e => e.event.name === name)) throw new Error('missing ' + name);
          }
        )JS"_kj);
      });
    }
  }
}

KJ_TEST("actor span marks use their creator stream and rejected spans emit nothing") {
  for (bool detached: {false, true}) {
    capnp::MallocMessageBuilder message;
    auto features = message.initRoot<CompatibilityFlags>();
    features.setOpentelemetryApi(true);
    TestFixture fixture({.featureFlags = features.asReader(),
      .mainModuleSource = R"JS(
      globalThis.events = [];
      export default {
        tailStream(onset) {
          events.push(onset);
          const collect = event => events.push(event);
          return { spanOpen: collect, spanClose: collect, spanEvent: collect,
            spanLinks: collect, exception: collect, outcome: collect };
        }
      };
    )JS"_kj,
      .actorId = Worker::Actor::Id(kj::str("span-mark-owner"))});
    TraceTaskErrors errors;
    kj::TaskSet tasks(errors);
    auto makeStreamingTracer = [&]() {
      auto tails = kj::heapArray<kj::Own<WorkerInterface>>(1);
      tails[0] = kj::heap<FixtureTail>(fixture, tasks);
      auto trace = kj::refcounted<Trace>(kj::none, kj::none, kj::none, kj::none, kj::none,
          kj::Array<kj::String>(), kj::none, ExecutionModel::STATELESS);
      auto tracer = kj::rc<WorkerTracer>(kj::none, kj::mv(trace), PipelineLogLevel::FULL, kj::none,
          kj::heap<tracing::TailStreamWriter>(kj::mv(tails), tasks));
      tracer->setMakeUserSpanParentFunc(
          [ptr = tracer.get()](tracing::SpanContext identity, DetachedRoot) {
        return SpanParent(kj::rc<UserSpanObserver>(
            kj::refcounted<FixtureSpanSubmitter>(*ptr, tracing::SpanContext::clone(identity)),
            identity.getTraceId(), identity.getTraceFlags()));
      });
      return tracer;
    };
    auto run = [](const TestFixture::Environment& env, kj::StringPtr source) {
      auto script = jsg::check(v8::Script::Compile(env.js.v8Context(), env.js.str(source)));
      jsg::check(script->Run(env.js.v8Context()));
    };
    auto context = fixture.newIoContext();
    auto firstTracer = makeStreamingTracer();
    auto first = fixture.newUndeliveredIncomingRequest(*context, firstTracer.clone());
    first->delivered();
    fixture.enterContext(*first, [&](const TestFixture::Environment& env) {
      firstTracer->setEventInfo(*first, tracing::CustomEventInfo());
      auto scope = context->makeRequestTraceScope(env.lock);
      run(env, detached ? "globalThis.detached = true"_kj : "globalThis.detached = false"_kj);
      run(env, R"JS(
      globalThis.api = globalThis[Symbol.for('opentelemetry.js.api.1')];
      globalThis.tracer = api.trace.getTracer('owner');
      globalThis.creatorInvocation = api.context.active().getValue(
        Symbol.for('OpenTelemetry Context Key SPAN'));
      globalThis.kept = tracer.startSpan('kept', {root: detached});
      globalThis.link = { context: { traceId: '00000000000000001111111111111111',
        spanId: '2222222222222222', traceFlags: 1 } };
      globalThis.mark = span => {
        span.addEvent('owner-event', {}, [1700000000, 1]);
        span.addLinks([link, link]);
        span.recordException(new Error('owner-exception'), [1700000000, 2]);
      };
      mark(creatorInvocation);
      const rejected = tracer.startSpan('rejected');
      mark(rejected);
      rejected.end();
    )JS"_kj);
    });
    auto secondTracer = makeStreamingTracer();
    auto second = fixture.newUndeliveredIncomingRequest(*context, secondTracer.clone());
    second->delivered();
    fixture.enterContext(*second, [&](const TestFixture::Environment& env) {
      secondTracer->setEventInfo(*second, tracing::CustomEventInfo());
      auto scope = context->makeRequestTraceScope(env.lock);
      run(env, "mark(kept); mark(creatorInvocation); kept.end();"_kj);
    });
    fixture.drainAndDestroy(kj::mv(first));
    fixture.drainAndDestroy(kj::mv(second));
    firstTracer = nullptr;
    secondTracer = nullptr;
    tasks.onEmpty().wait(fixture.getWaitScope());
    fixture.runInIoContext([&](const TestFixture::Environment& env) {
      run(env, R"JS(
      const opens = events.filter(e => e.event.type === 'spanOpen');
      if (opens.length !== 1 || opens[0].event.name !== 'kept') throw new Error('rejected open');
      const owner = opens[0].invocationId;
      if (detached && (opens[0].event.root !== true || opens[0].spanContext.spanId !== undefined))
        throw new Error('actor root not detached');
      const marks = events.filter(e => ['spanEvent', 'spanLinks', 'exception'].includes(e.event.type));
      if (marks.length !== 9) throw new Error('missing or leaked marks: ' + marks.length);
      const rootId = events.find(e => e.event.type === 'onset' && e.invocationId === owner).event.spanId;
      for (const e of marks) {
        if (e.invocationId !== owner) throw new Error('wrong creator stream');
        if (![opens[0].event.spanId, rootId].includes(e.spanContext.spanId)) throw new Error('wrong span ID');
        const expected = e.spanContext.spanId === opens[0].event.spanId ? opens[0] : events.find(e => e.event.type === 'onset' && e.invocationId === owner);
        if (e.spanContext.traceId !== expected.spanContext.traceId) throw new Error('wrong trace');
      }
      if (marks.filter(e => e.spanContext.spanId === rootId).length !== 6) throw new Error('invocation marks');
      if (marks.filter(e => e.event.type === 'spanLinks').some(e => e.event.links.length !== 2))
        throw new Error('links not batched');
    )JS"_kj);
    });
  }
}

KJ_TEST("a native trigger without a tracer remains a propagation-only root") {
  capnp::MallocMessageBuilder message;
  auto features = message.initRoot<CompatibilityFlags>();
  features.setOpentelemetryApi(true);
  TestFixture fixture({.featureFlags = features.asReader()});
  for (int flags: {-1, 0, 1, 255}) {
    auto context = fixture.newIoContext();
    auto nativeFlags = flags < 0 ? kj::Maybe<tracing::TraceFlags>(kj::none)
                                 : kj::Maybe<tracing::TraceFlags>(tracing::TraceFlags(flags));
    auto trigger = tracing::InvocationSpanContext(tracing::TraceId(1, 2), tracing::TraceId::nullId,
        tracing::SpanId(42), nativeFlags, kj::str("vendor=native"));
    auto request = fixture.newUndeliveredIncomingRequest(*context, kj::none, trigger.clone());
    request->delivered();
    fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
      auto scope = context->makeRequestTraceScope(env.lock);
      auto identity = KJ_ASSERT_NONNULL(context->getCurrentUserTraceSpan().toSpanContext());
      KJ_EXPECT(identity.getTraceId() == trigger.getTraceId());
      KJ_EXPECT(KJ_ASSERT_NONNULL(identity.getSpanId()) == trigger.getSpanId());
      KJ_EXPECT(identity.getTraceFlags() == nativeFlags);
      KJ_EXPECT(KJ_ASSERT_NONNULL(identity.cloneTraceState()) == "vendor=native");
      auto span = activeSpan(env);
      KJ_EXPECT(!span->getIsTraced());
      KJ_EXPECT(span->spanContext().traceId == trigger.getTraceId().toW3C());
      KJ_EXPECT(span->spanContext().spanId == trigger.getSpanId().toGoString());
      KJ_EXPECT(span->spanContext().traceFlags == (flags < 0 ? 0 : flags & 3));
      KJ_EXPECT(context->getWorkerTracer() == kj::none);
      auto child = context->makeUserTraceSpan("untraced-native-child"_kjc);
      auto propagated = KJ_ASSERT_NONNULL(child.getUserSpanParent().toSpanContext());
      KJ_EXPECT(propagated.getTraceId() == trigger.getTraceId());
      KJ_EXPECT(propagated.getTraceFlags() == nativeFlags);
    });
    fixture.drainAndDestroy(kj::mv(request));
  }
}

kj::Rc<WorkerTracer> makeSinkTracer(kj::TaskSet& tasks,
    bool streaming,
    PipelineLogLevel level = PipelineLogLevel::FULL,
    UserSpanFactory factory = UserSpanFactory::PRESENT) {
  auto trace = kj::refcounted<Trace>(kj::none, kj::none, kj::none, kj::none, kj::none,
      kj::Array<kj::String>(), kj::none, ExecutionModel::STATELESS);
  kj::Maybe<kj::Own<tracing::TailStreamWriter>> writer;
  if (streaming)
    writer = kj::heap<tracing::TailStreamWriter>(tracing::TailStreamWriter::Pending(), tasks);
  auto tracer = kj::rc<WorkerTracer>(kj::none, kj::mv(trace), level, kj::none, kj::mv(writer));
  tracer->markUnused();
  if (factory == UserSpanFactory::PRESENT) {
    tracer->setMakeUserRequestSpanContextFunc([](tracing::SpanContext identity) {
      return SpanParent::fromSpanContext(kj::mv(identity));
    });
  }
  return tracer;
}

KJ_TEST("rebuilt contexts inherit native invocation flags with or without a user-span factory") {
  TraceTaskErrors errors;
  kj::TaskSet tasks(errors);
  for (auto factory: {UserSpanFactory::PRESENT, UserSpanFactory::ABSENT}) {
    for (bool streaming: {false, true}) {
      for (auto level: {PipelineLogLevel::NONE, PipelineLogLevel::FULL}) {
        for (int nativeFlags: {-1, 0, 1, 3}) {
          capnp::MallocMessageBuilder message;
          auto features = message.initRoot<CompatibilityFlags>();
          features.setOpentelemetryApi(true);
          TestFixture fixture({.featureFlags = features.asReader()});
          auto context = fixture.newIoContext();
          auto request = fixture.newUndeliveredIncomingRequest(
              *context, makeSinkTracer(tasks, streaming, level, factory));
          auto& invocation = request->getInvocationSpanContext();
          if (nativeFlags >= 0) {
            invocation = tracing::InvocationSpanContext(invocation.getTraceId(),
                invocation.getInvocationId(), invocation.getSpanId(),
                tracing::TraceFlags(nativeFlags));
          }
          request->delivered();
          fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
            auto scope = context->makeRequestTraceScope(env.lock);
            auto primitive = env.js.alloc<api::TracingContext>();
            for (bool sameTrace: {false, true}) {
              for (uint8_t jsFlags: {0, 1, 3}) {
                auto expectedFlags = sameTrace
                    ? invocation.getTraceFlags()
                    : kj::Maybe<tracing::TraceFlags>(tracing::TraceFlags(jsFlags & 1));
                primitive->withActiveSpanContext(env.js,
                    api::user_tracing::TracingSpanContext{
                      .traceId =
                          (sameTrace ? invocation.getTraceId() : tracing::TraceId(1, 2)).toW3C(),
                      .spanId = tracing::SpanId(42).toGoString(),
                      .traceFlags = jsFlags,
                    },
                    jsg::Function<jsg::JsRef<jsg::JsValue>()>([&](jsg::Lock& js) {
                  auto parent = context->getCurrentUserTraceSpan();
                  auto identity = KJ_ASSERT_NONNULL(parent.toSpanContext());
                  KJ_EXPECT(identity.getTraceFlags() == expectedFlags);
                  KJ_EXPECT(KJ_ASSERT_NONNULL(identity.getSpanId()) == tracing::SpanId(42));
                  KJ_EXPECT(context->getInvocationSpanContext().getTraceFlags() == expectedFlags);
                  if (factory == UserSpanFactory::ABSENT) {
                    auto child = parent.newChild("propagation-only-child"_kjc);
                    auto grandchild = SpanParent(child).newChild("propagation-only-grandchild"_kjc);
                    auto nestedIdentity = KJ_ASSERT_NONNULL(SpanParent(grandchild).toSpanContext());
                    KJ_EXPECT(nestedIdentity.getTraceFlags() == expectedFlags);
                    KJ_EXPECT(nestedIdentity.getTraceId() == identity.getTraceId());
                    auto tracing = js.alloc<api::Tracing>();
                    auto span = tracing->startSpan(js, kj::str("no-factory"));
                    KJ_EXPECT(!span->getIsTraced());
                  }
                  return jsg::JsRef(js, js.undefined());
                }));
              }
            }
            KJ_EXPECT(invocation.getTraceFlags() ==
                (nativeFlags < 0
                        ? kj::none
                        : kj::Maybe<tracing::TraceFlags>(tracing::TraceFlags(nativeFlags))));
          });
          fixture.drainAndDestroy(kj::mv(request));
        }
      }
    }
  }
}

KJ_TEST("JS sampling projection uses export capability without changing native decisions") {
  TraceTaskErrors errors;
  kj::TaskSet tasks(errors);
  for (bool enabled: {false, true}) {
    for (bool streaming: {false, true}) {
      for (auto level: {PipelineLogLevel::NONE, PipelineLogLevel::FULL}) {
        for (int flags: {-1, 0, 1, 3}) {
          capnp::MallocMessageBuilder message;
          auto features = message.initRoot<CompatibilityFlags>();
          features.setOpentelemetryApi(enabled);
          TestFixture fixture({.featureFlags = features.asReader()});
          auto context = fixture.newIoContext();
          auto tracer = makeSinkTracer(tasks, streaming, level);
          KJ_EXPECT(tracer->exportsUserSpans() == (streaming && level == PipelineLogLevel::FULL));
          auto request = fixture.newUndeliveredIncomingRequest(*context, kj::mv(tracer));
          auto& invocation = request->getInvocationSpanContext();
          if (flags >= 0) {
            invocation = tracing::InvocationSpanContext(invocation.getTraceId(),
                invocation.getInvocationId(), invocation.getSpanId(), tracing::TraceFlags(flags));
          }
          request->delivered();
          fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
            auto scope = context->makeRequestTraceScope(env.lock);
            auto expected = flags >= 0                                    ? flags
                : enabled && streaming && level == PipelineLogLevel::FULL ? 1
                                                                          : 0;
            KJ_EXPECT(activeSpan(env)->spanContext().traceFlags == expected);
            auto primitive = env.js.alloc<api::TracingContext>();
            KJ_EXPECT(KJ_ASSERT_NONNULL(primitive->getInvocationSpanContext(env.js)).traceFlags ==
                expected);
            auto native = KJ_ASSERT_NONNULL(context->getCurrentUserTraceSpan().toSpanContext());
            if (flags < 0) {
              KJ_EXPECT(native.getTraceFlags() == kj::none);
              KJ_EXPECT(context->getInvocationSpanContext().getTraceFlags() == kj::none);
            } else {
              KJ_EXPECT(KJ_ASSERT_NONNULL(native.getTraceFlags()) == tracing::TraceFlags(flags));
            }
          });
          fixture.drainAndDestroy(kj::mv(request));
        }
      }
    }
  }
}

KJ_TEST("owner-less tracing overrides are safe without an incoming request") {
  capnp::MallocMessageBuilder message;
  auto features = message.initRoot<CompatibilityFlags>();
  features.setOpentelemetryApi(true);
  TestFixture fixture({.featureFlags = features.asReader()});
  auto context = fixture.newIoContext();
  auto request = fixture.newIncomingRequest(*context);
  context
      ->run([&](Worker::Lock& lock) {
    // Cancel the completed event while still in the context's last JS turn.
    { auto finished = request->finishScheduled(kj::mv(request)); }
    jsg::Lock& js = lock;
    jsg::AsyncContextFrame::Scope fresh(js, kj::none);
    KJ_EXPECT(!context->hasCurrentIncomingRequest());
    auto primitive = js.alloc<api::TracingContext>();
    primitive->withActiveSpanContext(js, kj::none,
        jsg::Function<jsg::JsRef<jsg::JsValue>()>(
            [](jsg::Lock& js) { return jsg::JsRef(js, js.undefined()); }));
  }).wait(fixture.getWaitScope());
}

KJ_TEST("foreign trace owners use the current request") {
  for (bool observed: {false, true}) {
    capnp::MallocMessageBuilder message;
    auto features = message.initRoot<CompatibilityFlags>();
    features.setOpentelemetryApi(true);
    TestFixture fixture({.featureFlags = features.asReader()});
    auto sourceContext = fixture.newIoContext();
    auto source = fixture.newUndeliveredIncomingRequest(
        *sourceContext, observed ? kj::Maybe<kj::Rc<BaseTracer>>(makeObservedTracer()) : kj::none);
    source->delivered();
    kj::Maybe<jsg::Ref<jsg::AsyncContextFrame>> captured;
    fixture.enterContext(*source, [&](const TestFixture::Environment& env) {
      auto scope = sourceContext->makeRequestTraceScope(env.lock);
      captured = jsg::AsyncContextFrame::currentRef(env.js);
    });
    auto targetContext = fixture.newIoContext();
    auto tracer = makeObservedTracer();
    auto target = fixture.newUndeliveredIncomingRequest(*targetContext, tracer.clone());
    target->delivered();
    fixture.enterContext(*target, [&](const TestFixture::Environment& env) {
      jsg::AsyncContextFrame::Scope restored(env.js, captured);
      KJ_EXPECT(&KJ_ASSERT_NONNULL(targetContext->getWorkerTracer()) == tracer.get());
      auto nested = targetContext->makeUserAsyncTraceScope(env.lock, observedParent(9));
      KJ_EXPECT(&KJ_ASSERT_NONNULL(targetContext->getWorkerTracer()) == tracer.get());
      KJ_EXPECT(&KJ_ASSERT_NONNULL(targetContext->getCurrentUserTracer()) == tracer.get());
    });
    fixture.drainAndDestroy(kj::mv(source));
    fixture.drainAndDestroy(kj::mv(target));
  }
}

KJ_TEST("actor frames retain their JS sampling decision after their exporting request expires") {
  TraceTaskErrors errors;
  kj::TaskSet tasks(errors);
  capnp::MallocMessageBuilder message;
  auto features = message.initRoot<CompatibilityFlags>();
  features.setOpentelemetryApi(true);
  TestFixture fixture({.featureFlags = features.asReader(),
    .actorId = Worker::Actor::Id(kj::str("sampling-owner"))});
  auto context = fixture.newIoContext();
  auto first = fixture.newUndeliveredIncomingRequest(*context, makeSinkTracer(tasks, true));
  first->delivered();
  kj::Maybe<jsg::Ref<jsg::AsyncContextFrame>> captured;
  fixture.enterContext(*first, [&](const TestFixture::Environment& env) {
    auto scope = context->makeRequestTraceScope(env.lock);
    KJ_EXPECT(activeSpan(env)->spanContext().traceFlags == 1);
    captured = jsg::AsyncContextFrame::currentRef(env.js);
  });
  auto second = fixture.newUndeliveredIncomingRequest(*context, makeSinkTracer(tasks, false));
  second->delivered();
  fixture.drainAndDestroy(kj::mv(first));
  fixture.enterContext(*second, [&](const TestFixture::Environment& env) {
    auto current = context->makeRequestTraceScope(env.lock);
    KJ_EXPECT(activeSpan(env)->spanContext().traceFlags == 0);
    jsg::AsyncContextFrame::Scope restored(env.js, captured);
    KJ_EXPECT(activeSpan(env)->spanContext().traceFlags == 1);
    KJ_EXPECT(!activeSpan(env)->getIsTraced());
    auto nested = context->makeUserAsyncTraceScope(env.lock, observedParent(9));
    KJ_EXPECT(activeSpan(env)->spanContext().traceFlags == 1);
    KJ_EXPECT(context->getWorkerTracer() == kj::none);
  });
  fixture.drainAndDestroy(kj::mv(second));
}

KJ_TEST("current no-tracer actor frames never borrow a newer invocation's sampling decision") {
  capnp::MallocMessageBuilder message;
  auto features = message.initRoot<CompatibilityFlags>();
  features.setOpentelemetryApi(true);
  TestFixture fixture({.featureFlags = features.asReader(),
    .actorId = Worker::Actor::Id(kj::str("untraced-owner"))});
  TraceTaskErrors errors;
  kj::TaskSet tasks(errors);
  auto context = fixture.newIoContext();
  auto first = fixture.newIncomingRequest(*context);
  kj::Maybe<jsg::Ref<jsg::AsyncContextFrame>> captured;
  fixture.enterContext(*first, [&](const TestFixture::Environment& env) {
    auto scope = context->makeRequestTraceScope(env.lock);
    captured = jsg::AsyncContextFrame::currentRef(env.js);
  });
  auto second = fixture.newUndeliveredIncomingRequest(*context, makeSinkTracer(tasks, true));
  second->delivered();
  fixture.drainAndDestroy(kj::mv(first));
  fixture.enterContext(*second, [&](const TestFixture::Environment& env) {
    auto current = context->makeRequestTraceScope(env.lock);
    KJ_EXPECT(KJ_ASSERT_NONNULL(context->getDefaultJsTraceFlags()).isSampled());
    jsg::AsyncContextFrame::Scope restored(env.js, captured);
    KJ_EXPECT(context->getWorkerTracer() == kj::none);
    KJ_EXPECT(!KJ_ASSERT_NONNULL(context->getDefaultJsTraceFlags()).isSampled());
    KJ_EXPECT(context->getUserTraceInvocationContext() == kj::none);
    auto nested = context->makeUserAsyncTraceScope(env.lock, observedParent(9));
    KJ_EXPECT(context->getWorkerTracer() == kj::none);
    KJ_EXPECT(!KJ_ASSERT_NONNULL(context->getDefaultJsTraceFlags()).isSampled());
  });
  fixture.drainAndDestroy(kj::mv(second));
}

KJ_TEST("active invocation identity uses the observer only when the flag is enabled") {
  for (bool enabled: {false, true}) {
    capnp::MallocMessageBuilder message;
    auto features = message.initRoot<CompatibilityFlags>();
    features.setOpentelemetryApi(enabled);
    TestFixture fixture({.featureFlags = features.asReader()});
    auto context = fixture.newIoContext();
    auto request = fixture.newUndeliveredIncomingRequest(*context, makeObservedTracer());
    request->delivered();
    fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
      auto scope = context->makeRequestTraceScope(env.lock);
      auto identity = activeSpan(env)->spanContext();
      auto& invocation = request->getInvocationSpanContext();
      KJ_EXPECT(identity.traceId ==
          (enabled ? tracing::TraceId(1, 2).toW3C() : invocation.getTraceId().toW3C()));
      KJ_EXPECT(identity.spanId ==
          (enabled ? tracing::SpanId(2).toGoString() : invocation.getSpanId().toGoString()));
    });
    fixture.drainAndDestroy(kj::mv(request));
  }
}

KJ_TEST("unobserved scopes retain identity and materialize their no-op span lazily") {
  for (bool hasTracer: {false, true}) {
    TestFixture fixture;
    auto context = fixture.newIoContext();
    kj::Maybe<kj::Rc<BaseTracer>> tracer;
    if (hasTracer) tracer = kj::Rc<BaseTracer>(makeTracer());
    auto request = fixture.newUndeliveredIncomingRequest(*context, kj::mv(tracer));
    request->delivered();
    fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
      auto scope = context->makeRequestTraceScope(env.lock);
      auto& frame = KJ_ASSERT_NONNULL(jsg::AsyncContextFrame::current(env.js));
      auto internalValue =
          KJ_ASSERT_NONNULL(frame.get(*env.lock.getTraceAsyncContextKey())).getHandle(env.js);
      auto& internalState =
          jsg::unwrapOpaqueRef<IoContext::TraceScopeState>(env.isolate, internalValue);
      KJ_EXPECT(internalState.is<IoContext::Id>());
      KJ_EXPECT(internalState.get<IoContext::Id>() == context->getId());
      auto userValue =
          KJ_ASSERT_NONNULL(frame.get(*env.lock.getUserTraceAsyncContextKey())).getHandle(env.js);
      auto& userState =
          jsg::unwrapOpaqueRef<IoContext::UserTraceScopeState>(env.isolate, userValue);
      KJ_EXPECT(userState.is<IoContext::Id>());
      KJ_EXPECT(userState.get<IoContext::Id>() == context->getId());
      KJ_EXPECT(!context->getCurrentTraceSpan().isObserved());
      KJ_EXPECT(!context->getCurrentUserTraceSpan().isObserved());

      auto holder = userValue.As<v8::Object>();
      auto cacheKey = v8::Private::ForApi(env.isolate, env.js.strIntern("workerd.activeSpan"_kjc));
      KJ_EXPECT(!jsg::check(holder->HasPrivate(env.js.v8Context(), cacheKey)));
      auto first = activeSpan(env);
      KJ_EXPECT(!first->getIsTraced());
      first->updateName(env.js, kj::str("unused"));
      first->end();
      KJ_EXPECT(!first->getIsTraced());
      KJ_EXPECT(jsg::check(holder->HasPrivate(env.js.v8Context(), cacheKey)));
      auto second = activeSpan(env);
      KJ_EXPECT(first.get() == second.get());
      {
        auto nested = context->makeUserAsyncTraceScope(env.lock, SpanParent(nullptr));
        auto other = activeSpan(env);
        KJ_EXPECT(other.get() != first.get());
        KJ_EXPECT(!other->getIsTraced());
      }
      KJ_EXPECT(activeSpan(env).get() == first.get());
    });
    fixture.drainAndDestroy(kj::mv(request));
  }
}

KJ_TEST("observed and unobserved user scopes restore their cached invocation spans") {
  TestFixture fixture;
  auto context = fixture.newIoContext();
  auto request = fixture.newUndeliveredIncomingRequest(*context, makeObservedTracer());
  request->delivered();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    auto scope = context->makeRequestTraceScope(env.lock);
    auto root = activeSpan(env);
    KJ_EXPECT(root->getIsTraced());
    {
      auto nested = context->makeUserAsyncTraceScope(env.lock, SpanParent(nullptr));
      auto unobserved = activeSpan(env);
      KJ_EXPECT(!unobserved->getIsTraced());
      KJ_EXPECT(unobserved.get() != root.get());
    }
    KJ_EXPECT(root->getIsTraced());
    KJ_EXPECT(activeSpan(env).get() == root.get());
  });
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST(
    "foreign unobserved frames fall back to the current request and reject active-span access") {
  for (bool destroySource: {false, true}) {
    for (bool cacheSpan: {false, true}) {
      TestFixture fixture({
        .useRealTimers = false,
        .requestObserverFactory = kj::Function<kj::Own<RequestObserver>()>(
            []() -> kj::Own<RequestObserver> { return kj::refcounted<ObservedRequest>(); }),
      });
      auto sourceContext = fixture.newIoContext();
      auto source = fixture.newIncomingRequest(*sourceContext);
      kj::Maybe<jsg::Ref<jsg::AsyncContextFrame>> captured;
      kj::Maybe<jsg::Ref<api::user_tracing::Span>> cached;
      fixture.enterContext(*source, [&](const TestFixture::Environment& env) {
        auto internalScope = sourceContext->makeAsyncTraceScope(env.lock, SpanParent(nullptr));
        auto userScope = sourceContext->makeUserAsyncTraceScope(env.lock, SpanParent(nullptr));
        KJ_EXPECT(!sourceContext->getCurrentTraceSpan().isObserved());
        KJ_EXPECT(!sourceContext->getCurrentUserTraceSpan().isObserved());
        captured = jsg::AsyncContextFrame::currentRef(env.js);
        if (cacheSpan) cached = activeSpan(env);
      });
      if (destroySource) {
        fixture.drainAndDestroy(kj::mv(source));
        sourceContext = nullptr;
      }

      auto targetContext = fixture.newIoContext();
      auto target = fixture.newUndeliveredIncomingRequest(*targetContext, makeObservedTracer());
      target->delivered();
      fixture.enterContext(*target, [&](const TestFixture::Environment& env) {
        jsg::AsyncContextFrame::Scope scope(env.js, captured);
        KJ_EXPECT(spanId(targetContext->getCurrentTraceSpan()) == 1);
        KJ_EXPECT(spanId(targetContext->getCurrentUserTraceSpan()) == 2);
        if (cacheSpan) {
          auto span = activeSpan(env);
          KJ_EXPECT(span.get() == KJ_ASSERT_NONNULL(cached).get());
          KJ_EXPECT_THROW_MESSAGE(
              "Cannot perform I/O on behalf of a different request", span->getIsTraced());
        } else {
          KJ_EXPECT_THROW_MESSAGE(
              "Cannot perform I/O on behalf of a different request", activeSpan(env));
        }
      });
      fixture.drainAndDestroy(kj::mv(target));
      if (source.get() != nullptr) fixture.drainAndDestroy(kj::mv(source));
    }
  }
}

KJ_TEST("an unobserved actor continuation masks a newer request's observed spans") {
  TestFixture fixture({
    .actorId = Worker::Actor::Id(kj::str("trace-scopes")),
    .useRealTimers = false,
    .requestObserverFactory =
        kj::Function<kj::Own<RequestObserver>()>([count = 0]() mutable -> kj::Own<RequestObserver> {
    if (count++ == 0) return kj::refcounted<RequestObserver>();
    return kj::refcounted<ObservedRequest>();
  }),
  });
  auto context = fixture.newIoContext();
  auto first = fixture.newIncomingRequest(*context);
  kj::Maybe<jsg::Ref<jsg::AsyncContextFrame>> captured;
  fixture.enterContext(*first, [&](const TestFixture::Environment& env) {
    auto scope = context->makeRequestTraceScope(env.lock);
    captured = jsg::AsyncContextFrame::currentRef(env.js);
  });
  auto second = fixture.newUndeliveredIncomingRequest(*context, makeObservedTracer());
  second->delivered();
  fixture.drainAndDestroy(kj::mv(first));
  fixture.enterContext(*second, [&](const TestFixture::Environment& env) {
    KJ_EXPECT(spanId(context->getCurrentTraceSpan()) == 1);
    KJ_EXPECT(spanId(context->getCurrentUserTraceSpan()) == 2);
    jsg::AsyncContextFrame::Scope scope(env.js, captured);
    KJ_EXPECT(!context->getCurrentTraceSpan().isObserved());
    KJ_EXPECT(!context->getCurrentUserTraceSpan().isObserved());
    KJ_EXPECT(!activeSpan(env)->getIsTraced());
  });
  fixture.drainAndDestroy(kj::mv(second));
}

KJ_TEST("foreign span event identities retain the owning invocation") {
  capnp::MallocMessageBuilder message;
  auto features = message.initRoot<CompatibilityFlags>();
  features.setOpentelemetryApi(true);
  TestFixture fixture({.featureFlags = features.asReader()});
  auto context = fixture.newIoContext();
  auto request = fixture.newUndeliveredIncomingRequest(*context, makeObservedTracer());
  request->delivered();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    auto scope = context->makeRequestTraceScope(env.lock);
    auto invocation = request->getInvocationSpanContext().clone();
    auto foreign = SpanParent::fromSpanContext(
        tracing::SpanContext(tracing::TraceId(3, 4), tracing::SpanId(55), tracing::TraceFlags(1)));
    auto nested = context->makeUserAsyncTraceScope(env.lock, foreign.addRef());
    auto event = context->getInvocationSpanContext();
    KJ_EXPECT(event.getTraceId() == tracing::TraceId(3, 4));
    KJ_EXPECT(event.getSpanId() == tracing::SpanId(55));
    KJ_EXPECT(event.getInvocationId() == invocation.getInvocationId());
    auto direct = request->getInvocationSpanContextForUserSpan(foreign);
    KJ_EXPECT(direct.getTraceId() == event.getTraceId());
    KJ_EXPECT(direct.getSpanId() == event.getSpanId());
  });
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("unsampled roots remain native carriers with either flag state") {
  for (bool enabled: {false, true}) {
    capnp::MallocMessageBuilder message;
    auto features = message.initRoot<CompatibilityFlags>();
    features.setOpentelemetryApi(enabled);
    for (uint8_t flags: {0, 1, 3}) {
      TestFixture fixture({.featureFlags = features.asReader()});
      auto context = fixture.newIoContext();
      auto tracer = makeTracer();
      int parents = 0;
      tracer->setMakeUserSpanParentFunc([&](tracing::SpanContext identity, DetachedRoot) {
        ++parents;
        return SpanParent::fromSpanContext(kj::mv(identity));
      });
      auto request = fixture.newUndeliveredIncomingRequest(*context, kj::mv(tracer));
      auto& invocation = request->getInvocationSpanContext();
      KJ_EXPECT(invocation.getTraceFlags() == kj::none);
      invocation = tracing::InvocationSpanContext(invocation.getTraceId(),
          invocation.getInvocationId(), invocation.getSpanId(), tracing::TraceFlags(flags));
      request->delivered();
      fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
        auto scope = context->makeRequestTraceScope(env.lock);
        KJ_EXPECT(activeSpan(env)->getIsTraced() == (!enabled || flags != 0));
        KJ_EXPECT(activeSpan(env)->spanContext().traceFlags == flags);
        KJ_EXPECT(context->getCurrentUserTraceSpan().isObserved());
        KJ_EXPECT(
            KJ_ASSERT_NONNULL(context->getInvocationSpanContext().getTraceFlags()).isSampled() ==
            ((flags & 1) == 1));
        auto carrier = KJ_ASSERT_NONNULL(context->getCurrentUserTraceSpan().toSpanContext());
        KJ_EXPECT(KJ_ASSERT_NONNULL(carrier.getTraceFlags()) == tracing::TraceFlags(flags));
        auto primitive = env.js.alloc<api::TracingContext>();
        KJ_EXPECT(primitive->getSpanCreationMode(env.js) ==
            (flags == 0 ? "unsampled"_kj : "recording"_kj));
        if (enabled && flags == 0) {
          auto script = jsg::check(v8::Script::Compile(env.js.v8Context(), env.js.str(R"JS(
            const api = globalThis[Symbol.for('opentelemetry.js.api.1')];
            const tracer = api.trace.getTracer('unsampled-root');
            const invocation = api.context.active().getValue(
              Symbol.for('OpenTelemetry Context Key SPAN')).spanContext();
            const child = tracer.startSpan('child');
            const root = tracer.startSpan('root', { root: true });
            const detached = tracer.startSpan('detached', {}, api.context.active().deleteValue(
              Symbol.for('OpenTelemetry Context Key SPAN')));
            for (const span of [child, root, detached]) {
              const sc = span.spanContext();
              if (span.isRecording() || sc.traceFlags !== 0 ||
                  (sc.traceId === invocation.traceId) !== (span === child) ||
                  sc.traceId === '00000000000000000000000000000000' ||
                  sc.spanId === invocation.spanId ||
                  sc.spanId === '0000000000000000') throw new Error('Invalid unsampled child');
              span.end();
            }
            if (root.spanContext().traceId === detached.spanContext().traceId)
              throw new Error('Detached roots reused a trace ID');
          )JS"_kj)));
          jsg::check(script->Run(env.js.v8Context()));
        }
      });
      // Explicit 00 uses a propagation-only root without allocating a discarded recording one.
      KJ_EXPECT(parents == (enabled && flags == 0 ? 0 : 1));
      fixture.drainAndDestroy(kj::mv(request));
    }
  }
}

KJ_TEST("tracer presence never invents a local sampling decision") {
  for (bool enabled: {false, true}) {
    for (bool hasTracer: {false, true}) {
      capnp::MallocMessageBuilder message;
      auto features = message.initRoot<CompatibilityFlags>();
      features.setOpentelemetryApi(enabled);
      TestFixture fixture({.featureFlags = features.asReader()});
      auto context = fixture.newIoContext();
      auto request = fixture.newUndeliveredIncomingRequest(
          *context, hasTracer ? kj::Maybe<kj::Rc<BaseTracer>>(makeObservedTracer()) : kj::none);
      request->delivered();
      auto flags = request->getInvocationSpanContext().getTraceFlags();
      KJ_EXPECT(flags == kj::none);
      fixture.drainAndDestroy(kj::mv(request));
    }
  }
}

KJ_TEST("overlapping actor frames retain their tracer through explicit-parent activation") {
  capnp::MallocMessageBuilder message;
  auto features = message.initRoot<CompatibilityFlags>();
  features.setOpentelemetryApi(true);
  TestFixture fixture({.featureFlags = features.asReader(),
    .actorId = Worker::Actor::Id(kj::str("otel-frame-owner"))});
  auto context = fixture.newIoContext();
  auto firstTracer = makeObservedTracer();
  auto first = fixture.newUndeliveredIncomingRequest(*context, firstTracer.clone());
  first->delivered();
  kj::Maybe<jsg::Ref<jsg::AsyncContextFrame>> captured;
  fixture.enterContext(*first, [&](const TestFixture::Environment& env) {
    auto scope = context->makeRequestTraceScope(env.lock);
    captured = jsg::AsyncContextFrame::currentRef(env.js);
  });
  auto secondTracer = makeObservedTracer();
  auto second = fixture.newUndeliveredIncomingRequest(*context, secondTracer.clone());
  second->delivered();
  fixture.enterContext(*second, [&](const TestFixture::Environment& env) {
    auto current = context->makeRequestTraceScope(env.lock);
    KJ_EXPECT(&KJ_ASSERT_NONNULL(context->getCurrentUserTracer()) == secondTracer.get());
    jsg::AsyncContextFrame::Scope restored(env.js, captured);
    KJ_EXPECT(&KJ_ASSERT_NONNULL(context->getCurrentUserTracer()) == firstTracer.get());
    auto tracingContext = env.js.alloc<api::TracingContext>();
    tracingContext->withActiveSpanContext(env.js,
        api::user_tracing::TracingSpanContext{
          kj::str("11111111111111111111111111111111"), kj::str("2222222222222222"), 1},
        jsg::Function<jsg::JsRef<jsg::JsValue>()>([&](jsg::Lock& js) {
      KJ_EXPECT(&KJ_ASSERT_NONNULL(context->getCurrentUserTracer()) == firstTracer.get());
      auto owner =
          KJ_ASSERT_NONNULL(context->getCurrentUserTraceAsyncContext()).getInvocationSpanContext();
      KJ_EXPECT(KJ_ASSERT_NONNULL(owner).getInvocationId() ==
          first->getInvocationSpanContext().getInvocationId());
      return jsg::JsRef(js, js.undefined());
    }));
  });
  fixture.drainAndDestroy(kj::mv(first));
  fixture.drainAndDestroy(kj::mv(second));
}

KJ_TEST("expired traced owners keep their invocation identity rather than their trigger") {
  capnp::MallocMessageBuilder message;
  auto features = message.initRoot<CompatibilityFlags>();
  features.setOpentelemetryApi(true);
  TestFixture fixture({.featureFlags = features.asReader(),
    .actorId = Worker::Actor::Id(kj::str("expired-trace-identity"))});
  auto context = fixture.newIoContext();
  auto trigger = tracing::InvocationSpanContext(tracing::TraceId(1, 2), tracing::TraceId::nullId,
      tracing::SpanId(42), tracing::TraceFlags(1));
  auto first =
      fixture.newUndeliveredIncomingRequest(*context, makeObservedTracer(), trigger.clone());
  first->delivered();
  auto expected = first->getInvocationSpanContext().getSpanId();
  KJ_EXPECT(expected != trigger.getSpanId());
  kj::Maybe<jsg::Ref<jsg::AsyncContextFrame>> captured;
  fixture.enterContext(*first, [&](const TestFixture::Environment& env) {
    auto scope = context->makeRequestTraceScope(env.lock);
    captured = jsg::AsyncContextFrame::currentRef(env.js);
  });
  auto second = fixture.newIncomingRequest(*context);
  fixture.drainAndDestroy(kj::mv(first));
  fixture.enterContext(*second, [&](const TestFixture::Environment& env) {
    jsg::AsyncContextFrame::Scope restored(env.js, captured);
    KJ_EXPECT(context->getWorkerTracer() == kj::none);
    auto identity = KJ_ASSERT_NONNULL(context->getUserTraceInvocationIdentity());
    KJ_EXPECT(identity.getSpanId() == expected);
    auto primitive = env.js.alloc<api::TracingContext>();
    KJ_EXPECT(KJ_ASSERT_NONNULL(primitive->getInvocationSpanContext(env.js)).spanId ==
        expected.toGoString());
  });
  fixture.drainAndDestroy(kj::mv(second));
}

KJ_TEST("owner-less frames inherit the current tracer with either flag state") {
  for (bool enabled: {false, true}) {
    capnp::MallocMessageBuilder message;
    auto features = message.initRoot<CompatibilityFlags>();
    features.setOpentelemetryApi(enabled);
    TestFixture fixture({.featureFlags = features.asReader()});
    auto context = fixture.newIoContext();
    auto tracer = makeObservedTracer();
    auto request = fixture.newUndeliveredIncomingRequest(*context, tracer.clone());
    request->delivered();
    fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
      jsg::AsyncContextFrame::Scope fresh(env.js, kj::none);
      auto scope = context->makeUserAsyncTraceScope(env.lock, observedParent(8));
      KJ_EXPECT(&KJ_ASSERT_NONNULL(context->getWorkerTracer()) == tracer.get());
      KJ_EXPECT(activeSpan(env)->getIsTraced());
      {
        v8::SealHandleScope sealed(env.isolate);
        KJ_EXPECT(context->hasWarningHandler());
        context->logWarning("trace scope warning");
      }
    });
    fixture.drainAndDestroy(kj::mv(request));
  }
}

KJ_TEST("expired actor tracers use the legacy fallback only with the flag off") {
  for (bool enabled: {false, true}) {
    capnp::MallocMessageBuilder message;
    auto features = message.initRoot<CompatibilityFlags>();
    features.setOpentelemetryApi(enabled);
    TestFixture fixture({.featureFlags = features.asReader(),
      .actorId = Worker::Actor::Id(kj::str("expired-owner"))});
    auto context = fixture.newIoContext();
    auto first = fixture.newUndeliveredIncomingRequest(*context, makeObservedTracer());
    first->delivered();
    kj::Maybe<jsg::Ref<jsg::AsyncContextFrame>> captured;
    fixture.enterContext(*first, [&](const TestFixture::Environment& env) {
      auto scope = context->makeRequestTraceScope(env.lock);
      captured = jsg::AsyncContextFrame::currentRef(env.js);
    });
    auto secondTracer = makeObservedTracer();
    auto second = fixture.newUndeliveredIncomingRequest(*context, secondTracer.clone());
    second->delivered();
    fixture.drainAndDestroy(kj::mv(first));
    fixture.enterContext(*second, [&](const TestFixture::Environment& env) {
      jsg::AsyncContextFrame::Scope restored(env.js, captured);
      auto sink = context->getWorkerTracer();
      if (enabled) {
        KJ_EXPECT(sink == kj::none);
      } else {
        KJ_EXPECT(&KJ_ASSERT_NONNULL(sink) == secondTracer.get());
      }
    });
    fixture.drainAndDestroy(kj::mv(second));
  }
}

}  // namespace
}  // namespace workerd
