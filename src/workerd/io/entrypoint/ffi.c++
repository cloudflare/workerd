// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ffi.h"

#include <workerd/io/entrypoint/ffi.rs.h>
#include <workerd/rust/worker/bridge.h>

namespace workerd::entrypoint {

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

kj::Promise<WorkerInterface::ScheduledResult> runScheduled(
    kj::Own<Invocation> invocation, kj::Date scheduledTime, kj::StringPtr cron) {
  auto scheduled =
      scheduled_event_start(kj::mv(invocation), scheduledTime, cron.asBytes().as<kj_rs::Rust>());
  return scheduled_event_finish(kj::mv(scheduled))
      .then([](::workerd::rust::worker::ScheduledResult result) {
    return kj::from<kj_rs::Rust>(result);
  });
}

kj::Promise<bool> runTest(kj::Own<Invocation> invocation) {
  return test_event_finish(test_event_start(kj::mv(invocation)));
}

}  // namespace workerd::entrypoint
