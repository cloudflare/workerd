// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "trace.h"

#include <workerd/api/global-scope.h>
#include <workerd/api/http.h>
#include <workerd/api/util.h>
#include <workerd/io/io-context.h>
#include <capnp/schema.h>
#include <workerd/util/thread-scopes.h>
#include <workerd/util/own-util.h>

namespace workerd::api {

TraceEvent::TraceEvent(kj::ArrayPtr<kj::Own<Trace>> traces)
    : ExtendableEvent("trace"),
      traces(KJ_MAP(t, traces) -> jsg::Ref<TraceItem> {
        return jsg::alloc<TraceItem>(kj::addRef(*t));
      }) {}

kj::Array<jsg::Ref<TraceItem>> TraceEvent::getTraces() {
  return KJ_MAP(t, traces) -> jsg::Ref<TraceItem> { return t.addRef(); };
}

TraceItem::TraceItem(kj::Own<Trace> trace) : trace(kj::mv(trace)) {}

kj::Maybe<TraceItem::EventInfo> TraceItem::getEvent() {
  KJ_IF_MAYBE(e, trace->eventInfo) {
    KJ_SWITCH_ONEOF(*e) {
      KJ_CASE_ONEOF(fetch, Trace::FetchEventInfo) {
        return kj::Maybe(jsg::alloc<FetchEventInfo>(kj::addRef(*trace), fetch, trace->fetchResponseInfo));
      }
      KJ_CASE_ONEOF(scheduled, Trace::ScheduledEventInfo) {
        return kj::Maybe(jsg::alloc<ScheduledEventInfo>(kj::addRef(*trace), scheduled));
      }
      KJ_CASE_ONEOF(alarm, Trace::AlarmEventInfo) {
        return kj::Maybe(jsg::alloc<AlarmEventInfo>(kj::addRef(*trace), alarm));
      }
      KJ_CASE_ONEOF(queue, Trace::QueueEventInfo) {
        return kj::Maybe(jsg::alloc<QueueEventInfo>(kj::addRef(*trace), queue));
      }
      KJ_CASE_ONEOF(email, Trace::EmailEventInfo) {
        return kj::Maybe(jsg::alloc<EmailEventInfo>(kj::addRef(*trace), email));
      }
      KJ_CASE_ONEOF(custom, Trace::CustomEventInfo) {
        return kj::Maybe(jsg::alloc<CustomEventInfo>(kj::addRef(*trace), custom));
      }
    }
  }
  return nullptr;
}

kj::Maybe<double> TraceItem::getEventTimestamp() {
  if (trace->eventTimestamp == kj::UNIX_EPOCH) {
    return nullptr;
  } else {
    if (isPredictableModeForTest()) {
      return 0.0;
    } else {
      return (trace->eventTimestamp - kj::UNIX_EPOCH) / kj::MILLISECONDS;
    }
  }
}

kj::Array<jsg::Ref<TraceLog>> TraceItem::getLogs() {
  return KJ_MAP(x, trace->logs) -> jsg::Ref<TraceLog> {
    return jsg::alloc<TraceLog>(kj::addRef(*trace), x);
  };
}

kj::Array<jsg::Ref<TraceException>> TraceItem::getExceptions() {
  return KJ_MAP(x, trace->exceptions) -> jsg::Ref<TraceException> {
    return jsg::alloc<TraceException>(kj::addRef(*trace), x);
  };
}

uint TraceItem::getCpuTime() {
  return trace->cpuTime / kj::MILLISECONDS;
}

uint TraceItem::getWallTime() {
  return trace->wallTime / kj::MILLISECONDS;
}

TraceItem::FetchEventInfo::FetchEventInfo(kj::Own<Trace> trace, const Trace::FetchEventInfo& eventInfo,
                                          kj::Maybe<const Trace::FetchResponseInfo&> responseInfo)
    : trace(kj::mv(trace)), eventInfo(eventInfo), responseInfo(responseInfo) {}

jsg::Ref<TraceItem::FetchEventInfo::Request> TraceItem::FetchEventInfo::getRequest() {
  return jsg::alloc<Request>(kj::addRef(*trace), eventInfo);
}

jsg::Optional<jsg::Ref<TraceItem::FetchEventInfo::Response>> TraceItem::FetchEventInfo::getResponse() {
  KJ_IF_MAYBE(response, responseInfo) {
    return jsg::alloc<Response>(kj::addRef(*trace), *response);
  } else {
    return nullptr;
  }
}

TraceItem::FetchEventInfo::Request::Request(kj::Own<Trace> trace, const Trace::FetchEventInfo& eventInfo)
    : trace(kj::mv(trace)), eventInfo(eventInfo) {}

jsg::Optional<v8::Local<v8::Object>> TraceItem::FetchEventInfo::Request::getCf(
    v8::Isolate* isolate) {
  const auto& cfJson = eventInfo.cfJson;
  if (cfJson.size() > 0) {
    auto jsonString = jsg::v8Str(isolate, cfJson);
    auto handle = jsg::check(v8::JSON::Parse(isolate->GetCurrentContext(), jsonString));
    return handle.As<v8::Object>();
  }
  return nullptr;
}

jsg::Dict<jsg::ByteString, jsg::ByteString> TraceItem::FetchEventInfo::Request::getHeaders() {
  auto shouldRedact = [](kj::StringPtr name) {
    return (
      //(name == "authorization"_kj) || // covered below
      (name == "cookie"_kj) ||
      (name == "set-cookie"_kj) ||
      (strstr(name.cStr(), "auth") != nullptr) ||
      (strstr(name.cStr(), "jwt") != nullptr) ||
      (strstr(name.cStr(), "key") != nullptr) ||
      (strstr(name.cStr(), "secret") != nullptr) ||
      (strstr(name.cStr(), "token") != nullptr)
    );
  };

  using HeaderDict = jsg::Dict<jsg::ByteString, jsg::ByteString>;
  auto builder = kj::heapArrayBuilder<HeaderDict::Field>(eventInfo.headers.size());
  for (const auto& header: eventInfo.headers) {
    auto v = (redacted && shouldRedact(header.name)) ? "REDACTED"_kj : header.value;
    builder.add(HeaderDict::Field {
        jsg::ByteString(kj::str(header.name)),
        jsg::ByteString(kj::str(v))});
  }

  // TODO(conform): Better to return a frozen JS Object?
  return HeaderDict{ builder.finish() };
}

kj::StringPtr TraceItem::FetchEventInfo::Request::getMethod() {
  return kj::toCharSequence(eventInfo.method);
}

kj::String TraceItem::FetchEventInfo::Request::getUrl() {
  kj::StringPtr url = eventInfo.url;
  return (redacted ? redactUrl(url) : kj::str(url));
}

jsg::Ref<TraceItem::FetchEventInfo::Request> TraceItem::FetchEventInfo::Request::getUnredacted() {
  auto request = jsg::alloc<Request>(kj::addRef(*trace), eventInfo);
  request->redacted = false;
  return kj::mv(request);
}

TraceItem::FetchEventInfo::Response::Response(kj::Own<Trace> trace, const Trace::FetchResponseInfo& responseInfo)
    : trace(kj::mv(trace)), responseInfo(responseInfo) {}

uint16_t TraceItem::FetchEventInfo::Response::getStatus() {
  return responseInfo.statusCode;
}

TraceItem::ScheduledEventInfo::ScheduledEventInfo(kj::Own<Trace> trace,
    const Trace::ScheduledEventInfo& eventInfo) : trace(kj::mv(trace)), eventInfo(eventInfo) {}

double TraceItem::ScheduledEventInfo::getScheduledTime() {
  return eventInfo.scheduledTime;
}
kj::StringPtr TraceItem::ScheduledEventInfo::getCron() {
  return eventInfo.cron;
}

TraceItem::AlarmEventInfo::AlarmEventInfo(kj::Own<Trace> trace,
    const Trace::AlarmEventInfo& eventInfo) : trace(kj::mv(trace)), eventInfo(eventInfo) {}

kj::Date TraceItem::AlarmEventInfo::getScheduledTime() {
  return eventInfo.scheduledTime;
}

TraceItem::QueueEventInfo::QueueEventInfo(kj::Own<Trace> trace,
    const Trace::QueueEventInfo& eventInfo) : trace(kj::mv(trace)), eventInfo(eventInfo) {}

kj::StringPtr TraceItem::QueueEventInfo::getQueueName() {
  return eventInfo.queueName;
}

uint32_t TraceItem::QueueEventInfo::getBatchSize() {
  return eventInfo.batchSize;
}

TraceItem::EmailEventInfo::EmailEventInfo(kj::Own<Trace> trace,
    const Trace::EmailEventInfo& eventInfo) : trace(kj::mv(trace)), eventInfo(eventInfo) {}

kj::StringPtr TraceItem::EmailEventInfo::getMailFrom() {
  return eventInfo.mailFrom;
}

kj::StringPtr TraceItem::EmailEventInfo::getRcptTo() {
  return eventInfo.rcptTo;
}

uint32_t TraceItem::EmailEventInfo::getRawSize() {
  return eventInfo.rawSize;
}

TraceItem::CustomEventInfo::CustomEventInfo(kj::Own<Trace> trace,
    const Trace::CustomEventInfo& eventInfo) : trace(kj::mv(trace)), eventInfo(eventInfo) {}

kj::Maybe<kj::StringPtr> TraceItem::getScriptName() {
  return trace->scriptName;
}

jsg::Optional<kj::StringPtr> TraceItem::getDispatchNamespace() {
  return trace->dispatchNamespace;
}

jsg::Optional<kj::Array<kj::StringPtr>> TraceItem::getScriptTags() {
  if (trace->scriptTags.size() > 0) {
    return KJ_MAP(t, trace->scriptTags) -> kj::StringPtr { return t; };
  } else {
    return nullptr;
  }
}

kj::StringPtr TraceItem::getOutcome() {
  // TODO(cleanup): Add to enumToStr() to capnp?
  auto enums = capnp::Schema::from<EventOutcome>().getEnumerants();
  uint i = static_cast<uint>(trace->outcome);
  KJ_ASSERT(i < enums.size(), "invalid outcome");
  return enums[i].getProto().getName();
}

TraceLog::TraceLog(kj::Own<Trace> trace, const Trace::Log& log) : trace(kj::mv(trace)), log(log) {}

double TraceLog::getTimestamp() {
  if (isPredictableModeForTest()) {
    return 0;
  } else {
    return (log.timestamp - kj::UNIX_EPOCH) / kj::MILLISECONDS;
  }
}

kj::StringPtr TraceLog::getLevel() {
  switch (log.logLevel) {
    case LogLevel::DEBUG_: return "debug"_kj;
    case LogLevel::INFO: return "info"_kj;
    case LogLevel::LOG: return "log"_kj;
    case LogLevel::WARN: return "warn"_kj;
    case LogLevel::ERROR: return "error"_kj;
  }
  KJ_UNREACHABLE;
}

v8::Local<v8::Object> TraceLog::getMessage(v8::Isolate* isolate) {
  auto jsonString = jsg::v8Str(isolate, log.message);
  auto handle = jsg::check(v8::JSON::Parse(isolate->GetCurrentContext(), jsonString));
  return handle.As<v8::Object>();
}

TraceException::TraceException(kj::Own<Trace> trace, const Trace::Exception& exception)
    : trace(kj::mv(trace)), exception(exception) {}

double TraceException::getTimestamp() {
  if (isPredictableModeForTest()) {
    return 0;
  } else {
    return (exception.timestamp - kj::UNIX_EPOCH) / kj::MILLISECONDS;
  }
}

kj::StringPtr TraceException::getMessage() {
  return exception.message;
}

kj::StringPtr TraceException::getName() {
  return exception.name;
}

TraceMetrics::TraceMetrics(uint cpuTime, uint wallTime) : cpuTime(cpuTime), wallTime(wallTime) {}

jsg::Ref<TraceMetrics> UnsafeTraceMetrics::fromTrace(jsg::Ref<TraceItem> item) {
  return jsg::alloc<TraceMetrics>(item->getCpuTime(), item->getWallTime());
}

namespace {
kj::Promise<void> sendTracesToExportedHandler(
    kj::Own<IoContext::IncomingRequest> incomingRequest, kj::Maybe<kj::StringPtr> entrypointNamePtr,
    kj::ArrayPtr<kj::Own<Trace>> traces) {
  // Mark the request as delivered because we're about to run some JS.
  incomingRequest->delivered();

  auto& context = incomingRequest->getContext();
  auto& metrics = incomingRequest->getMetrics();

  // Add the actual JS as a wait until because the handler may be an event listener which can't
  // wait around for async resolution. We're relying on `drain()` below to persist `incomingRequest`
  // and its members until this task completes.
  auto entrypointName = entrypointNamePtr.map([](auto s) { return kj::str(s); });
  try {
    co_await context.run(
        [&context, traces=mapAddRef(traces), entrypointName=kj::mv(entrypointName)]
        (Worker::Lock& lock) mutable {
      auto handler = lock.getExportedHandler(entrypointName, context.getActor());
      lock.getGlobalScope().sendTraces(traces, lock, handler);
    });
  } catch (kj::Exception e) {
    // TODO(someday): We only report sendTraces() as failed for metrics/logging if the initial
    //   event handler throws an exception; we do not consider waitUntil(). But all async work done
    //   in a trace handler has to be done using waitUntil(). So, this seems wrong. Should we
    //   change it so any waitUntil() failure counts as an error? For that matter, arguably *all*
    //   event types should report failure if a waitUntil() throws?
    metrics.reportFailure(e);

    // Log JS exceptions (from the initial sendTraces() call) to the JS console, if fiddle is
    // attached. This also has the effect of logging internal errors to syslog. (Note that
    // exceptions that occur asynchronously while waiting for the context to drain will be
    // logged elsewhere.)
    context.logUncaughtExceptionAsync(UncaughtExceptionSource::TRACE_HANDLER, kj::mv(e));
  };

  co_await incomingRequest->drain();
}
}  // namespace

auto TraceCustomEventImpl::run(
    kj::Own<IoContext::IncomingRequest> incomingRequest, kj::Maybe<kj::StringPtr> entrypointNamePtr)
    -> kj::Promise<Result> {
  // Don't bother to wait around for the handler to run, just hand it off to the waitUntil tasks.
  waitUntilTasks.add(
      sendTracesToExportedHandler(kj::mv(incomingRequest), entrypointNamePtr, traces));

  return Result {
    .outcome = EventOutcome::OK,
  };
}

auto TraceCustomEventImpl::sendRpc(
      capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
      capnp::ByteStreamFactory& byteStreamFactory,
      kj::TaskSet& waitUntilTasks,
      workerd::rpc::EventDispatcher::Client dispatcher) -> kj::Promise<Result> {
  auto req = dispatcher.sendTracesRequest();
  auto out = req.initTraces(traces.size());
  for (auto i: kj::indices(traces)) {
    traces[i]->copyTo(out[i]);
  }

  waitUntilTasks.add(req.send().ignoreResult());

  // As long as we sent it, we consider the result to be okay.
  co_return Result {
    .outcome = workerd::EventOutcome::OK,
  };
}

}  // namespace workerd::api
