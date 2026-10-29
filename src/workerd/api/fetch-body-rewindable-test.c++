// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "global-scope.h"
#include "tracing.h"

#include <workerd/io/io-context.h>
#include <workerd/io/observer.h>
#include <workerd/io/worker-interface.h>
#include <workerd/tests/test-fixture.h>
#include <workerd/util/strings.h>

#include <capnp/message.h>
#include <kj/test.h>

namespace workerd::api {
namespace {

struct RetryEligibility {
  bool bodyRewindable;
  ActorCallTargetRetryable targetRetryable;
};

class RecordingRequestObserver final: public RequestObserver {
 public:
  RecordingRequestObserver(kj::Vector<RetryEligibility>& calls): calls(calls) {}

  void setNextSubrequestRetryEligibility(
      SubrequestBodyRewindable bodyRewindable, ActorCallTargetRetryable targetRetryable) override {
    calls.add(RetryEligibility{bodyRewindable.toBool(), targetRetryable});
  }

 private:
  kj::Vector<RetryEligibility>& calls;
};

// Minimal WorkerInterface that answers every outgoing request() with an empty 200, draining the
// request body first so a streaming sender doesn't block on backpressure.
class MockFetchTarget final: public WorkerInterface {
 public:
  explicit MockFetchTarget(kj::Maybe<kj::Function<void()>> beforeResponse = kj::none,
      kj::Maybe<kj::Function<void(const kj::HttpHeaders&)>> checkHeaders = kj::none)
      : beforeResponse(kj::mv(beforeResponse)),
        checkHeaders(kj::mv(checkHeaders)) {}

  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      kj::HttpService::Response& response) override {
    KJ_IF_SOME(check, checkHeaders) check(headers);
    co_await requestBody.readAllBytes();
    KJ_IF_SOME(check, beforeResponse) {
      co_await kj::evalLater([]() {});
      check();
    }
    // Build the response headers on the same HttpHeaderTable as the request headers; the runtime
    // reads the response with its own registered header IDs, so a fresh table would mismatch.
    auto responseHeaders = headers.cloneShallow();
    responseHeaders.clear();
    response.send(200, "OK"_kj, responseHeaders, static_cast<uint64_t>(0));
  }

  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::HttpConnectSettings settings) override {
    KJ_UNIMPLEMENTED("not used in this test");
  }
  kj::Promise<void> prewarm(kj::StringPtr url) override {
    KJ_UNIMPLEMENTED("not used in this test");
  }
  kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override {
    KJ_UNIMPLEMENTED("not used in this test");
  }
  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime, uint32_t retryCount) override {
    KJ_UNIMPLEMENTED("not used in this test");
  }
  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override {
    return event->notSupported();
  }

 private:
  kj::Maybe<kj::Function<void()>> beforeResponse;
  kj::Maybe<kj::Function<void(const kj::HttpHeaders&)>> checkHeaders;
};

class W3cPolicyChannels final: public TestFixture::DummyIoChannelFactory {
 public:
  W3cPolicyChannels(TimerChannel& timer, bool inject, bool enabled)
      : DummyIoChannelFactory(timer),
        inject(inject),
        enabled(enabled) {}
  bool shouldInjectW3cTraceContext() const override {
    return inject;
  }
  kj::Own<WorkerInterface> startSubrequest(uint channel, SubrequestMetadata metadata) override {
    // Disabling the HTTP writer must not strip native trigger metadata.
    KJ_EXPECT((metadata.userSpanParent.toSpanContext() != kj::none) == enabled);
    return kj::heap<MockFetchTarget>(kj::none,
        kj::Function<void(const kj::HttpHeaders&)>(
            [expected = inject && enabled](const auto& headers) {
      uint parents = 0;
      headers.forEach([&](kj::StringPtr name, kj::StringPtr value) {
        if (strcaseeq(name, "traceparent"_kj)) {
          ++parents;
          KJ_EXPECT(value == "00-00000000000000020000000000000001-000000000000002a-01");
        }
      });
      KJ_EXPECT(parents == (expected ? 1 : 0));
    }));
  }

 private:
  bool inject;
  bool enabled;
};

KJ_TEST("the embedder can disable the W3C writer without disabling native propagation") {
  for (bool enabled: {false, true}) {
    for (bool inject: {false, true}) {
      capnp::MallocMessageBuilder message;
      auto flags = message.initRoot<CompatibilityFlags>();
      flags.setOpentelemetryApi(enabled);
      TestFixture fixture({
        .featureFlags = flags.asReader(),
        .ioChannelFactory = kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)>(
            [=](TimerChannel& timer) -> kj::Rc<IoChannelFactory> {
        return kj::rc<W3cPolicyChannels>(timer, inject, enabled);
      }),
      });
      fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
        auto primitive = env.js.alloc<TracingContext>();
        auto fetcher = env.js.alloc<Fetcher>(
            IoContext::NULL_CLIENT_CHANNEL, Fetcher::RequiresHostAndProtocol::YES);
        kj::Maybe<kj::Promise<void>> pending;
        primitive->withActiveSpanContext(env.js,
            user_tracing::TracingSpanContext{.traceId = tracing::TraceId(1, 2).toW3C(),
              .spanId = tracing::SpanId(42).toGoString(),
              .traceFlags = 1},
            jsg::Function<jsg::JsRef<jsg::JsValue>()>([&](jsg::Lock& js) {
          auto response = fetcher->fetch(js, kj::str("http://example.com/"), kj::none);
          pending = env.context.awaitJs(js, kj::mv(response)).ignoreResult();
          return jsg::JsRef(js, js.undefined());
        }));
        return kj::mv(KJ_ASSERT_NONNULL(pending)).attach(kj::mv(fetcher));
      });
    }
  }
}

KJ_TEST("owner-less untraced handlers cannot invent an invocation parent on egress") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setOpentelemetryApi(true);
  TestFixture fixture({
    .featureFlags = flags.asReader(),
    .ioChannelFactory = kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)>(
        [](TimerChannel& timer) -> kj::Rc<IoChannelFactory> {
    return kj::rc<W3cPolicyChannels>(timer, true, false);
  }),
  });
  // Like hibernatable WebSocket handlers, runInIoContext does not install a request trace
  // scope. A real explicit parent can be activated, but clearing it must not reveal a
  // lazily minted invocation identity that was never exported.
  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto primitive = env.js.alloc<TracingContext>();
    KJ_EXPECT(primitive->getInvocationSpanContext(env.js) == kj::none);
    auto fetcher = env.js.alloc<Fetcher>(
        IoContext::NULL_CLIENT_CHANNEL, Fetcher::RequiresHostAndProtocol::YES);
    kj::Maybe<kj::Promise<void>> pending;
    primitive->withActiveSpanContext(env.js,
        user_tracing::TracingSpanContext{.traceId = tracing::TraceId(1, 2).toW3C(),
          .spanId = tracing::SpanId(42).toGoString(),
          .traceFlags = 1},
        jsg::Function<jsg::JsRef<jsg::JsValue>()>([&](jsg::Lock& js) {
      KJ_EXPECT(primitive->getInvocationSpanContext(js) == kj::none);
      return primitive->withActiveSpanContext(
          js, kj::none, jsg::Function<jsg::JsRef<jsg::JsValue>()>([&](jsg::Lock& js) {
        auto response = fetcher->fetch(js, kj::str("http://example.com/"), kj::none);
        pending = env.context.awaitJs(js, kj::mv(response)).ignoreResult();
        return jsg::JsRef(js, js.undefined());
      }));
    }));
    return kj::mv(KJ_ASSERT_NONNULL(pending)).attach(kj::mv(fetcher));
  });
}

struct FetchTargetIoChannelFactory final: public TestFixture::DummyIoChannelFactory {
  FetchTargetIoChannelFactory(TimerChannel& timer): DummyIoChannelFactory(timer) {}

  kj::Own<WorkerInterface> startSubrequest(uint channel, SubrequestMetadata metadata) override {
    return kj::heap<MockFetchTarget>();
  }
};

// fetchImplNoOutputLock forwards payload and target retryability to RequestObserver so edgeworker
// can classify disconnected outgoing actor calls. The stashed signal is per-call, not sticky: one
// RequestObserver is shared across every outgoing subrequest in an IoContext, so the value set for
// one call must not carry over into the next. Two fetches exercise consecutive values.
KJ_TEST("fetch reports each outgoing call's retry eligibility without staleness") {
  kj::Vector<RetryEligibility> calls;

  TestFixture fixture(TestFixture::SetupParams{
    .mainModuleSource = R"SCRIPT(
        export default {
          async fetch(request) {
            // Buffered (string) body: rewindable.
            await fetch("http://example.com/buffered", { method: "POST", body: "hello" });

            // The incoming request body is a (non-buffer-backed) stream, so forwarding it yields a
            // non-rewindable body.
            await fetch("http://example.com/stream",
                { method: "POST", body: request.body, duplex: "half" });
            return new Response("OK");
          },
        };
      )SCRIPT"_kj,
    .ioChannelFactory = kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)>(
        [&](TimerChannel& timer) -> kj::Rc<IoChannelFactory> {
    return kj::rc<FetchTargetIoChannelFactory>(timer);
  }),
    .requestObserverFactory =
        kj::Function<kj::Own<RequestObserver>()>([&]() -> kj::Own<RequestObserver> {
    return kj::refcounted<RecordingRequestObserver>(calls);
  }),
  });

  auto result =
      fixture.runRequest(kj::HttpMethod::POST, "http://www.example.com"_kj, "incoming-body"_kj);
  KJ_EXPECT(result.statusCode == 200);

  KJ_ASSERT(calls.size() == 2, "expected exactly one retry eligibility signal per outgoing fetch");
  KJ_EXPECT(calls[0].bodyRewindable, "buffered request body should be rewindable");
  KJ_EXPECT(
      !calls[1].bodyRewindable, "streamed request body should not be rewindable (no carryover)");
  KJ_EXPECT(calls[0].targetRetryable == ActorCallTargetRetryable::NO);
  KJ_EXPECT(calls[1].targetRetryable == ActorCallTargetRetryable::NO);
}

struct RecordedFetchSpan {
  kj::String name;
  uint parent;
  uint opened = 0;
  uint closed = 0;
};

struct FetchSpanRecords: public kj::Refcounted {
  kj::Vector<RecordedFetchSpan> spans;
  kj::Vector<uint> fetchIndices;
  uint nextEvent = 1;
  uint checkedRequests = 0;
  bool recordFetchSetup = false;

  uint find(kj::StringPtr name, uint parent) {
    for (uint i = 0; i < spans.size(); ++i) {
      if (spans[i].name == name && spans[i].parent == parent) return i;
    }
    KJ_FAIL_REQUIRE("missing fetch span", name, parent);
    KJ_UNREACHABLE;
  }

  void checkSetup(uint fetchIndex) {
    if (!recordFetchSetup) {
      ++checkedRequests;
      return;
    }
    auto setupIndex = find("fetch_setup"_kj, fetchIndex);
    auto& setup = spans[setupIndex];
    KJ_EXPECT(setup.closed != 0, "fetch setup must finish before the response arrives");
    KJ_EXPECT(spans[fetchIndex].closed == 0, "fetch must still be waiting for the response");
    uint previousEnd = setup.opened;
    for (const auto& name: {"fetch_get_client"_kj, "fetch_prepare_request"_kj, "fetch_dispatch"_kj,
           "fetch_promise_setup"_kj}) {
      auto& phase = spans[find(name, setupIndex)];
      KJ_EXPECT(phase.opened > previousEnd);
      KJ_EXPECT(phase.closed > phase.opened);
      KJ_EXPECT(phase.closed < setup.closed);
      previousEnd = phase.closed;
    }
    ++checkedRequests;
  }
};

class FetchSpanObserver final: public SpanObserver {
 public:
  FetchSpanObserver(kj::Rc<FetchSpanRecords> records, uint parent)
      : records(kj::mv(records)),
        index(this->records->spans.size()) {
    this->records->spans.add(RecordedFetchSpan{.parent = parent});
  }

  kj::Rc<SpanObserver> newChild() override {
    return kj::rc<FetchSpanObserver>(records.addRef(), index);
  }
  kj::Rc<SpanObserver> newOptionalChild(kj::ConstString operationName) override {
    if (records->recordFetchSetup && operationName == "fetch_setup"_kjc) return newChild();
    return SpanObserver::newOptionalChild(kj::mv(operationName));
  }
  void onOpen(kj::ConstString operationName, kj::Date startTime) override {
    auto& span = records->spans[index];
    span.name = kj::str(operationName);
    span.opened = records->nextEvent++;
  }
  void onClose(kj::Date endTime, Span::TagMap&& tags, kj::Vector<Span::Log>&& logs) override {
    records->spans[index].closed = records->nextEvent++;
  }
  uint getIndex() {
    return index;
  }

 private:
  kj::Rc<FetchSpanRecords> records;
  uint index;
};

class FetchSpanRequestObserver final: public RequestObserver {
 public:
  explicit FetchSpanRequestObserver(kj::Rc<FetchSpanRecords> records)
      : span(kj::rc<FetchSpanObserver>(kj::mv(records), kj::maxValue), "request"_kjc) {}

  SpanParent getSpan() override {
    return SpanParent(span);
  }

 private:
  SpanBuilder span;
};

struct TracedFetchChannels final: public TestFixture::DummyIoChannelFactory {
  TracedFetchChannels(TimerChannel& timer, kj::Rc<FetchSpanRecords> records)
      : DummyIoChannelFactory(timer),
        records(kj::mv(records)) {}

  kj::Own<WorkerInterface> startSubrequest(uint channel, SubrequestMetadata metadata) override {
    auto& observer = KJ_ASSERT_NONNULL(metadata.parentSpan.getObserver());
    auto* fetchObserver = dynamic_cast<FetchSpanObserver*>(&observer);
    KJ_REQUIRE(fetchObserver != nullptr);
    auto fetchIndex = fetchObserver->getIndex();
    KJ_EXPECT(records->spans[fetchIndex].name == "fetch"_kj,
        "outgoing requests must keep fetch as their propagated parent");
    records->fetchIndices.add(fetchIndex);
    if (records->recordFetchSetup) {
      auto setupIndex = records->find("fetch_setup"_kj, fetchIndex);
      auto clientIndex = records->find("fetch_get_client"_kj, setupIndex);
      KJ_EXPECT(records->spans[clientIndex].closed == 0,
          "client construction must include channel routing");
    }
    return kj::heap<MockFetchTarget>(kj::Function<void()>(
        [records = records.addRef(), fetchIndex]() mutable { records->checkSetup(fetchIndex); }));
  }

  kj::Rc<FetchSpanRecords> records;
};

void runTracedFetches(kj::Rc<FetchSpanRecords> records) {
  TestFixture fixture(TestFixture::SetupParams{
    .mainModuleSource = R"SCRIPT(
      export default {
        async fetch(request) {
          await fetch("http://example.com/empty");
          await fetch("http://example.com/buffered", {method: "POST", body: "hello"});
          await fetch("http://example.com/stream", {
            method: "POST", body: request.body, duplex: "half"
          });
          await fetch("http://example.com/websocket", {headers: {Upgrade: "websocket"}});
          return new Response("OK");
        }
      };
    )SCRIPT"_kj,
    .ioChannelFactory = kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)>(
        [records = records.addRef()](TimerChannel& timer) mutable -> kj::Rc<IoChannelFactory> {
    return kj::rc<TracedFetchChannels>(timer, records.addRef());
  }),
    .requestObserverFactory = kj::Function<kj::Own<RequestObserver>()>(
        [records = records.addRef()]() mutable -> kj::Own<RequestObserver> {
    return kj::refcounted<FetchSpanRequestObserver>(records.addRef());
  }),
  });
  auto result = fixture.runRequest(kj::HttpMethod::POST, "http://worker/"_kj, "body"_kj);
  KJ_EXPECT(result.statusCode == 200);
  KJ_EXPECT(records->checkedRequests == 4);
}

KJ_TEST("fetch setup phases finish before I/O and preserve the outgoing trace parent") {
  auto records = kj::rc<FetchSpanRecords>();
  records->recordFetchSetup = true;
  runTracedFetches(records.addRef());
}

KJ_TEST("fetch setup does not emit unsupported spans or consume existing span IDs") {
  auto records = kj::rc<FetchSpanRecords>();
  runTracedFetches(records.addRef());
  KJ_ASSERT(records->fetchIndices.size() == 4);
  for (uint i = 1; i < records->fetchIndices.size(); ++i) {
    KJ_EXPECT(records->fetchIndices[i] == records->fetchIndices[i - 1] + 1,
        "unsupported setup spans must not allocate child observers");
  }
  for (const auto& span: records->spans) {
    KJ_EXPECT(span.name != "fetch_setup"_kj);
    KJ_EXPECT(span.name != "fetch_get_client"_kj);
    KJ_EXPECT(span.name != "fetch_prepare_request"_kj);
    KJ_EXPECT(span.name != "fetch_dispatch"_kj);
    KJ_EXPECT(span.name != "fetch_promise_setup"_kj);
  }
}

}  // namespace
}  // namespace workerd::api
