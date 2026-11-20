// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "trace-stream.h"

#include <workerd/io/tracer.h>

#include <capnp/compat/byte-stream.h>
#include <capnp/compat/http-over-capnp.h>
#include <kj/test.h>

namespace workerd::tracing {
namespace {

// Counts the bytes that sessions hold, refusing to hold more than `limit`.
class FakeBudget final: public TailStreamBudget {
 public:
  explicit FakeBudget(size_t limit): limit(limit) {}

  size_t held = 0;
  size_t limit;

  bool tryReserve(size_t bytes) override {
    if (held + bytes > limit) return false;
    held += bytes;
    return true;
  }
  void reserve(size_t bytes) override {
    held += bytes;
  }
  void release(size_t bytes) override {
    KJ_ASSERT(bytes <= held);
    held -= bytes;
  }
};

// A tail worker that records the events it receives. It acknowledges each batch only when the test
// calls ack(), so that the test can observe the writer while a batch is in flight.
class FakeTailStreamTarget final: public rpc::TailStreamTarget::Server {
 public:
  struct Batch {
    kj::Vector<rpc::Trace::TailEvent::Event::Which> events;
    kj::Maybe<uint32_t> droppedCount;
  };

  // The trace context of an event, in the order received.
  struct Event {
    rpc::Trace::TailEvent::Event::Which which;
    TraceId traceId;
    TraceId invocationId;
    kj::Maybe<uint64_t> spanId;
    kj::Maybe<TraceFlags> traceFlags;
    // For SpanOpen events, the opened span's ID.
    uint64_t openedSpanId = 0;
  };

  kj::Vector<Batch> batches;
  kj::Vector<Event> events;
  bool stopOnOnset = false;

  kj::Promise<void> report(ReportContext context) override {
    Batch batch;
    for (auto event: context.getParams().getEvents()) {
      auto e = event.getEvent();
      batch.events.add(e.which());
      auto spanContext = event.getSpanContext();
      Event recorded{
        .which = e.which(),
        .traceId = TraceId::fromCapnp(spanContext.getTraceId()),
        .invocationId = TraceId::fromCapnp(event.getInvocationId()),
      };
      if (spanContext.getInfo().isSpanId()) {
        recorded.spanId = spanContext.getInfo().getSpanId();
      }
      if (spanContext.hasTraceFlags() && spanContext.getTraceFlags().getValue().isSet()) {
        recorded.traceFlags = TraceFlags(spanContext.getTraceFlags().getValue().getSet());
      }
      if (e.isSpanOpen()) {
        recorded.openedSpanId = e.getSpanOpen().getSpanId();
      }
      events.add(kj::mv(recorded));
      if (e.isStreamDiagnostics()) {
        batch.droppedCount = e.getStreamDiagnostics().getDiagnostic().getDroppedEvents().getCount();
      }
    }
    bool isOnset =
        batch.events.size() == 1 && batch.events[0] == rpc::Trace::TailEvent::Event::ONSET;
    batches.add(kj::mv(batch));
    context.getResults().setStop(isOnset && stopOnOnset);

    auto paf = kj::newPromiseAndFulfiller<void>();
    acks.add(kj::mv(paf.fulfiller));
    return kj::mv(paf.promise);
  }

  // Acknowledges the oldest batch that hasn't been acknowledged.
  void ack() {
    KJ_ASSERT(nextAck < acks.size());
    acks[nextAck++]->fulfill();
  }

  // Receives and acknowledges batches until the writer has nothing more to send.
  void drain(kj::WaitScope& waitScope) {
    for (;;) {
      waitScope.poll();
      if (nextAck == acks.size()) break;
      while (nextAck < acks.size()) ack();
    }
  }

 private:
  kj::Vector<kj::Own<kj::PromiseFulfiller<void>>> acks;
  size_t nextAck = 0;
};

class FakeDispatcher final: public rpc::EventDispatcher::Server {
 public:
  explicit FakeDispatcher(rpc::TailStreamTarget::Client target): target(kj::mv(target)) {}

  kj::Promise<void> tailStreamSession(TailStreamSessionContext context) override {
    context.getResults().setTopLevel(target);
    return kj::READY_NOW;
  }

 private:
  rpc::TailStreamTarget::Client target;
};

// Delivers a tail stream session to `dispatcher` over (local) RPC.
class FakeTailWorker final: public WorkerInterface {
 public:
  FakeTailWorker(rpc::EventDispatcher::Client dispatcher): dispatcher(kj::mv(dispatcher)) {}

  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      Response& response) override {
    KJ_UNIMPLEMENTED("not used");
  }
  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::HttpConnectSettings settings) override {
    KJ_UNIMPLEMENTED("not used");
  }
  kj::Promise<void> prewarm(kj::StringPtr url) override {
    KJ_UNIMPLEMENTED("not used");
  }
  kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override {
    KJ_UNIMPLEMENTED("not used");
  }
  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime, uint32_t retryCount) override {
    KJ_UNIMPLEMENTED("not used");
  }
  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override {
    return event
        ->sendRpc(httpOverCapnpFactory, byteStreamFactory, getUnsupportedFrankenvalueHandler(),
            dispatcher)
        .attach(kj::mv(event));
  }

 private:
  rpc::EventDispatcher::Client dispatcher;
  capnp::ByteStreamFactory byteStreamFactory;
  kj::HttpHeaderTable::Builder headerTableBuilder;
  capnp::HttpOverCapnpFactory httpOverCapnpFactory{byteStreamFactory,
    capnp::HttpOverCapnpFactory::HeaderIdBundle(headerTableBuilder),
    capnp::HttpOverCapnpFactory::LEVEL_2};
  kj::Own<kj::HttpHeaderTable> headerTable = headerTableBuilder.build();
};

struct ErrorHandler final: public kj::TaskSet::ErrorHandler {
  void taskFailed(kj::Exception&& exception) override {
    KJ_FAIL_EXPECT(exception);
  }
};

struct WriterTest {
  kj::EventLoop loop;
  kj::WaitScope waitScope{loop};
  ErrorHandler errorHandler;
  kj::TaskSet waitUntilTasks{errorHandler};
  FakeTailStreamTarget* target;
  // Keeps `target` alive after the session ends.
  kj::Maybe<rpc::TailStreamTarget::Client> targetClient;
  kj::Own<TailStreamWriter> writer;
  InvocationSpanContext context = InvocationSpanContext::newForInvocation();

  explicit WriterTest(kj::Maybe<kj::Rc<TailStreamBudget>> budget = kj::none) {
    auto ownTarget = kj::heap<FakeTailStreamTarget>();
    target = ownTarget.get();
    rpc::TailStreamTarget::Client client = kj::mv(ownTarget);
    targetClient = client;
    rpc::EventDispatcher::Client dispatcher = kj::heap<FakeDispatcher>(kj::mv(client));
    auto workers = kj::heapArray<kj::Own<WorkerInterface>>(1);
    workers[0] = kj::heap<FakeTailWorker>(kj::mv(dispatcher));
    writer = KJ_ASSERT_NONNULL(
        initializeTailStreamWriter(kj::mv(workers), waitUntilTasks, kj::mv(budget)));
  }

  void reportOnset() {
    writer->report(context.toSpanContext(), context.getInvocationId(),
        kj::heap<Onset>(SpanId(1),
            Onset::Info(FetchEventInfo(
                kj::HttpMethod::GET, kj::str("https://example.com"), kj::str("{}"), nullptr)),
            Onset::WorkerInfo{}, nullptr),
        kj::UNIX_EPOCH, 0);
  }

  // Reports a log whose message is reported to own `size` bytes.
  void reportLog(size_t size) {
    writer->report(context.toSpanContext(), context.getInvocationId(),
        Log(kj::UNIX_EPOCH, LogLevel::INFO, kj::str("x")), kj::UNIX_EPOCH, size);
  }

  void reportOutcome() {
    writer->report(context.toSpanContext(), context.getInvocationId(),
        Outcome(EventOutcome::OK, 0 * kj::SECONDS, 0 * kj::SECONDS), kj::UNIX_EPOCH, 0);
  }

  void poll() {
    waitScope.poll();
  }
};

constexpr size_t MIB = 1024 * 1024;

KJ_TEST("TailStreamWriter drops events past a session's memory limit") {
  WriterTest test;
  test.reportOnset();
  // Each of these holds a little over 1 MiB, so three fit in the session's 4 MiB along with the
  // onset, and the fourth doesn't.
  for (auto i KJ_UNUSED: kj::zeroTo(4)) {
    test.reportLog(MIB);
  }
  test.reportOutcome();

  test.poll();
  KJ_ASSERT(test.target->batches.size() == 1);
  test.target->ack();
  test.poll();

  KJ_ASSERT(test.target->batches.size() == 2);
  auto& batch = test.target->batches[1];
  using Event = rpc::Trace::TailEvent::Event;
  auto expected = kj::heapArray<Event::Which>(
      {Event::LOG, Event::LOG, Event::LOG, Event::STREAM_DIAGNOSTICS, Event::OUTCOME});
  KJ_EXPECT(batch.events.asPtr() == expected.asPtr());
  KJ_EXPECT(KJ_ASSERT_NONNULL(batch.droppedCount) == 1);
  test.target->ack();
  test.poll();
}

KJ_TEST("TailStreamWriter holds sent events against its budget until they are acknowledged") {
  auto budget = kj::rc<FakeBudget>(kj::maxValue);
  WriterTest test(budget.addRef());

  test.reportOnset();
  test.reportLog(1000);
  auto queued = budget->held;
  KJ_EXPECT(
      queued == TailStreamWriter::queuedEventSize(0) + TailStreamWriter::queuedEventSize(1000));

  // The onset is sent on its own. Until it is acknowledged, it still counts.
  test.poll();
  KJ_ASSERT(test.target->batches.size() == 1);
  KJ_EXPECT(budget->held == queued);
  test.target->ack();
  test.poll();
  KJ_EXPECT(budget->held == TailStreamWriter::queuedEventSize(1000));

  // Then the log is sent, and counts until it is acknowledged too.
  KJ_ASSERT(test.target->batches.size() == 2);
  test.target->ack();
  test.poll();
  KJ_EXPECT(budget->held == 0);

  test.reportOutcome();
  test.poll();
  test.target->ack();
  test.poll();
  KJ_EXPECT(budget->held == 0);
}

KJ_TEST("TailStreamWriter drops events that its budget refuses, but never the outcome") {
  // Room for the onset and two logs.
  auto budget = kj::rc<FakeBudget>(
      TailStreamWriter::queuedEventSize(0) + 2 * TailStreamWriter::queuedEventSize(1000));
  WriterTest test(budget.addRef());

  test.reportOnset();
  for (auto i KJ_UNUSED: kj::zeroTo(5)) {
    test.reportLog(1000);
  }
  // The outcome and the diagnostics event are held regardless of the budget.
  test.reportOutcome();
  KJ_EXPECT(budget->held > budget->limit);

  test.poll();
  test.target->ack();
  test.poll();

  KJ_ASSERT(test.target->batches.size() == 2);
  auto& batch = test.target->batches[1];
  using Event = rpc::Trace::TailEvent::Event;
  auto expected = kj::heapArray<Event::Which>(
      {Event::LOG, Event::LOG, Event::STREAM_DIAGNOSTICS, Event::OUTCOME});
  KJ_EXPECT(batch.events.asPtr() == expected.asPtr());
  KJ_EXPECT(KJ_ASSERT_NONNULL(batch.droppedCount) == 3);
  test.target->ack();
  test.poll();
  KJ_EXPECT(budget->held == 0);
}

KJ_TEST("TailStreamWriter gives back its budget when the tail worker stops the session") {
  auto budget = kj::rc<FakeBudget>(kj::maxValue);
  WriterTest test(budget.addRef());
  test.target->stopOnOnset = true;

  test.reportOnset();
  test.reportLog(1000);
  test.reportLog(1000);
  test.poll();
  KJ_ASSERT(test.target->batches.size() == 1);
  test.target->ack();
  test.poll();

  KJ_EXPECT(budget->held == 0);
  KJ_EXPECT(test.target->batches.size() == 1);
}

// Forwards spans straight to a WorkerTracer, with predictable span IDs, and records the parent
// span ID each span is opened with.
class TestSpanSubmitter final: public SpanSubmitter {
 public:
  TestSpanSubmitter(kj::Own<BaseTracer::WeakRef> weakTracer, kj::Vector<SpanId>& parentSpanIds)
      : weakTracer(kj::mv(weakTracer)),
        parentSpanIds(parentSpanIds) {}

  bool submitSpanOpen(SpanId spanId,
      SpanId parentSpanId,
      kj::ConstString operationName,
      kj::Date startTime) override {
    bool submitted = false;
    weakTracer->runIfAlive([&](BaseTracer& tracer) {
      parentSpanIds.add(parentSpanId);
      tracer.addSpanOpen(spanId, parentSpanId, kj::mv(operationName), startTime);
      submitted = true;
    });
    return submitted;
  }
  void submitSpanClose(
      SpanId spanId, kj::Date startTime, kj::Date endTime, workerd::Span::TagMap&& tags) override {
    weakTracer->runIfAlive([&](BaseTracer& tracer) {
      tracer.addSpanClose(SpanEndData(spanId, endTime, kj::mv(tags)), startTime);
    });
  }
  void submitSpanUpdate(SpanId spanId, SpanUpdate&& update) override {
    weakTracer->runIfAlive([&](BaseTracer& tracer) {
      static_cast<WorkerTracer&>(tracer).addSpanUpdateInternal(
          spanId, kj::mv(update), kj::UNIX_EPOCH);
    });
  }
  void submitSpanException(SpanId spanId,
      kj::Date timestamp,
      kj::Maybe<Exception::Code> code,
      kj::String name,
      kj::String message,
      kj::Maybe<kj::String> stack) override {
    weakTracer->runIfAlive([&](BaseTracer& tracer) {
      tracer.addSpanException(
          spanId, timestamp, kj::mv(code), kj::mv(name), kj::mv(message), kj::mv(stack));
    });
  }
  SpanId makeSpanId() override {
    return SpanId(nextSpanId++);
  }

 private:
  kj::Own<BaseTracer::WeakRef> weakTracer;
  kj::Vector<SpanId>& parentSpanIds;
  uint64_t nextSpanId = 100;
};

struct TracerTest {
  WriterTest writer;
  kj::Maybe<kj::Rc<WorkerTracer>> tracer;
  // The parent span ID of each span opened, in order.
  kj::Vector<SpanId> parentSpanIds;
  // The invocation's root user span, as IoContext::IncomingRequest creates it.
  SpanParent root = nullptr;

  TracerTest() {
    auto trace = kj::refcounted<Trace>(kj::none, kj::none, kj::none, kj::none, kj::none,
        kj::Array<kj::String>(), kj::none, ExecutionModel::STATELESS);
    auto& t = tracer.emplace(kj::rc<WorkerTracer>(
        kj::none, kj::mv(trace), PipelineLogLevel::FULL, kj::none, kj::mv(writer.writer)));
    t->setEventInfoInternal(writer.context, kj::none, kj::UNIX_EPOCH,
        FetchEventInfo(
            kj::HttpMethod::GET, kj::str("https://example.com"), kj::str("{}"), nullptr));
    t->setUserSpanSubmitter(kj::refcounted<TestSpanSubmitter>(t->getWeakRef(), parentSpanIds));
    root = t->makeUserRequestSpan(writer.context.toSpanContext());
  }

  // Ends the invocation and returns the SpanOpen events the tail worker received.
  kj::Vector<FakeTailStreamTarget::Event> finish() {
    root = nullptr;
    tracer = kj::none;
    writer.target->drain(writer.waitScope);
    kj::Vector<FakeTailStreamTarget::Event> spanOpens;
    for (const auto& event: writer.target->events) {
      if (event.which == rpc::Trace::TailEvent::Event::SPAN_OPEN) {
        spanOpens.add(event);
      }
    }
    return spanOpens;
  }
};

KJ_TEST("The root user span stands for the onset span") {
  TracerTest test;
  auto& invocation = test.writer.context;
  auto rootContext = KJ_ASSERT_NONNULL(test.root.toSpanContext());
  KJ_EXPECT(rootContext.getTraceId() == invocation.getTraceId());
  KJ_EXPECT(KJ_ASSERT_NONNULL(rootContext.getSpanId()) == invocation.getSpanId());

  {
    auto child = test.root.newChild("child"_kjc, kj::UNIX_EPOCH);
    SpanParent childParent(child);
    auto grandchild = childParent.newChild("grandchild"_kjc, kj::UNIX_EPOCH);
  }
  auto spanOpens = test.finish();

  // Spans at the top of the invocation are opened as children of the onset span, rather than with
  // no parent for the tracer to fill in.
  KJ_ASSERT(test.parentSpanIds.size() == 2, test.parentSpanIds.size());
  KJ_EXPECT(test.parentSpanIds[0] == invocation.getSpanId());
  KJ_ASSERT(spanOpens.size() == 2, spanOpens.size());
  KJ_EXPECT(spanOpens[0].traceId == invocation.getTraceId());
  KJ_EXPECT(spanOpens[0].invocationId == invocation.getInvocationId());
  KJ_EXPECT(KJ_ASSERT_NONNULL(spanOpens[0].spanId) == invocation.getSpanId().getId());
  KJ_EXPECT(test.parentSpanIds[1] == SpanId(spanOpens[0].openedSpanId));
  KJ_EXPECT(spanOpens[1].invocationId == invocation.getInvocationId());
  KJ_EXPECT(KJ_ASSERT_NONNULL(spanOpens[1].spanId) == spanOpens[0].openedSpanId);
}

KJ_TEST("WorkerTracer stamps logs and exceptions with the onset's invocationId") {
  TracerTest test;
  auto& invocation = test.writer.context;
  auto logContext = SpanContext(invocation.getTraceId(), SpanId(77), invocation.getTraceFlags());
  KJ_ASSERT_NONNULL(test.tracer)
      ->addLog(logContext, kj::UNIX_EPOCH, LogLevel::INFO, kj::str("[\"hello\"]"));
  KJ_ASSERT_NONNULL(test.tracer)
      ->addException(logContext, kj::UNIX_EPOCH, kj::str("Error"), kj::str("boom"), kj::none);

  test.finish();
  // Onset, Log, Exception, Outcome.
  KJ_ASSERT(test.writer.target->events.size() == 4, test.writer.target->events.size());
  for (const auto& event: test.writer.target->events) {
    KJ_EXPECT(event.invocationId == invocation.getInvocationId());
  }
  KJ_EXPECT(KJ_ASSERT_NONNULL(test.writer.target->events[1].spanId) == 77u);
  KJ_EXPECT(KJ_ASSERT_NONNULL(test.writer.target->events[2].spanId) == 77u);
}

KJ_TEST("WorkerTracer falls back to the onset span when a span has no parent") {
  TracerTest test;
  auto& invocation = test.writer.context;
  {
    KJ_EXPECT_LOG(WARNING, "reported span without a parent span");
    KJ_ASSERT_NONNULL(test.tracer)
        ->addSpanOpen(SpanId(5), SpanId::nullId, "orphan"_kjc, kj::UNIX_EPOCH);
  }
  auto spanOpens = test.finish();

  KJ_ASSERT(spanOpens.size() == 1, spanOpens.size());
  KJ_EXPECT(spanOpens[0].traceId == invocation.getTraceId());
  KJ_EXPECT(KJ_ASSERT_NONNULL(spanOpens[0].spanId) == invocation.getSpanId().getId());
  KJ_EXPECT(spanOpens[0].openedSpanId == 5);
}

KJ_TEST("TailStreamWriter::report() preserves SpanContext and invocationId") {
  WriterTest test;
  TraceId traceId(0x1111, 0x2222);
  TraceId invocationId(0x3333, 0x4444);

  // Onset with no parent spanId omits spanId on the wire.
  test.writer->report(SpanContext(traceId, kj::none, TraceFlags(0x01)), invocationId,
      kj::heap<Onset>(SpanId(10),
          Onset::Info(FetchEventInfo(
              kj::HttpMethod::GET, kj::str("https://example.com"), kj::str("{}"), nullptr)),
          Onset::WorkerInfo{}, nullptr),
      kj::UNIX_EPOCH, 0);
  // Log with a concrete spanId and unsampled traceFlags (0x00, distinct from absent).
  test.writer->report(SpanContext(traceId, SpanId(42), TraceFlags(0x00)), invocationId,
      Log(kj::UNIX_EPOCH, LogLevel::INFO, kj::str("hi")), kj::UNIX_EPOCH, 2);
  // Explicit SpanId::nullId is normalized to absent on the wire.
  test.writer->report(SpanContext(traceId, SpanId::nullId, kj::none), invocationId,
      Outcome(EventOutcome::OK, 0 * kj::SECONDS, 0 * kj::SECONDS), kj::UNIX_EPOCH, 0);

  test.target->drain(test.waitScope);
  KJ_ASSERT(test.target->events.size() == 3);

  KJ_EXPECT(test.target->events[0].traceId == traceId);
  KJ_EXPECT(test.target->events[0].invocationId == invocationId);
  KJ_EXPECT(test.target->events[0].spanId == kj::none);
  KJ_EXPECT(KJ_ASSERT_NONNULL(test.target->events[0].traceFlags) == TraceFlags(0x01));

  KJ_EXPECT(test.target->events[1].traceId == traceId);
  KJ_EXPECT(test.target->events[1].invocationId == invocationId);
  KJ_EXPECT(KJ_ASSERT_NONNULL(test.target->events[1].spanId) == 42u);
  KJ_EXPECT(KJ_ASSERT_NONNULL(test.target->events[1].traceFlags) == TraceFlags(0x00));

  KJ_EXPECT(test.target->events[2].traceId == traceId);
  KJ_EXPECT(test.target->events[2].invocationId == invocationId);
  KJ_EXPECT(test.target->events[2].spanId == kj::none);
  KJ_EXPECT(test.target->events[2].traceFlags == kj::none);
}

}  // namespace
}  // namespace workerd::tracing
