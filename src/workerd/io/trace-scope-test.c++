// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/tracing.h>
#include <workerd/io/io-context.h>
#include <workerd/io/trace-stream.h>
#include <workerd/tests/test-fixture.h>

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

class NullSpanSubmitter final: public SpanSubmitter {
 public:
  bool submitSpanOpen(tracing::TraceId,
      kj::Maybe<tracing::TraceFlags>,
      tracing::SpanId,
      tracing::SpanId,
      kj::ConstString,
      kj::Date) override {
    return true;
  }
  void submitSpanClose(tracing::SpanId, kj::Date, kj::Date, Span::TagMap&&) override {}
  void submitSpanUpdate(tracing::SpanId, tracing::SpanUpdate&&) override {}
  void submitSpanException(tracing::SpanId,
      kj::Date,
      kj::Maybe<tracing::Exception::Code>,
      kj::String,
      kj::String,
      kj::Maybe<kj::String>) override {}
  tracing::SpanId makeSpanId() override {
    return tracing::SpanId(1);
  }
  tracing::TraceId makeRootTraceId() override {
    return tracing::TraceId(1, 2);
  }
};

kj::Rc<BaseTracer> makeObservedTracer() {
  auto tracer = makeTracer();
  tracer->setUserSpanSubmitter(kj::refcounted<NullSpanSubmitter>());
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

KJ_TEST("an unobserved stateless request builds no frame and makes its no-op span once") {
  for (bool hasTracer: {false, true}) {
    TestFixture fixture;
    kj::Maybe<jsg::Ref<api::user_tracing::Span>> firstRequestSpan;
    auto firstContext = fixture.newIoContext();
    auto runRequest = [&](IoContext& context) {
      kj::Maybe<kj::Rc<BaseTracer>> tracer;
      if (hasTracer) tracer = kj::Rc<BaseTracer>(makeTracer());
      auto request = fixture.newUndeliveredIncomingRequest(context, kj::mv(tracer));
      request->delivered();
      fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
        auto scope = context.makeRequestTraceScope(env.lock);
        KJ_EXPECT(jsg::AsyncContextFrame::current(env.js) == kj::none);
        KJ_EXPECT(!context.getCurrentTraceSpan().isObserved());
        KJ_EXPECT(!context.getCurrentUserTraceSpan().isObserved());

        auto first = activeSpan(env);
        KJ_EXPECT(!first->getIsTraced());
        first->updateName(env.js, kj::str("unused"));
        first->end();
        KJ_EXPECT(!first->getIsTraced());
        KJ_EXPECT(activeSpan(env).get() == first.get());
        {
          auto nested = context.makeUserAsyncTraceScope(env.lock, SpanParent(nullptr));
          auto other = activeSpan(env);
          KJ_EXPECT(other.get() != first.get());
          KJ_EXPECT(!other->getIsTraced());
        }
        KJ_EXPECT(activeSpan(env).get() == first.get());

        KJ_IF_SOME(previous, firstRequestSpan) {
          KJ_EXPECT(first.get() != previous.get());
        } else {
          firstRequestSpan = kj::mv(first);
        }
      });
      fixture.drainAndDestroy(kj::mv(request));
    };
    runRequest(*firstContext);
    auto secondContext = fixture.newIoContext();
    runRequest(*secondContext);
  }
}

KJ_TEST("a frame made in an unobserved request resolves to the spans of the request running it") {
  TestFixture fixture({
    .useRealTimers = false,
    .requestObserverFactory =
        kj::Function<kj::Own<RequestObserver>()>([count = 0]() mutable -> kj::Own<RequestObserver> {
    if (count++ == 0) return kj::refcounted<RequestObserver>();
    return kj::refcounted<ObservedRequest>();
  }),
  });
  auto sourceContext = fixture.newIoContext();
  auto source = fixture.newIncomingRequest(*sourceContext);
  kj::Maybe<jsg::Ref<jsg::AsyncContextFrame>> captured;
  fixture.enterContext(*source, [&](const TestFixture::Environment& env) {
    auto scope = sourceContext->makeRequestTraceScope(env.lock);
    // As AsyncLocalStorage.run() would. The frame has no trace entries.
    auto key = kj::arc<jsg::AsyncContextFrame::StorageKey>();
    jsg::AsyncContextFrame::StorageScope storageScope(
        env.js, kj::mv(key), env.js.v8Ref(v8::Integer::New(env.isolate, 55).As<v8::Value>()));
    captured = jsg::AsyncContextFrame::currentRef(env.js);
  });
  fixture.drainAndDestroy(kj::mv(source));

  auto targetContext = fixture.newIoContext();
  auto target = fixture.newUndeliveredIncomingRequest(*targetContext, makeObservedTracer());
  target->delivered();
  fixture.enterContext(*target, [&](const TestFixture::Environment& env) {
    jsg::AsyncContextFrame::Scope scope(env.js, captured);
    KJ_EXPECT(spanId(targetContext->getCurrentTraceSpan()) == 1);
    KJ_EXPECT(spanId(targetContext->getCurrentUserTraceSpan()) == 2);
    auto span = activeSpan(env);
    KJ_EXPECT(span->getIsTraced());
    KJ_EXPECT(activeSpan(env).get() == span.get());
  });
  fixture.drainAndDestroy(kj::mv(target));
}

KJ_TEST("an observed stateless request builds its own request frame") {
  TestFixture fixture({
    .useRealTimers = false,
    .requestObserverFactory = kj::Function<kj::Own<RequestObserver>()>(
        []() -> kj::Own<RequestObserver> { return kj::refcounted<ObservedRequest>(); }),
  });
  auto context = fixture.newIoContext();
  auto request = fixture.newIncomingRequest(*context);
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    auto scope = context->makeRequestTraceScope(env.lock);
    KJ_EXPECT(jsg::AsyncContextFrame::current(env.js) != kj::none);
    KJ_EXPECT(spanId(context->getCurrentTraceSpan()) == 1);
  });
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("code outside a traced request's frame gets the request's active span") {
  TestFixture fixture({.useRealTimers = false});
  auto context = fixture.newIoContext();
  auto request = fixture.newUndeliveredIncomingRequest(*context, makeObservedTracer());
  request->delivered();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    auto scope = context->makeRequestTraceScope(env.lock);
    auto span = activeSpan(env);
    KJ_EXPECT(span->getIsTraced());
    // As a function bound outside every request would run.
    jsg::AsyncContextFrame::Scope rootScope(env.js, kj::none);
    KJ_EXPECT(activeSpan(env).get() == span.get());
  });
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("an unobserved request inside an existing frame builds a request frame") {
  TestFixture fixture;
  auto unobservedContext = fixture.newIoContext();
  auto unobservedRequest = fixture.newIncomingRequest(*unobservedContext);
  fixture.enterContext(*unobservedRequest, [&](const TestFixture::Environment& env) {
    auto key = kj::arc<jsg::AsyncContextFrame::StorageKey>();
    jsg::AsyncContextFrame::StorageScope storageScope(
        env.js, key.addRef(), env.js.v8Ref(v8::Integer::New(env.isolate, 55).As<v8::Value>()));
    auto scope = unobservedContext->makeRequestTraceScope(env.lock);
    auto& frame = KJ_ASSERT_NONNULL(jsg::AsyncContextFrame::current(env.js));
    KJ_EXPECT(KJ_ASSERT_NONNULL(frame.get(*key)).getHandle(env.js).As<v8::Int32>()->Value() == 55);
    auto userValue =
        KJ_ASSERT_NONNULL(frame.get(*env.lock.getUserTraceAsyncContextKey())).getHandle(env.js);
    auto& userState = jsg::unwrapOpaqueRef<IoContext::UserTraceScopeState>(env.isolate, userValue);
    KJ_EXPECT(userState.get<IoContext::Id>() == unobservedContext->getId());
    KJ_EXPECT(!unobservedContext->getCurrentTraceSpan().isObserved());
  });
  fixture.drainAndDestroy(kj::mv(unobservedRequest));
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
      auto targetUserSpanId = target->getInvocationSpanContext().getSpanId().getId();
      fixture.enterContext(*target, [&](const TestFixture::Environment& env) {
        jsg::AsyncContextFrame::Scope scope(env.js, captured);
        KJ_EXPECT(spanId(targetContext->getCurrentTraceSpan()) == 1);
        KJ_EXPECT(spanId(targetContext->getCurrentUserTraceSpan()) == targetUserSpanId);
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
    KJ_EXPECT(jsg::AsyncContextFrame::current(env.js) != kj::none);
    captured = jsg::AsyncContextFrame::currentRef(env.js);
  });
  auto second = fixture.newUndeliveredIncomingRequest(*context, makeObservedTracer());
  second->delivered();
  auto secondUserSpanId = second->getInvocationSpanContext().getSpanId().getId();
  fixture.drainAndDestroy(kj::mv(first));
  fixture.enterContext(*second, [&](const TestFixture::Environment& env) {
    KJ_EXPECT(spanId(context->getCurrentTraceSpan()) == 1);
    KJ_EXPECT(spanId(context->getCurrentUserTraceSpan()) == secondUserSpanId);
    jsg::AsyncContextFrame::Scope scope(env.js, captured);
    KJ_EXPECT(!context->getCurrentTraceSpan().isObserved());
    KJ_EXPECT(!context->getCurrentUserTraceSpan().isObserved());
    KJ_EXPECT(!activeSpan(env)->getIsTraced());
  });
  fixture.drainAndDestroy(kj::mv(second));
}

}  // namespace
}  // namespace workerd
