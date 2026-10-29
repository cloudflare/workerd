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

kj::Rc<BaseTracer> makeObservedTracer() {
  auto tracer = makeTracer();
  tracer->setMakeUserRequestSpanFunc([](tracing::SpanContext) { return observedParent(2); });
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

}  // namespace
}  // namespace workerd
