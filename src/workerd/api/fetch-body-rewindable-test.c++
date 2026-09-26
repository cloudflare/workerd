// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "global-scope.h"

#include <workerd/io/io-context.h>
#include <workerd/io/observer.h>
#include <workerd/io/worker-interface.h>
#include <workerd/tests/test-fixture.h>

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
  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      kj::HttpService::Response& response) override {
    co_await requestBody.readAllBytes();
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
};

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

}  // namespace
}  // namespace workerd::api
