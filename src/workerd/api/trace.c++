// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "trace.h"

#include <workerd/api/global-scope.h>
#include <workerd/api/http.h>
#include <workerd/api/util.h>
#include <workerd/io/io-context.h>
#include <workerd/jsg/ser.h>
#include <capnp/schema.h>
#include <workerd/util/thread-scopes.h>
#include <workerd/util/own-util.h>

namespace workerd::api {

TailEvent::TailEvent(jsg::Lock& js, kj::StringPtr type, kj::ArrayPtr<kj::Own<Trace>> events)
    : ExtendableEvent(kj::str(type)),
      events(KJ_MAP(e, events) -> jsg::Ref<TraceItem> {
        return jsg::alloc<TraceItem>(js, *e);
      }) {}

kj::Array<jsg::Ref<TraceItem>> TailEvent::getEvents() {
  return KJ_MAP(e, events) -> jsg::Ref<TraceItem> { return e.addRef(); };
}

namespace {
kj::Maybe<double> getTraceTimestamp(const Trace& trace) {
  if (trace.eventTimestamp == kj::UNIX_EPOCH) {
    return nullptr;
  }
  if (isPredictableModeForTest()) {
    return 0.0;
  }
  return (trace.eventTimestamp - kj::UNIX_EPOCH) / kj::MILLISECONDS;
}

double getTraceLogTimestamp(const Trace::Log& log) {
  if (isPredictableModeForTest()) {
    return 0;
  } else {
    return (log.timestamp - kj::UNIX_EPOCH) / kj::MILLISECONDS;
  }
}

double getTraceDiagnosticChannelEventTimestamp(const Trace::DiagnosticChannelEvent& event) {
  if (isPredictableModeForTest()) {
    return 0;
  } else {
    return (event.timestamp - kj::UNIX_EPOCH) / kj::MILLISECONDS;
  }
}

kj::String getTraceLogLevel(const Trace::Log& log) {
    switch (log.logLevel) {
    case LogLevel::DEBUG_: return kj::str("debug");
    case LogLevel::INFO: return kj::str("info");
    case LogLevel::LOG: return kj::str("log");
    case LogLevel::WARN: return kj::str("warn");
    case LogLevel::ERROR: return kj::str("error");
  }
  KJ_UNREACHABLE;
}

jsg::V8Ref<v8::Object> getTraceLogMessage(jsg::Lock& js, const Trace::Log& log) {
  return js.parseJson(log.message).cast<v8::Object>(js);
}

kj::Array<jsg::Ref<TraceLog>> getTraceLogs(jsg::Lock& js, const Trace& trace) {
  return KJ_MAP(x, trace.logs) -> jsg::Ref<TraceLog> {
    return jsg::alloc<TraceLog>(js, trace, x);
  };
}

kj::Array<jsg::Ref<TraceDiagnosticChannelEvent>> getTraceDiagnosticChannelEvents(
    jsg::Lock& js, const Trace& trace) {
  return KJ_MAP(x, trace.diagnosticChannelEvents) -> jsg::Ref<TraceDiagnosticChannelEvent> {
    return jsg::alloc<TraceDiagnosticChannelEvent>(trace, x);
  };
}

double getTraceExceptionTimestamp(const Trace::Exception& ex) {
  if (isPredictableModeForTest()) {
    return 0;
  } else {
    return (ex.timestamp - kj::UNIX_EPOCH) / kj::MILLISECONDS;
  }
}

kj::Array<jsg::Ref<TraceException>> getTraceExceptions(const Trace& trace) {
  return KJ_MAP(x, trace.exceptions) -> jsg::Ref<TraceException> {
    return jsg::alloc<TraceException>(trace, x);
  };
}

jsg::Optional<kj::Array<kj::String>> getTraceScriptTags(const Trace& trace) {
  if (trace.scriptTags.size() > 0) {
    return KJ_MAP(t, trace.scriptTags) -> kj::String { return kj::str(t); };
  } else {
    return nullptr;
  }
}

kj::String getTraceOutcome(const Trace& trace) {
  // TODO(cleanup): Add to enumToStr() to capnp?
  auto enums = capnp::Schema::from<EventOutcome>().getEnumerants();
  uint i = static_cast<uint>(trace.outcome);
  KJ_ASSERT(i < enums.size(), "invalid outcome");
  return kj::str(enums[i].getProto().getName());
}

kj::Own<TraceItem::FetchEventInfo::Request::Detail> getFetchRequestDetail(
    jsg::Lock& js,
    const Trace& trace,
    const Trace::FetchEventInfo& eventInfo) {
  const auto getCf = [&]() -> jsg::Optional<jsg::V8Ref<v8::Object>> {
    const auto& cfJson = eventInfo.cfJson;
    if (cfJson.size() > 0) {
      return js.parseJson(cfJson).cast<v8::Object>(js);
    }
    return nullptr;
  };

  const auto getHeaders = [&]() -> kj::Array<Trace::FetchEventInfo::Header> {
    return KJ_MAP(header, eventInfo.headers) {
      return Trace::FetchEventInfo::Header(kj::str(header.name), kj::str(header.value));
    };
  };

  return kj::refcounted<TraceItem::FetchEventInfo::Request::Detail>(
    getCf(),
    getHeaders(),
    kj::str(eventInfo.method),
    kj::str(eventInfo.url));
}

kj::Maybe<TraceItem::EventInfo> getTraceEvent(jsg::Lock& js, const Trace& trace) {
  KJ_IF_MAYBE(e, trace.eventInfo) {
    KJ_SWITCH_ONEOF(*e) {
      KJ_CASE_ONEOF(fetch, Trace::FetchEventInfo) {
        return kj::Maybe(jsg::alloc<TraceItem::FetchEventInfo>(js, trace, fetch, trace.fetchResponseInfo));
      }
      KJ_CASE_ONEOF(scheduled, Trace::ScheduledEventInfo) {
        return kj::Maybe(jsg::alloc<TraceItem::ScheduledEventInfo>(trace, scheduled));
      }
      KJ_CASE_ONEOF(alarm, Trace::AlarmEventInfo) {
        return kj::Maybe(jsg::alloc<TraceItem::AlarmEventInfo>(trace, alarm));
      }
      KJ_CASE_ONEOF(queue, Trace::QueueEventInfo) {
        return kj::Maybe(jsg::alloc<TraceItem::QueueEventInfo>(trace, queue));
      }
      KJ_CASE_ONEOF(email, Trace::EmailEventInfo) {
        return kj::Maybe(jsg::alloc<TraceItem::EmailEventInfo>(trace, email));
      }
      KJ_CASE_ONEOF(custom, Trace::CustomEventInfo) {
        return kj::Maybe(jsg::alloc<TraceItem::CustomEventInfo>(trace, custom));
      }
    }
  }
  return nullptr;
}
}  // namespace

TraceItem::TraceItem(jsg::Lock& js, const Trace& trace)
    : eventInfo(getTraceEvent(js, trace)),
      eventTimestamp(getTraceTimestamp(trace)),
      logs(getTraceLogs(js, trace)),
      exceptions(getTraceExceptions(trace)),
      diagnosticChannelEvents(getTraceDiagnosticChannelEvents(js, trace)),
      scriptName(trace.scriptName.map([](auto& name) { return kj::str(name); })),
      dispatchNamespace(trace.dispatchNamespace.map([](auto& ns) { return kj::str(ns); })),
      scriptTags(getTraceScriptTags(trace)),
      outcome(getTraceOutcome(trace)),
      cpuTime(trace.cpuTime / kj::MILLISECONDS),
      wallTime(trace.wallTime / kj::MILLISECONDS) {}

kj::Maybe<TraceItem::EventInfo> TraceItem::getEvent(jsg::Lock& js) {
  return eventInfo.map([](auto& info) -> TraceItem::EventInfo {
    KJ_SWITCH_ONEOF(info) {
      KJ_CASE_ONEOF(info, jsg::Ref<FetchEventInfo>) { return info.addRef(); }
      KJ_CASE_ONEOF(info, jsg::Ref<ScheduledEventInfo>) { return info.addRef(); }
      KJ_CASE_ONEOF(info, jsg::Ref<AlarmEventInfo>) { return info.addRef(); }
      KJ_CASE_ONEOF(info, jsg::Ref<QueueEventInfo>) { return info.addRef(); }
      KJ_CASE_ONEOF(info, jsg::Ref<EmailEventInfo>) { return info.addRef(); }
      KJ_CASE_ONEOF(info, jsg::Ref<CustomEventInfo>) { return info.addRef(); }
    }
    KJ_UNREACHABLE;
  });
}

kj::Maybe<double> TraceItem::getEventTimestamp() {
  return eventTimestamp;
}

kj::ArrayPtr<jsg::Ref<TraceLog>> TraceItem::getLogs() {
  return logs;
}

kj::ArrayPtr<jsg::Ref<TraceException>> TraceItem::getExceptions() {
  return exceptions;
}

kj::ArrayPtr<jsg::Ref<TraceDiagnosticChannelEvent>> TraceItem::getDiagnosticChannelEvents() {
  return diagnosticChannelEvents;
}

kj::Maybe<kj::StringPtr> TraceItem::getScriptName() {
  return scriptName.map([](auto& name) -> kj::StringPtr { return name; });
}

jsg::Optional<kj::StringPtr> TraceItem::getDispatchNamespace() {
  return dispatchNamespace.map([](auto& ns) -> kj::StringPtr { return ns; });
}

jsg::Optional<kj::Array<kj::StringPtr>> TraceItem::getScriptTags() {
  return scriptTags.map([](kj::Array<kj::String>& tags) {
    return KJ_MAP(t, tags) -> kj::StringPtr { return t; };
  });
}

kj::StringPtr TraceItem::getOutcome() { return outcome; }

uint TraceItem::getCpuTime() { return cpuTime; }

uint TraceItem::getWallTime() { return wallTime; }

TraceItem::FetchEventInfo::FetchEventInfo(jsg::Lock& js,
                                          const Trace& trace,
                                          const Trace::FetchEventInfo& eventInfo,
                                          kj::Maybe<const Trace::FetchResponseInfo&> responseInfo)
    : request(jsg::alloc<Request>(js, trace, eventInfo)),
      response(responseInfo.map([&](auto& info) { return jsg::alloc<Response>(trace, info); })) {}

TraceItem::FetchEventInfo::Request::Detail::Detail(
    jsg::Optional<jsg::V8Ref<v8::Object>> cf,
    kj::Array<Trace::FetchEventInfo::Header> headers,
    kj::String method,
    kj::String url)
    : cf(kj::mv(cf)),
      headers(kj::mv(headers)),
      method(kj::mv(method)),
      url(kj::mv(url)) {}

jsg::Ref<TraceItem::FetchEventInfo::Request> TraceItem::FetchEventInfo::getRequest() {
  return request.addRef();
}

jsg::Optional<jsg::Ref<TraceItem::FetchEventInfo::Response>> TraceItem::FetchEventInfo::getResponse() {
  return response.map([](auto& ref) mutable -> jsg::Ref<TraceItem::FetchEventInfo::Response> {
    return ref.addRef();
  });
}

TraceItem::FetchEventInfo::Request::Request(jsg::Lock& js,
                                            const Trace& trace,
                                            const Trace::FetchEventInfo& eventInfo)
    : detail(getFetchRequestDetail(js, trace, eventInfo)) {}

TraceItem::FetchEventInfo::Request::Request(Detail& detail, bool redacted)
    : redacted(redacted), detail(kj::addRef(detail)) {}

jsg::Optional<jsg::V8Ref<v8::Object>> TraceItem::FetchEventInfo::Request::getCf(jsg::Lock& js) {
  return detail->cf.map([&](jsg::V8Ref<v8::Object>& obj) {
    return obj.addRef(js);
  });
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
  auto builder = kj::heapArrayBuilder<HeaderDict::Field>(detail->headers.size());
  for (const auto& header: detail->headers) {
    auto v = (redacted && shouldRedact(header.name)) ? "REDACTED"_kj : header.value;
    builder.add(HeaderDict::Field {
        jsg::ByteString(kj::str(header.name)),
        jsg::ByteString(kj::str(v))});
  }

  // TODO(conform): Better to return a frozen JS Object?
  return HeaderDict{ builder.finish() };
}

kj::StringPtr TraceItem::FetchEventInfo::Request::getMethod() {
  return detail->method;
}

kj::String TraceItem::FetchEventInfo::Request::getUrl() {
  return (redacted ? redactUrl(detail->url) : kj::str(detail->url));
}

jsg::Ref<TraceItem::FetchEventInfo::Request> TraceItem::FetchEventInfo::Request::getUnredacted() {
  return jsg::alloc<Request>(*detail, false /* details are not redacted */);
}

TraceItem::FetchEventInfo::Response::Response(const Trace& trace,
                                              const Trace::FetchResponseInfo& responseInfo)
    : status(responseInfo.statusCode) {}

uint16_t TraceItem::FetchEventInfo::Response::getStatus() {
  return status;
}

TraceItem::ScheduledEventInfo::ScheduledEventInfo(const Trace& trace,
                                                  const Trace::ScheduledEventInfo& eventInfo)
    : scheduledTime(eventInfo.scheduledTime),
      cron(kj::str(eventInfo.cron)) {}

double TraceItem::ScheduledEventInfo::getScheduledTime() {
  return scheduledTime;
}
kj::StringPtr TraceItem::ScheduledEventInfo::getCron() {
  return cron;
}

TraceItem::AlarmEventInfo::AlarmEventInfo(const Trace& trace,
                                          const Trace::AlarmEventInfo& eventInfo)
  : scheduledTime(eventInfo.scheduledTime) {}

kj::Date TraceItem::AlarmEventInfo::getScheduledTime() {
  return scheduledTime;
}

TraceItem::QueueEventInfo::QueueEventInfo(const Trace& trace,
                                          const Trace::QueueEventInfo& eventInfo)
    : queueName(kj::str(eventInfo.queueName)),
      batchSize(eventInfo.batchSize) {}

kj::StringPtr TraceItem::QueueEventInfo::getQueueName() {
  return queueName;
}

uint32_t TraceItem::QueueEventInfo::getBatchSize() {
  return batchSize;
}

TraceItem::EmailEventInfo::EmailEventInfo(const Trace& trace,
                                          const Trace::EmailEventInfo& eventInfo)
    : mailFrom(kj::str(eventInfo.mailFrom)),
      rcptTo(kj::str(eventInfo.rcptTo)),
      rawSize(eventInfo.rawSize) {}

kj::StringPtr TraceItem::EmailEventInfo::getMailFrom() {
  return mailFrom;
}

kj::StringPtr TraceItem::EmailEventInfo::getRcptTo() {
  return rcptTo;
}

uint32_t TraceItem::EmailEventInfo::getRawSize() {
  return rawSize;
}

TraceDiagnosticChannelEvent::TraceDiagnosticChannelEvent(
    const Trace& trace,
    const Trace::DiagnosticChannelEvent& eventInfo)
    : timestamp(getTraceDiagnosticChannelEventTimestamp(eventInfo)),
      channel(kj::heapString(eventInfo.channel)),
      message(kj::heapArray<kj::byte>(eventInfo.message)) {}

kj::StringPtr TraceDiagnosticChannelEvent::getChannel() {
  return channel;
}

jsg::JsValue TraceDiagnosticChannelEvent::getMessage(jsg::Lock& js) {
  if (message.size() == 0) return js.undefined();
  jsg::Deserializer des(js, message.asPtr());
  return des.readValue(js);
}

double TraceDiagnosticChannelEvent::getTimestamp() { return timestamp; }

TraceItem::CustomEventInfo::CustomEventInfo(const Trace& trace,
                                            const Trace::CustomEventInfo& eventInfo)
    : eventInfo(eventInfo) {}

TraceLog::TraceLog(jsg::Lock& js, const Trace& trace, const Trace::Log& log)
    : timestamp(getTraceLogTimestamp(log)),
      level(getTraceLogLevel(log)),
      message(getTraceLogMessage(js, log)) {}

double TraceLog::getTimestamp() {
  return timestamp;
}

kj::StringPtr TraceLog::getLevel() {
  return level;
}

jsg::V8Ref<v8::Object> TraceLog::getMessage(jsg::Lock& js) {
  return message.addRef(js);
}

TraceException::TraceException(const Trace& trace, const Trace::Exception& exception)
    : timestamp(getTraceExceptionTimestamp(exception)),
      name(kj::str(exception.name)),
      message(kj::str(exception.message)) {}

double TraceException::getTimestamp() {
  return timestamp;
}

kj::StringPtr TraceException::getMessage() {
  return message;
}

kj::StringPtr TraceException::getName() {
  return name;
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
      jsg::AsyncContextFrame::StorageScope traceScope = context.makeAsyncTraceScope(lock);

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
