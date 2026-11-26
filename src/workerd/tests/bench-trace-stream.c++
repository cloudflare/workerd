// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Streams batches of tail events to a tail worker whose handler does nothing, which measures how
// fast TailStreamTarget turns the events of a report() into JavaScript values and hands them over.

#include <workerd/io/trace-stream.h>
#include <workerd/io/worker-interface.capnp.h>
#include <workerd/tests/bench-tools.h>
#include <workerd/tests/test-fixture.h>

namespace workerd::tracing {
namespace {

constexpr kj::StringPtr TAIL_WORKER = R"SCRIPT(
  export default {
    tailStream() {
      return () => {};
    }
  };
)SCRIPT"_kj;

struct ErrorHandler final: public kj::TaskSet::ErrorHandler {
  void taskFailed(kj::Exception&& exception) override {
    kj::throwFatalException(kj::mv(exception));
  }
};

using EventsBuilder = capnp::List<rpc::Trace::TailEvent>::Builder;

// The events of every batch: each kind that a traced request commonly reports.
constexpr uint EVENTS_PER_BATCH = 100;

void setCommon(rpc::Trace::TailEvent::Builder event, uint32_t sequence) {
  auto context = event.initSpanContext();
  auto traceId = context.initTraceId();
  traceId.setHigh(0x0123456789abcdef);
  traceId.setLow(0xfedcba9876543210);
  context.getInfo().setSpanId(0xab);
  context.initTraceFlags().getValue().setSet(1);
  auto invocationId = event.initInvocationId();
  invocationId.setHigh(0x1111111111111111);
  invocationId.setLow(0x2222222222222222);
  event.setTimestampNs(1'700'000'000'123'456'789);
  event.setSequence(sequence);
}

void setAttributes(capnp::List<rpc::Trace::Attribute>::Builder attributes) {
  for (auto i: kj::indices(attributes)) {
    attributes[i].setName(kj::str("attribute.", i));
    auto value = attributes[i].initValue(1)[0];
    switch (i % 3) {
      case 0:
        value.setString("some text");
        break;
      case 1:
        value.setInt64(i);
        break;
      default:
        value.setBool(true);
        break;
    }
  }
}

void setEvents(EventsBuilder events, uint32_t& sequence) {
  for (auto i: kj::indices(events)) {
    auto event = events[i];
    setCommon(event, ++sequence);
    switch (i % 5) {
      case 0: {
        auto open = event.getEvent().initSpanOpen();
        open.setOperationName("fetch");
        open.setSpanId(sequence);
        auto fetch = open.getInfo().initFetch();
        fetch.setMethod(capnp::HttpMethod::GET);
        fetch.setUrl("https://example.com/some/path?with=query");
        auto headers = fetch.initHeaders(4);
        for (auto h: kj::indices(headers)) {
          headers[h].setName(kj::str("x-header-", h));
          headers[h].setValue("some header value");
        }
        break;
      }
      case 1:
        setAttributes(event.getEvent().initAttribute(6));
        break;
      case 2: {
        auto log = event.getEvent().initLog();
        log.setLogLevel(rpc::Trace::Log::Level::LOG);
        log.setMessage(R"(["request handled",{"status":200,"path":"/some/path"}])");
        break;
      }
      case 3:
        event.getEvent().initReturn().getInfo().initFetch().setStatusCode(200);
        break;
      default:
        event.getEvent().initSpanClose().setOutcome(EventOutcome::OK);
        break;
    }
  }
}

struct TraceStreamBenchmark: public benchmark::Fixture {
  virtual ~TraceStreamBenchmark() noexcept(true) {}

  void SetUp(benchmark::State& state) noexcept(true) override {
    fixture = kj::heap<TestFixture>(TestFixture::SetupParams{.mainModuleSource = TAIL_WORKER});
    waitUntilTasks = kj::heap<kj::TaskSet>(errorHandler);
    auto event = kj::heap<TailStreamCustomEvent>();
    cap = event->getCap();
    auto context = fixture->newIoContext();
    done = event
               ->run(fixture->newUndeliveredIncomingRequest(*context), kj::none, kj::none,
                   Frankenvalue(), *waitUntilTasks, false)
               .attach(kj::mv(event));

    auto request = cap.reportRequest();
    auto event0 = request.initEvents(1)[0];
    setCommon(event0, 0);
    event0.getEvent().initOnset().initInfo().initFetch().setUrl("https://example.com/");
    KJ_ASSERT(!request.send().wait(fixture->getWaitScope()).getStop());
  }

  void TearDown(benchmark::State& state) noexcept(true) override {
    auto request = cap.reportRequest();
    auto event = request.initEvents(1)[0];
    setCommon(event, ++sequence);
    event.getEvent().initOutcome().setOutcome(EventOutcome::OK);
    KJ_ASSERT(request.send().wait(fixture->getWaitScope()).getStop());
    done.wait(fixture->getWaitScope());
    waitUntilTasks->onEmpty().wait(fixture->getWaitScope());
    cap = nullptr;
    waitUntilTasks = nullptr;
    fixture = nullptr;
  }

  kj::Own<TestFixture> fixture;
  ErrorHandler errorHandler;
  kj::Own<kj::TaskSet> waitUntilTasks;
  rpc::TailStreamTarget::Client cap = nullptr;
  kj::Promise<WorkerInterface::CustomEvent::Result> done = nullptr;
  uint32_t sequence = 0;
};

BENCHMARK_F(TraceStreamBenchmark, report)(benchmark::State& state) {
  for (auto _: state) {
    auto request = cap.reportRequest();
    setEvents(request.initEvents(EVENTS_PER_BATCH), sequence);
    benchmark::DoNotOptimize(request.send().wait(fixture->getWaitScope()).getStop());
  }
  state.SetItemsProcessed(state.iterations() * EVENTS_PER_BATCH);
}

}  // namespace
}  // namespace workerd::tracing
