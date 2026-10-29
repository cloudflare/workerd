// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/io/trace-stream.h>
#include <workerd/io/tracer.h>
#include <workerd/util/thread-scopes.h>

#include <kj/test.h>

namespace workerd {
namespace {

kj::Rc<WorkerTracer> makeTracer() {
  auto trace = kj::refcounted<Trace>(kj::none, kj::none, kj::none, kj::none, kj::none,
      kj::Array<kj::String>(), kj::none, ExecutionModel::STATELESS);
  auto tracer =
      kj::rc<WorkerTracer>(kj::none, kj::mv(trace), PipelineLogLevel::NONE, kj::none, kj::none);
  tracer->markUnused();
  tracer->setMakeUserSpanParentFunc([](tracing::SpanContext identity, DetachedRoot) {
    return SpanParent::fromSpanContext(kj::mv(identity));
  });
  return tracer;
}

// Isolated executable: predictable mode is process-global and cannot be turned off.
KJ_TEST("predictable roots use independent deterministic keys without shifting trace IDs") {
  setPredictableModeForTest();
  class NoEntropy final: public kj::EntropySource {
    void generate(kj::ArrayPtr<kj::byte>) override {
      KJ_FAIL_ASSERT("unexpected entropy");
    }
  } entropy;
  // Deliberately consume keys and IDs first; assertions must not assume either starts at zero.
  makeTracer()->makeDetachedTraceId(entropy);
  auto before = tracing::TraceId::fromEntropy().getHigh() ^ tracing::staticSpanId;
  auto first = makeTracer();
  auto second = makeTracer();
  auto a = first->makeDetachedTraceId(entropy);
  auto b = first->makeDetachedTraceId(entropy);
  auto c = second->makeDetachedTraceId(entropy);
  KJ_EXPECT(a.getHigh() == (tracing::staticSpanId ^ (before + 1)));
  KJ_EXPECT(b.getHigh() == (tracing::staticSpanId ^ (before + 2)));
  KJ_EXPECT(c.getHigh() == (tracing::staticSpanId ^ (before + 3)));
  KJ_EXPECT(tracing::TraceId::fromEntropy().getHigh() == (tracing::staticSpanId ^ (before + 4)));
  KJ_EXPECT(first->isDetachedTraceId(a));
  KJ_EXPECT(first->isDetachedTraceId(b));
  KJ_EXPECT(second->isDetachedTraceId(c));
  KJ_EXPECT(!second->isDetachedTraceId(a));
  KJ_EXPECT(!first->isDetachedTraceId(c));
}

}  // namespace
}  // namespace workerd
