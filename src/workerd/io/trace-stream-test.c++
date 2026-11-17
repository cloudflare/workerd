// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "trace-stream.h"

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

  kj::Vector<Batch> batches;
  bool stopOnOnset = false;

  kj::Promise<void> report(ReportContext context) override {
    Batch batch;
    for (auto event: context.getParams().getEvents()) {
      auto e = event.getEvent();
      batch.events.add(e.which());
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

}  // namespace
}  // namespace workerd::tracing
