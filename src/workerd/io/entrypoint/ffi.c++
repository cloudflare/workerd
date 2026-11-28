// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ffi.h"

#include <workerd/io/entrypoint/ffi.rs.h>
#include <workerd/rust/worker/bridge.h>
#include <workerd/util/autogate.h>

namespace workerd::entrypoint {

kj::Own<Invocation> cxx_entrypoint_take_invocation(CxxEntrypoint& entrypoint) {
  return entrypoint.takeInvocation();
}

IoContext::IncomingRequest& invocation_request(Invocation& invocation) {
  return *invocation.request;
}

kj::Own<IoContext::IncomingRequest> invocation_take_request(Invocation& invocation) {
  return kj::mv(invocation.request);
}

namespace {

// Finds the test event's handler, as the prelude of the run that calls it: its test() function,
// the handler, env and ctx, in that order.
class TestHandlerLookupPrelude final: public io_context::RunPrelude {
 public:
  explicit TestHandlerLookupPrelude(HandlerLookup lookup): lookup(kj::mv(lookup)) {}

  kj::Array<v8::Local<v8::Value>> run(Worker::Lock& lock, IoContext& context) override {
    auto handles = getTestHandles(lock, context, kj::mv(lookup));
    return kj::arr(handles.test, handles.handler, handles.env, handles.ctx);
  }

  kj::StringPtr spanName() const override {
    return "WorkerEntrypoint::test() run"_kj;
  }

 private:
  HandlerLookup lookup;
};

}  // namespace

kj::Own<io_context::RunPrelude> invocation_take_test_handler_lookup(Invocation& invocation) {
  return kj::heap<TestHandlerLookupPrelude>(HandlerLookup{
    .entrypointName = kj::mv(invocation.entrypointName),
    .versionInfo = kj::mv(invocation.versionInfo),
    .props = kj::mv(invocation.props),
  });
}

void invocation_start_scheduled_in_wait_until(
    Invocation& invocation, kj::Date scheduledTime, ::rust::Slice<const kj::byte> cron) {
  startScheduledInWaitUntil(invocation.request->getContext(), scheduledTime,
      kj::str(kj::from<kj_rs::Rust>(cron).asChars()), kj::mv(invocation.entrypointName),
      kj::mv(invocation.versionInfo), kj::mv(invocation.props));
}

kj::Promise<bool> runTest(kj::Own<Invocation> invocation) {
  return test_event_finish(test_event_start(kj::mv(invocation)));
}

namespace {

// The WorkerInterface of a Rust WorkerEntrypoint. The events that Rust delivers go to Rust, and the
// others straight to the C++ WorkerEntrypoint, with no Rust in between.
class RustWorkerEntrypoint final: public WorkerInterface {
 public:
  explicit RustWorkerEntrypoint(::rust::Box<WorkerEntrypoint> entrypoint)
      : entrypoint(kj::mv(entrypoint)),
        cxx(this->entrypoint->cxx()) {}

  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      Response& response) override {
    return cxx.request(method, url, headers, requestBody, response);
  }

  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::HttpConnectSettings settings) override {
    return cxx.connect(host, headers, connection, response, settings);
  }

  kj::Promise<void> prewarm(kj::StringPtr url) override {
    return cxx.prewarm(url);
  }

  kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override {
    if (!util::Autogate::isEnabled(util::AutogateKey::RUST_ENTRYPOINT_SCHEDULED)) {
      return cxx.runScheduled(scheduledTime, cron);
    }
    // Not wrapped with the canceler, which is set only for an actor's events. The event is
    // cancelled by dropping the promise: that drops the Rust future, then the promise of
    // finishScheduled(), which owns the IncomingRequest.
    auto scheduled =
        scheduled_event_start(*entrypoint, scheduledTime, cron.asBytes().as<kj_rs::Rust>());
    return scheduled_event_finish(kj::mv(scheduled))
        .then([](::workerd::rust::worker::ScheduledResult result) {
      return kj::from<kj_rs::Rust>(result);
    });
  }

  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime, uint32_t retryCount) override {
    return cxx.runAlarm(scheduledTime, retryCount);
  }

  kj::Promise<kj::Maybe<kj::Date>> abandonAlarm(kj::Date scheduledTime) override {
    return cxx.abandonAlarm(scheduledTime);
  }

  kj::Promise<bool> test() override {
    return cxx.test();
  }

  kj::Promise<void> bench(
      bench::BenchParams::Reader params, bench::BenchReport::Group::Builder report) override {
    return cxx.bench(params, report);
  }

  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override {
    return cxx.customEvent(kj::mv(event));
  }

 private:
  ::rust::Box<WorkerEntrypoint> entrypoint;
  // Owned by `entrypoint`.
  CxxEntrypoint& cxx;
};

}  // namespace

kj::Own<WorkerInterface> newWorkerEntrypoint(kj::Own<CxxEntrypoint> cxx) {
  return kj::heap<RustWorkerEntrypoint>(worker_entrypoint_new(kj::mv(cxx)));
}

}  // namespace workerd::entrypoint
