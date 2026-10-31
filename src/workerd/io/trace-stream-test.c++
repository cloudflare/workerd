// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/io/frankenvalue.h>
#include <workerd/io/trace-stream.h>
#include <workerd/io/worker-interface.h>

#include <capnp/compat/byte-stream.h>
#include <capnp/compat/http-over-capnp.h>
#include <kj/async.h>
#include <kj/test.h>

namespace workerd::tracing {
namespace {

// What a tail worker received, in order.
struct Received {
  // One entry per event: its type, followed by the stack for exceptions.
  kj::Vector<kj::String> events;
  kj::Own<kj::PromiseFulfiller<void>> outcomeFulfiller;
};

class RecordingTarget final: public rpc::TailStreamTarget::Server {
 public:
  explicit RecordingTarget(Received& received): received(received) {}

  kj::Promise<void> report(ReportContext context) override {
    for (auto event: context.getParams().getEvents()) {
      auto info = event.getEvent();
      switch (info.which()) {
        case rpc::Trace::TailEvent::Event::ONSET:
          received.events.add(kj::str("onset"));
          break;
        case rpc::Trace::TailEvent::Event::LOG:
          received.events.add(kj::str("log"));
          break;
        case rpc::Trace::TailEvent::Event::EXCEPTION:
          received.events.add(kj::str("exception: ", info.getException().getStack()));
          break;
        case rpc::Trace::TailEvent::Event::OUTCOME:
          received.events.add(kj::str("outcome"));
          context.initResults().setStop(true);
          received.outcomeFulfiller->fulfill();
          break;
        default:
          KJ_FAIL_ASSERT("unexpected tail event", info);
      }
    }
    return kj::READY_NOW;
  }

 private:
  Received& received;
};

class FakeDispatcher final: public rpc::EventDispatcher::Server {
 public:
  FakeDispatcher(Received& received, kj::Promise<void> outcomeReceived)
      : received(received),
        outcomeReceived(kj::mv(outcomeReceived)) {}

  kj::Promise<void> tailStreamSession(TailStreamSessionContext context) override {
    rpc::TailStreamTarget::Client target = kj::heap<RecordingTarget>(received);
    capnp::PipelineBuilder<TailStreamSessionResults> pipelineBuilder;
    pipelineBuilder.setTopLevel(target);
    context.setPipeline(pipelineBuilder.build());
    context.getResults().setTopLevel(kj::mv(target));

    co_await outcomeReceived;
    context.getResults().setResult(EventOutcome::OK);
  }

 private:
  Received& received;
  kj::Promise<void> outcomeReceived;
};

// A tail worker that records the events it is sent over RPC.
class FakeTailWorker final: public WorkerInterface {
 public:
  explicit FakeTailWorker(Received& received) {
    auto paf = kj::newPromiseAndFulfiller<void>();
    received.outcomeFulfiller = kj::mv(paf.fulfiller);
    dispatcher = kj::heap<FakeDispatcher>(received, kj::mv(paf.promise));
  }

  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override {
    return event
        ->sendRpc(httpOverCapnpFactory, byteStreamFactory, getUnsupportedFrankenvalueHandler(),
            kj::mv(dispatcher))
        .attach(kj::mv(event));
  }

  kj::Promise<void> request(kj::HttpMethod,
      kj::StringPtr,
      const kj::HttpHeaders&,
      kj::AsyncInputStream&,
      kj::HttpService::Response&) override {
    KJ_UNIMPLEMENTED("not used in this test");
  }
  kj::Promise<void> connect(kj::StringPtr,
      const kj::HttpHeaders&,
      kj::AsyncIoStream&,
      ConnectResponse&,
      kj::HttpConnectSettings) override {
    KJ_UNIMPLEMENTED("not used in this test");
  }
  kj::Promise<void> prewarm(kj::StringPtr) override {
    KJ_UNIMPLEMENTED("not used in this test");
  }
  kj::Promise<ScheduledResult> runScheduled(kj::Date, kj::StringPtr) override {
    KJ_UNIMPLEMENTED("not used in this test");
  }
  kj::Promise<AlarmResult> runAlarm(kj::Date, uint32_t) override {
    KJ_UNIMPLEMENTED("not used in this test");
  }

 private:
  capnp::ByteStreamFactory byteStreamFactory;
  kj::HttpHeaderTable::Builder headerTableBuilder;
  capnp::HttpOverCapnpFactory httpOverCapnpFactory{byteStreamFactory,
    capnp::HttpOverCapnpFactory::HeaderIdBundle(headerTableBuilder),
    capnp::HttpOverCapnpFactory::LEVEL_2};
  rpc::EventDispatcher::Client dispatcher = nullptr;
};

// Prefixes each stack with "remapped ". While `gate` is set, remapping waits for it.
class TestRemapper final: public TailStreamWriter::StackRemapper {
 public:
  uint calls = 0;
  bool fail = false;
  kj::Maybe<kj::Promise<void>> gate;

  kj::Promise<void> remap(kj::ArrayPtr<kj::String> stacks) override {
    ++calls;

    if (fail) {
      return KJ_EXCEPTION(FAILED, "remap failed");
    }

    KJ_IF_SOME(promise, gate) {
      auto result = promise.then([stacks]() mutable { prefix(stacks); });
      gate = kj::none;
      return result;
    }

    prefix(stacks);
    return kj::READY_NOW;
  }

 private:
  static void prefix(kj::ArrayPtr<kj::String> stacks) {
    for (auto& stack: stacks) {
      stack = kj::str("remapped ", stack);
    }
  }
};

class FailOnTaskError final: public kj::TaskSet::ErrorHandler {
 public:
  void taskFailed(kj::Exception&& exception) override {
    KJ_FAIL_EXPECT(exception);
  }
};

// Reports tail events to one fake tail worker.
struct TestStream {
  kj::EventLoop loop;
  kj::WaitScope waitScope{loop};
  FailOnTaskError errorHandler;
  kj::TaskSet waitUntilTasks{errorHandler};
  Received received;
  kj::Own<TailStreamWriter> writer;
  InvocationSpanContext context{TraceId(1, 2), TraceId(3, 4), SpanId(5), kj::none};

  explicit TestStream(kj::Maybe<kj::Rc<TailStreamWriter::StackRemapper>> remapper) {
    writer = KJ_ASSERT_NONNULL(initializeTailStreamWriter(
        kj::arr<kj::Own<WorkerInterface>>(kj::heap<FakeTailWorker>(received)), waitUntilTasks,
        kj::mv(remapper)));
  }

  void reportOnset() {
    report(Onset(SpanId(5), ScheduledEventInfo(0, kj::str()), Onset::WorkerInfo{}, nullptr));
  }
  void reportLog() {
    report(Log(kj::UNIX_EPOCH, LogLevel::LOG, kj::str("message")));
  }
  void reportException(kj::Maybe<kj::StringPtr> stack) {
    report(Exception(kj::UNIX_EPOCH, kj::str("Error"), kj::str("boom"),
        stack.map([](kj::StringPtr s) { return kj::str(s); })));
  }

  // Reports the outcome and waits until the tail worker's session is over.
  void finish() {
    report(Outcome(EventOutcome::OK, 0 * kj::SECONDS, 0 * kj::SECONDS));
    waitUntilTasks.onEmpty().wait(waitScope);
  }

  kj::String receivedEvents() {
    return kj::strArray(received.events, ", ");
  }

 private:
  void report(TailEvent::Event&& event) {
    writer->report(context, kj::mv(event), kj::UNIX_EPOCH, 0);
  }
};

KJ_TEST("TailStreamWriter delivers stacks unchanged without a remapper") {
  TestStream stream(kj::none);
  stream.reportOnset();
  stream.reportException("at foo"_kj);
  stream.finish();

  KJ_EXPECT(
      stream.receivedEvents() == "onset, exception: at foo, outcome", stream.receivedEvents());
}

KJ_TEST("TailStreamWriter remaps exception stacks") {
  auto remapper = kj::rc<TestRemapper>();
  TestStream stream(remapper.addRef());
  stream.reportOnset();
  stream.reportException("at foo"_kj);
  stream.reportException("at bar"_kj);
  stream.finish();

  KJ_EXPECT(stream.receivedEvents() ==
          "onset, exception: remapped at foo, exception: remapped at bar, outcome",
      stream.receivedEvents());
  KJ_EXPECT(remapper->calls == 1);
}

KJ_TEST("TailStreamWriter only remaps batches with an exception stack") {
  auto remapper = kj::rc<TestRemapper>();
  TestStream stream(remapper.addRef());
  stream.reportOnset();
  stream.reportLog();
  stream.reportException(kj::none);
  stream.finish();

  KJ_EXPECT(stream.receivedEvents() == "onset, log, exception: , outcome", stream.receivedEvents());
  KJ_EXPECT(remapper->calls == 0);
}

KJ_TEST("TailStreamWriter delivers events reported during a remap afterwards, in order") {
  auto remapper = kj::rc<TestRemapper>();
  auto gate = kj::newPromiseAndFulfiller<void>();
  remapper->gate = kj::mv(gate.promise);
  TestStream stream(remapper.addRef());
  stream.reportOnset();
  stream.reportException("at foo"_kj);

  // Let the exception's batch reach the remapper, then report more events while it waits.
  while (remapper->calls == 0) {
    KJ_ASSERT(stream.waitScope.poll() > 0, "remapper was never called");
  }
  stream.reportLog();
  gate.fulfiller->fulfill();
  stream.finish();

  KJ_EXPECT(stream.receivedEvents() == "onset, exception: remapped at foo, log, outcome",
      stream.receivedEvents());
}

KJ_TEST("TailStreamWriter delivers the original stacks when remapping fails") {
  auto remapper = kj::rc<TestRemapper>();
  remapper->fail = true;
  TestStream stream(remapper.addRef());
  stream.reportOnset();
  stream.reportException("at foo"_kj);

  KJ_EXPECT_LOG(WARNING, "failed to remap tail stream exception stacks");
  stream.finish();

  KJ_EXPECT(
      stream.receivedEvents() == "onset, exception: at foo, outcome", stream.receivedEvents());
}

}  // namespace
}  // namespace workerd::tracing
