// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ffi.h"

#include <workerd/api/global-scope.h>
#include <workerd/io/entrypoint/ffi.rs.h>
#include <workerd/io/io-context/ffi.h>
#include <workerd/io/tracer.h>
#include <workerd/rust/jsg/ffi-inl.h>
#include <workerd/rust/worker/bridge.h>
#include <workerd/util/use-perfetto-categories.h>

namespace workerd::entrypoint {

namespace {

// The NUL-terminated string `bytes` holds with its NUL, for a name that Rust passes as the bytes
// of a `&'static CStr`. `what` names it in the exception for bytes that aren't one.
kj::StringPtr staticName(::rust::Slice<const kj::byte> bytes, kj::StringPtr what) {
  // A kj::StringPtr needs a NUL right after its text and none within it. A CStr's bytes with its
  // NUL meet both, but this doesn't rely on that.
  auto all = kj::arrayPtr(bytes.data(), bytes.size());
  KJ_REQUIRE(all.size() > 0 && all.back() == '\0', "not NUL-terminated", what);
  auto text = all.first(all.size() - 1);
  KJ_REQUIRE(text.findFirst('\0') == kj::none, "contains a NUL", what);
  return kj::StringPtr(text.asChars().begin(), text.size());
}

}  // namespace

void invocation_set_custom_event_info(Invocation& invocation) {
  KJ_IF_SOME(tracer, invocation.request->getWorkerTracer()) {
    tracer.setEventInfo(*invocation.request, tracing::CustomEventInfo());
  }
}

void invocation_set_scheduled_event_info(
    Invocation& invocation, kj::Date scheduledTime, ::rust::Slice<const kj::byte> cron) {
  KJ_IF_SOME(tracer, invocation.request->getWorkerTracer()) {
    double eventTime = (scheduledTime - kj::UNIX_EPOCH) / kj::MILLISECONDS;
    tracer.setEventInfo(*invocation.request,
        tracing::ScheduledEventInfo(eventTime, kj::str(kj::from<kj_rs::Rust>(cron).asChars())));
  }
}

void invocation_delivered(Invocation& invocation, ::rust::Slice<const kj::byte> eventType) {
  invocation.request->delivered(staticName(eventType, "the event type"));
}

void invocation_drain(kj::Own<Invocation> invocation) {
  KJ_IF_SOME(exception, kj::runCatchingExceptions([&]() {
    invocation->request->drain(invocation->waitUntilTasks, kj::mv(invocation->request));
  })) {
    KJ_LOG(ERROR, "draining an entrypoint invocation failed", exception);
  }
}

void invocation_run_in_wait_until(Invocation& invocation,
    ::rust::Slice<const kj::byte> spanName,
    ::rust::Box<HandlerCallback> callback,
    ::rust::Box<WaitUntilContinuation> then) {
  auto name = staticName(spanName, "the span name");
  auto& context = invocation.request->getContext();
  context.addWaitUntil(
      context
          .run([name, entrypointName = invocation.entrypointName.map([](kj::String& name) {
    return kj::str(name);
  }),
                   versionInfo = kj::mv(invocation.versionInfo), props = kj::mv(invocation.props),
                   callback = kj::mv(callback)](Worker::Lock& lock, IoContext& context) mutable {
    // Used only in Perfetto builds.
    (void)name;
    TRACE_EVENT("workerd", perfetto::StaticString(name.cStr()));
    auto traceScope = context.makeRequestTraceScope(lock);
    auto handles = getHandlerHandles(lock, context,
        entrypointName.map([](kj::String& name) -> kj::StringPtr { return name; }),
        kj::mv(versionInfo), kj::mv(props));
    auto isolate = reinterpret_cast<uintptr_t>(lock.getIsolate());
    // A KJ exception that the callback failed with is thrown on from here.
    auto outcome =
        handler_callback_run(isolate, kj::mv(callback), rust::jsg::to_ffi(kj::mv(handles.handler)),
            rust::jsg::to_ffi(kj::mv(handles.env)), rust::jsg::to_ffi(kj::mv(handles.ctx)));
    io_context::endRunOfIoCallback(outcome, "the callback of an entrypoint event");
  }).then([then = kj::mv(then)]() mutable { return wait_until_continuation_wait(kj::mv(then)); }));
}

void invocation_start_scheduled_in_wait_until(
    Invocation& invocation, kj::Date scheduledTime, ::rust::Slice<const kj::byte> cron) {
  auto& context = invocation.request->getContext();
  // Scheduled handlers run entirely in waitUntil() tasks.
  context.addWaitUntil(context.run(
      [scheduledTime, cron = kj::str(kj::from<kj_rs::Rust>(cron).asChars()),
          entrypointName =
              invocation.entrypointName.map([](kj::String& name) { return kj::str(name); }),
          versionInfo = kj::mv(invocation.versionInfo),
          props = kj::mv(invocation.props)](Worker::Lock& lock, IoContext& context) mutable {
    TRACE_EVENT("workerd", "WorkerEntrypoint::runScheduled() run");
    auto traceScope = context.makeRequestTraceScope(lock);

    lock.getGlobalScope().startScheduled(scheduledTime, cron, lock,
        lock.getExportedHandler(
            entrypointName.map([](kj::String& name) -> kj::StringPtr { return name; }),
            kj::mv(versionInfo), kj::mv(props), context.getActor()));
  }));
}

kj::Promise<::workerd::rust::worker::ScheduledResult> invocation_finish_scheduled(
    kj::Own<Invocation> invocation, ::rust::Slice<const kj::byte> spanName) {
  auto name = staticName(spanName, "the span name");
  // Used only in Perfetto builds.
  (void)name;
  TRACE_EVENT("workerd", perfetto::StaticString(name.cStr()));
  auto& request = *invocation->request;
  return request.finishScheduled(kj::mv(invocation->request))
      .then([](WorkerInterface::ScheduledResult result) {
    return ::workerd::rust::worker::ScheduledResult{
      .retry = result.retry,
      .outcome = ::workerd::rust::worker::toRustOutcome(result.outcome),
    };
  });
}

kj::Promise<WorkerInterface::ScheduledResult> runScheduled(
    kj::Own<Invocation> invocation, kj::Date scheduledTime, kj::StringPtr cron) {
  // Only the Rust path has this span, so a trace shows which path delivered the event.
  TRACE_EVENT("workerd", "entrypoint::runScheduled()");
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
