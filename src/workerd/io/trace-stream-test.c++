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
    kj::Maybe<uint64_t> spanId;
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
      };
      if (spanContext.getInfo().isSpanId()) {
        recorded.spanId = spanContext.getInfo().getSpanId();
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
    writer->report(context,
        kj::heap<Onset>(SpanId(1),
            Onset::Info(FetchEventInfo(
                kj::HttpMethod::GET, kj::str("https://example.com"), kj::str("{}"), nullptr)),
            Onset::WorkerInfo{}, nullptr),
        kj::UNIX_EPOCH, 0);
  }

  // Reports a log whose message is reported to own `size` bytes.
  void reportLog(size_t size) {
    writer->report(
        context, Log(kj::UNIX_EPOCH, LogLevel::INFO, kj::str("x")), kj::UNIX_EPOCH, size);
  }

  void reportOutcome() {
    writer->report(
        context, Outcome(EventOutcome::OK, 0 * kj::SECONDS, 0 * kj::SECONDS), kj::UNIX_EPOCH, 0);
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

// Forwards spans straight to a WorkerTracer, with predictable span and trace IDs.
class TestSpanSubmitter final: public SpanSubmitter {
 public:
  explicit TestSpanSubmitter(WorkerTracer& tracer): tracer(tracer) {}

  bool submitSpanOpen(SpanId spanId,
      SpanId parentSpanId,
      kj::ConstString operationName,
      kj::Date startTime) override {
    tracer.addSpanOpen(spanId, parentSpanId, kj::mv(operationName), startTime);
    return true;
  }
  void submitSpanClose(
      SpanId spanId, kj::Date startTime, kj::Date endTime, workerd::Span::TagMap&& tags) override {
    tracer.addSpanClose(SpanEndData(spanId, endTime, kj::mv(tags)), startTime);
  }
  void submitSpanUpdate(SpanId spanId, SpanUpdate&& update) override {
    tracer.addSpanUpdateInternal(spanId, kj::mv(update), kj::UNIX_EPOCH);
  }
  void submitSpanException(SpanId spanId,
      kj::Date timestamp,
      kj::Maybe<Exception::Code> code,
      kj::String name,
      kj::String message,
      kj::Maybe<kj::String> stack) override {
    tracer.addSpanException(
        spanId, timestamp, kj::mv(code), kj::mv(name), kj::mv(message), kj::mv(stack));
  }
  SpanId makeSpanId() override {
    return SpanId(nextSpanId++);
  }
  void submitSpanTrace(SpanId spanId, TraceId traceId, kj::Maybe<TraceFlags> traceFlags) override {
    tracer.setSpanTrace(spanId, kj::mv(traceId), traceFlags);
  }
  TraceId makeRootTraceId() override {
    return TraceId(nextTraceId++, 0xf00d);
  }

 private:
  WorkerTracer& tracer;
  uint64_t nextSpanId = 100;
  uint64_t nextTraceId = 1;
};

struct TracerTest {
  WriterTest writer;
  kj::Maybe<kj::Rc<WorkerTracer>> tracer;
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
    root = SpanParent(kj::rc<UserSpanObserver>(kj::refcounted<TestSpanSubmitter>(*t),
        writer.context.getTraceId(), writer.context.getTraceFlags()));
  }

  const TraceId& invocationTraceId() {
    return writer.context.getTraceId();
  }

  // Ends the invocation and returns the span events the tail worker received.
  kj::Vector<FakeTailStreamTarget::Event> finish() {
    root = nullptr;
    tracer = kj::none;
    writer.target->drain(writer.waitScope);
    kj::Vector<FakeTailStreamTarget::Event> spanEvents;
    using Which = rpc::Trace::TailEvent::Event;
    for (auto& event: writer.target->events) {
      if (event.which == Which::SPAN_OPEN || event.which == Which::SPAN_CLOSE ||
          event.which == Which::SPAN_UPDATE || event.which == Which::ATTRIBUTE ||
          event.which == Which::EXCEPTION) {
        spanEvents.add(kj::mv(event));
      }
    }
    return spanEvents;
  }
};

using Which = rpc::Trace::TailEvent::Event;

KJ_TEST("WorkerTracer reports children of a remote parent in the remote trace") {
  TracerTest test;
  TraceId remoteTraceId(0x1111, 0x2222);
  SpanContext remoteContext(remoteTraceId, SpanId(0x77), TraceFlags(1));
  auto remote = test.root.forActiveContext(remoteContext);
  KJ_ASSERT(remote.isObserved());
  {
    auto child = remote.newChild("child"_kjc, kj::UNIX_EPOCH);
    child.setStatus(SpanStatus(SpanStatusCode::OK, kj::none));
    child.setTag("key"_kjc, "value"_kjc);
    // SpanBuilder::recordException() reads the time from the IoContext, which this test lacks.
    KJ_ASSERT_NONNULL(child.getObserver())
        .onException(kj::UNIX_EPOCH, kj::none, kj::str("Error"), kj::str("boom"), kj::none);
    SpanParent childParent(child);
    auto grandchild = childParent.newChild("grandchild"_kjc, kj::UNIX_EPOCH);
  }
  { auto local = test.root.newChild("local"_kjc, kj::UNIX_EPOCH); }
  auto events = test.finish();

  KJ_ASSERT(events.size() == 9, events.size());
  auto& childOpen = events[0];
  KJ_EXPECT(childOpen.which == Which::SPAN_OPEN);
  KJ_EXPECT(childOpen.traceId == remoteTraceId);
  KJ_EXPECT(KJ_ASSERT_NONNULL(childOpen.spanId) == 0x77);
  auto childId = childOpen.openedSpanId;

  // The child's own events are all in the remote trace.
  for (auto i: {1, 2, 5, 6}) {
    KJ_EXPECT(events[i].traceId == remoteTraceId, i);
    KJ_EXPECT(KJ_ASSERT_NONNULL(events[i].spanId) == childId, i);
  }
  KJ_EXPECT(events[1].which == Which::SPAN_UPDATE);
  KJ_EXPECT(events[2].which == Which::EXCEPTION);
  KJ_EXPECT(events[5].which == Which::ATTRIBUTE);
  KJ_EXPECT(events[6].which == Which::SPAN_CLOSE);

  auto& grandchildOpen = events[3];
  KJ_EXPECT(grandchildOpen.which == Which::SPAN_OPEN);
  KJ_EXPECT(grandchildOpen.traceId == remoteTraceId);
  KJ_EXPECT(KJ_ASSERT_NONNULL(grandchildOpen.spanId) == childId);
  KJ_EXPECT(events[4].which == Which::SPAN_CLOSE);
  KJ_EXPECT(events[4].traceId == remoteTraceId);
  KJ_EXPECT(KJ_ASSERT_NONNULL(events[4].spanId) == grandchildOpen.openedSpanId);

  // Spans in the invocation's trace are unaffected.
  KJ_EXPECT(events[7].which == Which::SPAN_OPEN);
  KJ_EXPECT(events[7].traceId == test.invocationTraceId());
  KJ_EXPECT(KJ_ASSERT_NONNULL(events[7].spanId) == test.writer.context.getSpanId().getId());
  KJ_EXPECT(events[8].which == Which::SPAN_CLOSE);
  KJ_EXPECT(events[8].traceId == test.invocationTraceId());
}

KJ_TEST("WorkerTracer doesn't make a root of a span registered in the invocation's trace") {
  TracerTest test;
  auto& tracer = *KJ_ASSERT_NONNULL(test.tracer);
  tracer.setSpanTrace(SpanId(5), test.invocationTraceId(), kj::none);
  tracer.addSpanOpen(SpanId(5), SpanId::nullId, kj::ConstString("span"_kjc), kj::UNIX_EPOCH);
  tracer.addSpanClose(SpanEndData(SpanId(5), kj::UNIX_EPOCH), kj::UNIX_EPOCH);
  auto events = test.finish();

  KJ_ASSERT(events.size() == 2, events.size());
  KJ_EXPECT(events[0].traceId == test.invocationTraceId());
  KJ_EXPECT(KJ_ASSERT_NONNULL(events[0].spanId) == test.writer.context.getSpanId().getId());
}

KJ_TEST("WorkerTracer reports a root span with no parent, in its own trace") {
  TracerTest test;
  auto roots = test.root.forActiveContext(kj::none);
  KJ_ASSERT(roots.isObserved());
  KJ_EXPECT(roots.toSpanContext() == kj::none);
  {
    auto first = roots.newChild("first"_kjc, kj::UNIX_EPOCH);
    // A new trace is sampled, whatever the invocation's trace flags.
    auto firstContext =
        KJ_ASSERT_NONNULL(KJ_ASSERT_NONNULL(first.getObserver()).toSpanContext());
    KJ_EXPECT(KJ_ASSERT_NONNULL(firstContext.getTraceFlags()) == TraceFlags(1));
    SpanParent firstParent(first);
    auto child = firstParent.newChild("child"_kjc, kj::UNIX_EPOCH);
  }
  { auto second = roots.newChild("second"_kjc, kj::UNIX_EPOCH); }
  auto events = test.finish();

  KJ_ASSERT(events.size() == 6, events.size());
  auto& firstOpen = events[0];
  KJ_EXPECT(firstOpen.which == Which::SPAN_OPEN);
  KJ_EXPECT(firstOpen.spanId == kj::none);
  auto firstTraceId = firstOpen.traceId;
  KJ_EXPECT(firstTraceId != test.invocationTraceId());

  auto& childOpen = events[1];
  KJ_EXPECT(childOpen.which == Which::SPAN_OPEN);
  KJ_EXPECT(childOpen.traceId == firstTraceId);
  KJ_EXPECT(KJ_ASSERT_NONNULL(childOpen.spanId) == firstOpen.openedSpanId);

  KJ_EXPECT(events[2].which == Which::SPAN_CLOSE);
  KJ_EXPECT(events[2].traceId == firstTraceId);
  KJ_EXPECT(events[3].which == Which::SPAN_CLOSE);
  KJ_EXPECT(events[3].traceId == firstTraceId);
  KJ_EXPECT(KJ_ASSERT_NONNULL(events[3].spanId) == firstOpen.openedSpanId);

  // Each child of the root parent starts its own trace.
  auto& secondOpen = events[4];
  KJ_EXPECT(secondOpen.spanId == kj::none);
  KJ_EXPECT(secondOpen.traceId != firstTraceId);
  KJ_EXPECT(secondOpen.traceId != test.invocationTraceId());
  KJ_EXPECT(events[5].traceId == secondOpen.traceId);
}

KJ_TEST("WorkerTracer treats an invalid active context as no active span") {
  TracerTest test;
  SpanContext noSpanId(TraceId(0x1111, 0x2222), SpanId::nullId, TraceFlags(1));
  SpanContext noTraceId(TraceId(nullptr), SpanId(0x77), TraceFlags(1));
  for (auto* context: {&noSpanId, &noTraceId}) {
    auto parent = test.root.forActiveContext(*context);
    KJ_EXPECT(parent.toSpanContext() == kj::none);
    { auto span = parent.newChild("span"_kjc, kj::UNIX_EPOCH); }
  }
  auto events = test.finish();

  KJ_ASSERT(events.size() == 4, events.size());
  for (auto i: {0, 2}) {
    KJ_EXPECT(events[i].which == Which::SPAN_OPEN);
    KJ_EXPECT(events[i].spanId == kj::none, i);
    KJ_EXPECT(events[i].traceId != test.invocationTraceId(), i);
    KJ_EXPECT(events[i].traceId != TraceId(0x1111, 0x2222), i);
  }
}

}  // namespace
}  // namespace workerd::tracing
