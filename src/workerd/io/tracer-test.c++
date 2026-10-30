// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/io/otlp-span-submitter.h>
#include <workerd/io/timer-test-fixture.h>
#include <workerd/io/tracer.h>
#include <workerd/tests/test-fixture.h>

#include <kj/test.h>

namespace workerd {
namespace {

class RecordingSpanSubmitter final: public SpanSubmitter {
 public:
  kj::Vector<kj::Date> starts;
  kj::Vector<kj::Date> exceptions;

  bool submitSpanOpen(tracing::SpanId spanId,
      tracing::SpanId parentSpanId,
      kj::ConstString operationName,
      kj::Date startTime) override {
    starts.add(startTime);
    return true;
  }

  void submitSpanClose(
      tracing::SpanId spanId, kj::Date startTime, kj::Date endTime, Span::TagMap&& tags) override {}
  void submitSpanUpdate(tracing::SpanId spanId, tracing::SpanUpdate&& update) override {}
  void submitSpanException(tracing::SpanId spanId,
      kj::Date timestamp,
      kj::Maybe<tracing::Exception::Code> code,
      kj::String name,
      kj::String message,
      kj::Maybe<kj::String> stack) override {
    exceptions.add(timestamp);
  }
  tracing::SpanId makeSpanId() override {
    return tracing::SpanId(nextId++);
  }

 private:
  uint64_t nextId = 1;
};

class TestEntropySource final: public kj::EntropySource {
 public:
  void generate(kj::ArrayPtr<kj::byte> buffer) override {
    for (auto& byte: buffer) byte = next++;
  }

 private:
  kj::byte next = 1;
};

class RecordingExporter final: public OtlpSpanExporter {
 public:
  uint spanCount = 0;

  void exportSpans(kj::Array<const kj::byte> request, uint count) override {
    spanCount += count;
  }
};

void expectPreciseTime(SpanObserver& observer) {
  auto before = kj::systemPreciseCalendarClock().now();
  auto time = observer.getTime();
  auto after = kj::systemPreciseCalendarClock().now();
  KJ_EXPECT(time >= before && time <= after);
}

void expectNativeClocks() {
  TestEntropySource entropy;
  auto exporter = kj::rc<RecordingExporter>();
  {
    auto root = startExportedUserSpan(entropy, exporter.addRef(),
        tracing::SpanContext(tracing::TraceId(1, 2), tracing::SpanId(3)),
        {.serviceName = "clock-test"}, "root"_kjc);
    expectPreciseTime(KJ_ASSERT_NONNULL(root.getObserver()));
    auto child = root.newChild("runtime"_kjc);
    expectPreciseTime(KJ_ASSERT_NONNULL(child.getObserver()));
    auto user =
        SpanBuilder(KJ_ASSERT_NONNULL(child.getObserver()).newChildFromUserCode(), "user"_kjc);
    expectPreciseTime(KJ_ASSERT_NONNULL(user.getObserver()));
    auto nested = user.newChild("nested"_kjc);
    expectPreciseTime(KJ_ASSERT_NONNULL(nested.getObserver()));
    user.recordException(kj::none, kj::str("Error"), kj::str("test"), kj::none);
  }
  KJ_EXPECT(exporter->spanCount == 4);
}

KJ_TEST("streaming spans use I/O time while native spans use precise time") {
  test::TestTimerChannel timer(test::TimerClock::CLAMPED);
  timer.advanceTo(123);
  TestFixture fixture({.ioChannelFactory = kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)>(
                           [&](TimerChannel&) -> kj::Rc<IoChannelFactory> {
    return kj::rc<TestFixture::DummyIoChannelFactory>(timer);
  })});
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    KJ_ASSERT(!env.features.getPreciseTimers());
    auto time = kj::UNIX_EPOCH + 123 * kj::MILLISECONDS;
    KJ_ASSERT(env.context.now() == time);
    auto submitter = kj::refcounted<RecordingSpanSubmitter>();
    auto observer = kj::rc<UserSpanObserver>(kj::addRef(*submitter));
    KJ_EXPECT(observer->getTime() == time);
    auto runtime = SpanBuilder(observer->newChild(), "runtime"_kjc);
    auto user =
        SpanBuilder(KJ_ASSERT_NONNULL(runtime.getObserver()).newChildFromUserCode(), "user"_kjc);
    auto nested = user.newChild("nested"_kjc);
    user.recordException(kj::none, kj::str("Error"), kj::str("test"), kj::none);
    KJ_ASSERT(submitter->starts.size() == 3);
    for (auto start: submitter->starts) KJ_EXPECT(start == time);
    KJ_ASSERT(submitter->exceptions.size() == 1);
    KJ_EXPECT(submitter->exceptions[0] == time);

    expectNativeClocks();
  });
}

KJ_TEST("native span clocks work without an IoContext") {
  expectNativeClocks();
}

}  // namespace
}  // namespace workerd
