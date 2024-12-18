// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "trace.h"

#include <workerd/api/global-scope.h>
#include <workerd/api/http.h>
#include <workerd/api/util.h>
#include <workerd/io/io-context.h>
#include <workerd/jsg/ser.h>
#include <workerd/util/own-util.h>
#include <workerd/util/thread-scopes.h>
#include <workerd/util/uncaught-exception-source.h>
#include <workerd/util/uuid.h>

#include <capnp/schema.h>
#include <kj/encoding.h>

namespace workerd::api {

TailEvent::TailEvent(jsg::Lock& js, kj::StringPtr type, kj::ArrayPtr<kj::Own<Trace>> events)
    : ExtendableEvent(kj::str(type)),
      events(KJ_MAP(e, events) -> jsg::Ref<TraceItem> { return jsg::alloc<TraceItem>(js, *e); }) {}

kj::Array<jsg::Ref<TraceItem>> TailEvent::getEvents() {
  return KJ_MAP(e, events) -> jsg::Ref<TraceItem> { return e.addRef(); };
}

namespace {
kj::Maybe<double> getTraceTimestamp(const Trace& trace) {
  if (trace.eventTimestamp == kj::UNIX_EPOCH) {
    return kj::none;
  }
  if (isPredictableModeForTest()) {
    return 0.0;
  }
  return (trace.eventTimestamp - kj::UNIX_EPOCH) / kj::MILLISECONDS;
}

double getTraceLogTimestamp(const tracing::Log& log) {
  if (isPredictableModeForTest()) {
    return 0;
  } else {
    return (log.timestamp - kj::UNIX_EPOCH) / kj::MILLISECONDS;
  }
}

double getTraceDiagnosticChannelEventTimestamp(const tracing::DiagnosticChannelEvent& event) {
  if (isPredictableModeForTest()) {
    return 0;
  } else {
    return (event.timestamp - kj::UNIX_EPOCH) / kj::MILLISECONDS;
  }
}

kj::String getTraceLogLevel(const tracing::Log& log) {
  switch (log.logLevel) {
    case LogLevel::DEBUG_:
      return kj::str("debug");
    case LogLevel::INFO:
      return kj::str("info");
    case LogLevel::LOG:
      return kj::str("log");
    case LogLevel::WARN:
      return kj::str("warn");
    case LogLevel::ERROR:
      return kj::str("error");
  }
  KJ_UNREACHABLE;
}

jsg::V8Ref<v8::Object> getTraceLogMessage(jsg::Lock& js, const tracing::Log& log) {
  return js.parseJson(log.message).cast<v8::Object>(js);
}

kj::Array<jsg::Ref<TraceLog>> getTraceLogs(jsg::Lock& js, const Trace& trace) {
  return KJ_MAP(x, trace.logs) -> jsg::Ref<TraceLog> { return jsg::alloc<TraceLog>(js, trace, x); };
}

kj::Array<jsg::Ref<OTelSpan>> getTraceSpans(const Trace& trace) {
  return KJ_MAP(x, trace.spans) -> jsg::Ref<OTelSpan> { return jsg::alloc<OTelSpan>(x); };
}

kj::Array<jsg::Ref<TraceDiagnosticChannelEvent>> getTraceDiagnosticChannelEvents(
    jsg::Lock& js, const Trace& trace) {
  return KJ_MAP(x, trace.diagnosticChannelEvents) -> jsg::Ref<TraceDiagnosticChannelEvent> {
    return jsg::alloc<TraceDiagnosticChannelEvent>(trace, x);
  };
}

kj::Maybe<ScriptVersion> getTraceScriptVersion(const Trace& trace) {
  return trace.scriptVersion.map([](const auto& version) { return ScriptVersion(*version); });
}

double getTraceExceptionTimestamp(const tracing::Exception& ex) {
  if (isPredictableModeForTest()) {
    return 0;
  } else {
    return (ex.timestamp - kj::UNIX_EPOCH) / kj::MILLISECONDS;
  }
}

kj::Array<jsg::Ref<TraceException>> getTraceExceptions(const Trace& trace) {
  return KJ_MAP(x, trace.exceptions) -> jsg::Ref<TraceException> { return jsg::alloc<TraceException>(trace, x); };
}

jsg::Optional<kj::Array<kj::String>> getTraceScriptTags(const Trace& trace) {
  if (trace.scriptTags.size() > 0) {
    return KJ_MAP(t, trace.scriptTags) -> kj::String { return kj::str(t); };
  } else {
    return kj::none;
  }
}

template <typename Enum>
kj::String enumToStr(const Enum& var) {
  // TODO(cleanup): Port this to capnproto.
  auto enums = capnp::Schema::from<Enum>().getEnumerants();
  uint i = static_cast<uint>(var);
  KJ_ASSERT(i < enums.size(), "invalid enum value");
  return kj::str(enums[i].getProto().getName());
}

kj::Own<TraceItem::FetchEventInfo::Request::Detail> getFetchRequestDetail(
    jsg::Lock& js, const Trace& trace, const tracing::FetchEventInfo& eventInfo) {
  const auto getCf = [&]() -> jsg::Optional<jsg::V8Ref<v8::Object>> {
    const auto& cfJson = eventInfo.cfJson;
    if (cfJson.size() > 0) {
      return js.parseJson(cfJson).cast<v8::Object>(js);
    }
    return kj::none;
  };

  const auto getHeaders = [&]() -> kj::Array<tracing::FetchEventInfo::Header> {
    return KJ_MAP(header, eventInfo.headers) {
      return tracing::FetchEventInfo::Header(kj::str(header.name), kj::str(header.value));
    };
  };

  return kj::refcounted<TraceItem::FetchEventInfo::Request::Detail>(
      getCf(), getHeaders(), kj::str(eventInfo.method), kj::str(eventInfo.url));
}

kj::Maybe<TraceItem::EventInfo> getTraceEvent(jsg::Lock& js, const Trace& trace) {
  KJ_IF_SOME(e, trace.eventInfo) {
    KJ_SWITCH_ONEOF(e) {
      KJ_CASE_ONEOF(fetch, tracing::FetchEventInfo) {
        return kj::Maybe(
            jsg::alloc<TraceItem::FetchEventInfo>(js, trace, fetch, trace.fetchResponseInfo));
      }
      KJ_CASE_ONEOF(jsRpc, tracing::JsRpcEventInfo) {
        return kj::Maybe(jsg::alloc<TraceItem::JsRpcEventInfo>(trace, jsRpc));
      }
      KJ_CASE_ONEOF(scheduled, tracing::ScheduledEventInfo) {
        return kj::Maybe(jsg::alloc<TraceItem::ScheduledEventInfo>(trace, scheduled));
      }
      KJ_CASE_ONEOF(alarm, tracing::AlarmEventInfo) {
        return kj::Maybe(jsg::alloc<TraceItem::AlarmEventInfo>(trace, alarm));
      }
      KJ_CASE_ONEOF(queue, tracing::QueueEventInfo) {
        return kj::Maybe(jsg::alloc<TraceItem::QueueEventInfo>(trace, queue));
      }
      KJ_CASE_ONEOF(email, tracing::EmailEventInfo) {
        return kj::Maybe(jsg::alloc<TraceItem::EmailEventInfo>(trace, email));
      }
      KJ_CASE_ONEOF(tracedTrace, tracing::TraceEventInfo) {
        return kj::Maybe(jsg::alloc<TraceItem::TailEventInfo>(trace, tracedTrace));
      }
      KJ_CASE_ONEOF(hibWs, tracing::HibernatableWebSocketEventInfo) {
        KJ_SWITCH_ONEOF(hibWs.type) {
          KJ_CASE_ONEOF(message, tracing::HibernatableWebSocketEventInfo::Message) {
            return kj::Maybe(jsg::alloc<TraceItem::HibernatableWebSocketEventInfo>(trace, message));
          }
          KJ_CASE_ONEOF(close, tracing::HibernatableWebSocketEventInfo::Close) {
            return kj::Maybe(jsg::alloc<TraceItem::HibernatableWebSocketEventInfo>(trace, close));
          }
          KJ_CASE_ONEOF(error, tracing::HibernatableWebSocketEventInfo::Error) {
            return kj::Maybe(jsg::alloc<TraceItem::HibernatableWebSocketEventInfo>(trace, error));
          }
        }
        KJ_UNREACHABLE;
      }
      KJ_CASE_ONEOF(resume, tracing::Resume) {
        // Resume events are not used with legacy trace
        KJ_UNREACHABLE;
      }
      KJ_CASE_ONEOF(custom, tracing::CustomEventInfo) {
        return kj::Maybe(jsg::alloc<TraceItem::CustomEventInfo>(trace, custom));
      }
    }
  }
  return kj::none;
}
}  // namespace

TraceItem::TraceItem(jsg::Lock& js, const Trace& trace)
    : eventInfo(getTraceEvent(js, trace)),
      eventTimestamp(getTraceTimestamp(trace)),
      logs(getTraceLogs(js, trace)),
      exceptions(getTraceExceptions(trace)),
      diagnosticChannelEvents(getTraceDiagnosticChannelEvents(js, trace)),
      scriptName(trace.scriptName.map([](auto& name) { return kj::str(name); })),
      entrypoint(trace.entrypoint.map([](auto& name) { return kj::str(name); })),
      scriptVersion(getTraceScriptVersion(trace)),
      dispatchNamespace(trace.dispatchNamespace.map([](auto& ns) { return kj::str(ns); })),
      scriptTags(getTraceScriptTags(trace)),
      executionModel(enumToStr(trace.executionModel)),
      spans(getTraceSpans(trace)),
      outcome(enumToStr(trace.outcome)),
      cpuTime(trace.cpuTime / kj::MILLISECONDS),
      wallTime(trace.wallTime / kj::MILLISECONDS),
      truncated(trace.truncated) {}

kj::Maybe<TraceItem::EventInfo> TraceItem::getEvent(jsg::Lock& js) {
  return eventInfo.map([](auto& info) -> TraceItem::EventInfo {
    KJ_SWITCH_ONEOF(info) {
      KJ_CASE_ONEOF(info, jsg::Ref<FetchEventInfo>) {
        return info.addRef();
      }
      KJ_CASE_ONEOF(info, jsg::Ref<JsRpcEventInfo>) {
        return info.addRef();
      }
      KJ_CASE_ONEOF(info, jsg::Ref<ScheduledEventInfo>) {
        return info.addRef();
      }
      KJ_CASE_ONEOF(info, jsg::Ref<AlarmEventInfo>) {
        return info.addRef();
      }
      KJ_CASE_ONEOF(info, jsg::Ref<QueueEventInfo>) {
        return info.addRef();
      }
      KJ_CASE_ONEOF(info, jsg::Ref<EmailEventInfo>) {
        return info.addRef();
      }
      KJ_CASE_ONEOF(info, jsg::Ref<TailEventInfo>) {
        return info.addRef();
      }
      KJ_CASE_ONEOF(info, jsg::Ref<HibernatableWebSocketEventInfo>) {
        return info.addRef();
      }
      KJ_CASE_ONEOF(info, jsg::Ref<CustomEventInfo>) {
        return info.addRef();
      }
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

jsg::Optional<kj::StringPtr> TraceItem::getEntrypoint() {
  return entrypoint;
}

jsg::Optional<ScriptVersion> TraceItem::getScriptVersion() {
  return scriptVersion;
}

jsg::Optional<kj::StringPtr> TraceItem::getDispatchNamespace() {
  return dispatchNamespace.map([](auto& ns) -> kj::StringPtr { return ns; });
}

jsg::Optional<kj::Array<kj::StringPtr>> TraceItem::getScriptTags() {
  return scriptTags.map(
      [](kj::Array<kj::String>& tags) { return KJ_MAP(t, tags) -> kj::StringPtr { return t; }; });
}

kj::StringPtr TraceItem::getExecutionModel() {
  return executionModel;
}

kj::ArrayPtr<jsg::Ref<OTelSpan>> TraceItem::getSpans() {
  return spans;
}

kj::StringPtr TraceItem::getOutcome() {
  return outcome;
}

bool TraceItem::getTruncated() {
  return truncated;
}

uint TraceItem::getCpuTime() {
  return cpuTime;
}

uint TraceItem::getWallTime() {
  return wallTime;
}

TraceItem::FetchEventInfo::FetchEventInfo(jsg::Lock& js,
    const Trace& trace,
    const tracing::FetchEventInfo& eventInfo,
    kj::Maybe<const tracing::FetchResponseInfo&> responseInfo)
    : request(jsg::alloc<Request>(js, trace, eventInfo)),
      response(responseInfo.map([&](auto& info) { return jsg::alloc<Response>(trace, info); })) {}

TraceItem::FetchEventInfo::Request::Detail::Detail(jsg::Optional<jsg::V8Ref<v8::Object>> cf,
    kj::Array<tracing::FetchEventInfo::Header> headers,
    kj::String method,
    kj::String url)
    : cf(kj::mv(cf)),
      headers(kj::mv(headers)),
      method(kj::mv(method)),
      url(kj::mv(url)) {}

jsg::Ref<TraceItem::FetchEventInfo::Request> TraceItem::FetchEventInfo::getRequest() {
  return request.addRef();
}

jsg::Optional<jsg::Ref<TraceItem::FetchEventInfo::Response>> TraceItem::FetchEventInfo::
    getResponse() {
  return response.map([](auto& ref) mutable -> jsg::Ref<TraceItem::FetchEventInfo::Response> {
    return ref.addRef();
  });
}

TraceItem::FetchEventInfo::Request::Request(
    jsg::Lock& js, const Trace& trace, const tracing::FetchEventInfo& eventInfo)
    : detail(getFetchRequestDetail(js, trace, eventInfo)) {}

TraceItem::FetchEventInfo::Request::Request(Detail& detail, bool redacted)
    : redacted(redacted),
      detail(kj::addRef(detail)) {}

jsg::Optional<jsg::V8Ref<v8::Object>> TraceItem::FetchEventInfo::Request::getCf(jsg::Lock& js) {
  return detail->cf.map([&](jsg::V8Ref<v8::Object>& obj) { return obj.addRef(js); });
}

jsg::Dict<jsg::ByteString, jsg::ByteString> TraceItem::FetchEventInfo::Request::getHeaders() {
  auto shouldRedact = [](kj::StringPtr name) {
    return (
        //(name == "authorization"_kj) || // covered below
        (name == "cookie"_kj) || (name == "set-cookie"_kj) || name.contains("auth"_kjc) ||
        name.contains("jwt"_kjc) || name.contains("key"_kjc) || name.contains("secret"_kjc) ||
        name.contains("token"_kjc));
  };

  using HeaderDict = jsg::Dict<jsg::ByteString, jsg::ByteString>;
  auto builder = kj::heapArrayBuilder<HeaderDict::Field>(detail->headers.size());
  for (const auto& header: detail->headers) {
    auto v = (redacted && shouldRedact(header.name)) ? "REDACTED"_kj : header.value;
    builder.add(
        HeaderDict::Field{jsg::ByteString(kj::str(header.name)), jsg::ByteString(kj::str(v))});
  }

  // TODO(conform): Better to return a frozen JS Object?
  return HeaderDict{builder.finish()};
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

TraceItem::FetchEventInfo::Response::Response(
    const Trace& trace, const tracing::FetchResponseInfo& responseInfo)
    : status(responseInfo.statusCode) {}

uint16_t TraceItem::FetchEventInfo::Response::getStatus() {
  return status;
}

TraceItem::JsRpcEventInfo::JsRpcEventInfo(
    const Trace& trace, const tracing::JsRpcEventInfo& eventInfo)
    : rpcMethod(kj::str(eventInfo.methodName)) {}

kj::StringPtr TraceItem::JsRpcEventInfo::getRpcMethod() {
  return rpcMethod;
}

TraceItem::ScheduledEventInfo::ScheduledEventInfo(
    const Trace& trace, const tracing::ScheduledEventInfo& eventInfo)
    : scheduledTime(eventInfo.scheduledTime),
      cron(kj::str(eventInfo.cron)) {}

double TraceItem::ScheduledEventInfo::getScheduledTime() {
  return scheduledTime;
}
kj::StringPtr TraceItem::ScheduledEventInfo::getCron() {
  return cron;
}

TraceItem::AlarmEventInfo::AlarmEventInfo(
    const Trace& trace, const tracing::AlarmEventInfo& eventInfo)
    : scheduledTime(eventInfo.scheduledTime) {}

kj::Date TraceItem::AlarmEventInfo::getScheduledTime() {
  return scheduledTime;
}

TraceItem::QueueEventInfo::QueueEventInfo(
    const Trace& trace, const tracing::QueueEventInfo& eventInfo)
    : queueName(kj::str(eventInfo.queueName)),
      batchSize(eventInfo.batchSize) {}

kj::StringPtr TraceItem::QueueEventInfo::getQueueName() {
  return queueName;
}

uint32_t TraceItem::QueueEventInfo::getBatchSize() {
  return batchSize;
}

TraceItem::EmailEventInfo::EmailEventInfo(
    const Trace& trace, const tracing::EmailEventInfo& eventInfo)
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

kj::Array<jsg::Ref<TraceItem::TailEventInfo::TailItem>> getConsumedEventsFromEventInfo(
    const tracing::TraceEventInfo& eventInfo) {
  return KJ_MAP(t, eventInfo.traces) -> jsg::Ref<TraceItem::TailEventInfo::TailItem> {
    return jsg::alloc<TraceItem::TailEventInfo::TailItem>(t);
  };
}

TraceItem::TailEventInfo::TailEventInfo(
    const Trace& trace, const tracing::TraceEventInfo& eventInfo)
    : consumedEvents(getConsumedEventsFromEventInfo(eventInfo)) {}

kj::Array<jsg::Ref<TraceItem::TailEventInfo::TailItem>> TraceItem::TailEventInfo::
    getConsumedEvents() {
  return KJ_MAP(consumedEvent, consumedEvents) -> jsg::Ref<TailEventInfo::TailItem> {
    return consumedEvent.addRef();
  };
}

TraceItem::TailEventInfo::TailItem::TailItem(const tracing::TraceEventInfo::TraceItem& traceItem)
    : scriptName(traceItem.scriptName.map([](auto& s) { return kj::str(s); })) {}

kj::Maybe<kj::StringPtr> TraceItem::TailEventInfo::TailItem::getScriptName() {
  return scriptName;
}

TraceDiagnosticChannelEvent::TraceDiagnosticChannelEvent(
    const Trace& trace, const tracing::DiagnosticChannelEvent& eventInfo)
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

double TraceDiagnosticChannelEvent::getTimestamp() {
  return timestamp;
}

ScriptVersion::ScriptVersion(workerd::ScriptVersion::Reader version)
    : id{[&]() -> kj::Maybe<kj::String> {
        return UUID::fromUpperLower(version.getId().getUpper(), version.getId().getLower())
            .map([](const auto& uuid) { return uuid.toString(); });
      }()},
      tag{[&]() -> kj::Maybe<kj::String> {
        if (version.hasTag()) {
          return kj::str(version.getTag());
        }
        return kj::none;
      }()},
      message{[&]() -> kj::Maybe<kj::String> {
        if (version.hasMessage()) {
          return kj::str(version.getMessage());
        }
        return kj::none;
      }()} {}

ScriptVersion::ScriptVersion(const ScriptVersion& other)
    : id{other.id.map([](const auto& id) { return kj::str(id); })},
      tag{other.tag.map([](const auto& tag) { return kj::str(tag); })},
      message{other.message.map([](const auto& message) { return kj::str(message); })} {}

TraceItem::CustomEventInfo::CustomEventInfo(
    const Trace& trace, const tracing::CustomEventInfo& eventInfo)
    : eventInfo(eventInfo) {}

TraceItem::HibernatableWebSocketEventInfo::HibernatableWebSocketEventInfo(
    const Trace& trace, const tracing::HibernatableWebSocketEventInfo::Message& eventInfo)
    : eventType(jsg::alloc<TraceItem::HibernatableWebSocketEventInfo::Message>(trace, eventInfo)) {}

TraceItem::HibernatableWebSocketEventInfo::HibernatableWebSocketEventInfo(
    const Trace& trace, const tracing::HibernatableWebSocketEventInfo::Close& eventInfo)
    : eventType(jsg::alloc<TraceItem::HibernatableWebSocketEventInfo::Close>(trace, eventInfo)) {}

TraceItem::HibernatableWebSocketEventInfo::HibernatableWebSocketEventInfo(
    const Trace& trace, const tracing::HibernatableWebSocketEventInfo::Error& eventInfo)
    : eventType(jsg::alloc<TraceItem::HibernatableWebSocketEventInfo::Error>(trace, eventInfo)) {}

TraceItem::HibernatableWebSocketEventInfo::Type TraceItem::HibernatableWebSocketEventInfo::
    getEvent() {
  KJ_SWITCH_ONEOF(eventType) {
    KJ_CASE_ONEOF(m, jsg::Ref<TraceItem::HibernatableWebSocketEventInfo::Message>) {
      return m.addRef();
    }
    KJ_CASE_ONEOF(c, jsg::Ref<TraceItem::HibernatableWebSocketEventInfo::Close>) {
      return c.addRef();
    }
    KJ_CASE_ONEOF(e, jsg::Ref<TraceItem::HibernatableWebSocketEventInfo::Error>) {
      return e.addRef();
    }
  }
  KJ_UNREACHABLE;
}

uint16_t TraceItem::HibernatableWebSocketEventInfo::Close::getCode() {
  return eventInfo.code;
}

bool TraceItem::HibernatableWebSocketEventInfo::Close::getWasClean() {
  return eventInfo.wasClean;
}

kj::StringPtr OTelSpan::getOperation() {
  return operation;
}

kj::Date OTelSpan::getStartTime() {
  return startTime;
}

kj::StringPtr OTelSpan::getSpanID() {
  return spanId;
}
kj::StringPtr OTelSpan::getParentSpanID() {
  return parentSpanId;
}

kj::Date OTelSpan::getEndTime() {
  return endTime;
}

kj::ArrayPtr<OTelSpanTag> OTelSpan::getTags() {
  return tags;
}

OTelSpan::OTelSpan(const CompleteSpan& span)
    : operation(kj::str(span.operationName)),
      startTime(span.startTime),
      endTime(span.endTime),
      tags(kj::heapArray<OTelSpanTag>(span.tags.size())) {
  // IDs are represented as network-order hex strings.
  uint64_t netSpanId = __builtin_bswap64(span.spanId);
  uint64_t netParentSpanId = __builtin_bswap64(span.parentSpanId);
  spanId = kj::encodeHex(kj::ArrayPtr<byte>((kj::byte*)&netSpanId, sizeof(uint64_t)));
  parentSpanId = kj::encodeHex(kj::ArrayPtr<byte>((kj::byte*)&netParentSpanId, sizeof(uint64_t)));
  uint32_t i = 0;
  for (auto& tag: span.tags) {
    tags[i].key = kj::str(tag.key);
    tags[i].value = spanTagClone(tag.value);
    i++;
  }
}

TraceLog::TraceLog(jsg::Lock& js, const Trace& trace, const tracing::Log& log)
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

TraceException::TraceException(const Trace& trace, const tracing::Exception& exception)
    : timestamp(getTraceExceptionTimestamp(exception)),
      name(kj::str(exception.name)),
      message(kj::str(exception.message)),
      stack(exception.stack.map([](kj::StringPtr s) { return kj::str(s); })) {}

double TraceException::getTimestamp() {
  return timestamp;
}

kj::StringPtr TraceException::getMessage() {
  return message;
}

kj::StringPtr TraceException::getName() {
  return name;
}

jsg::Optional<kj::StringPtr> TraceException::getStack(jsg::Lock& js) {
  return stack;
}

TraceMetrics::TraceMetrics(uint cpuTime, uint wallTime): cpuTime(cpuTime), wallTime(wallTime) {}

jsg::Ref<TraceMetrics> UnsafeTraceMetrics::fromTrace(jsg::Ref<TraceItem> item) {
  return jsg::alloc<TraceMetrics>(item->getCpuTime(), item->getWallTime());
}

namespace {
kj::Promise<void> sendTracesToExportedHandler(kj::Own<IoContext::IncomingRequest> incomingRequest,
    kj::Maybe<kj::StringPtr> entrypointNamePtr,
    Frankenvalue props,
    kj::ArrayPtr<kj::Own<Trace>> traces) {
  // Mark the request as delivered because we're about to run some JS.
  incomingRequest->delivered();

  auto& context = incomingRequest->getContext();
  auto& metrics = incomingRequest->getMetrics();

  KJ_IF_SOME(t, incomingRequest->getWorkerTracer()) {
    t.setEventInfo(context.now(), tracing::TraceEventInfo(traces));
  }

  auto nonEmptyTraces = kj::Vector<kj::Own<Trace>>(kj::size(traces));
  for (auto& trace: traces) {
    if (trace->eventInfo != kj::none) {
      nonEmptyTraces.add(kj::mv(trace));
    }
  }

  // Add the actual JS as a wait until because the handler may be an event listener which can't
  // wait around for async resolution. We're relying on `drain()` below to persist `incomingRequest`
  // and its members until this task completes.
  auto entrypointName = entrypointNamePtr.map([](auto s) { return kj::str(s); });
  try {
    co_await context.run(
        [&context, nonEmptyTraces = kj::mv(nonEmptyTraces), entrypointName = kj::mv(entrypointName),
            props = kj::mv(props)](Worker::Lock& lock) mutable {
      jsg::AsyncContextFrame::StorageScope traceScope = context.makeAsyncTraceScope(lock);

      auto handler = lock.getExportedHandler(entrypointName, kj::mv(props), context.getActor());
      return lock.getGlobalScope().sendTraces(nonEmptyTraces.asPtr(), lock, handler);
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

auto TraceCustomEventImpl::run(kj::Own<IoContext::IncomingRequest> incomingRequest,
    kj::Maybe<kj::StringPtr> entrypointNamePtr,
    Frankenvalue props,
    kj::TaskSet& waitUntilTasks) -> kj::Promise<Result> {
  // Don't bother to wait around for the handler to run, just hand it off to the waitUntil tasks.
  waitUntilTasks.add(sendTracesToExportedHandler(
      kj::mv(incomingRequest), entrypointNamePtr, kj::mv(props), traces));

  return Result{
    .outcome = EventOutcome::OK,
  };
}

auto TraceCustomEventImpl::sendRpc(capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
    capnp::ByteStreamFactory& byteStreamFactory,
    workerd::rpc::EventDispatcher::Client dispatcher) -> kj::Promise<Result> {
  auto req = dispatcher.sendTracesRequest();
  auto out = req.initTraces(traces.size());
  for (auto i: kj::indices(traces)) {
    traces[i]->copyTo(out[i]);
  }

  auto resp = co_await req.send();
  auto respResult = resp.getResult();
  co_return WorkerInterface::CustomEvent::Result{
    .outcome = respResult.getOutcome(),
  };
}

void TailEvent::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  for (const auto& event: events) {
    tracker.trackField(nullptr, event);
  }
}

void TraceItem::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  KJ_IF_SOME(event, eventInfo) {
    KJ_SWITCH_ONEOF(event) {
      KJ_CASE_ONEOF(info, jsg::Ref<FetchEventInfo>) {
        tracker.trackField("eventInfo", info);
      }
      KJ_CASE_ONEOF(info, jsg::Ref<JsRpcEventInfo>) {
        tracker.trackField("eventInfo", info);
      }
      KJ_CASE_ONEOF(info, jsg::Ref<ScheduledEventInfo>) {
        tracker.trackField("eventInfo", info);
      }
      KJ_CASE_ONEOF(info, jsg::Ref<AlarmEventInfo>) {
        tracker.trackField("eventInfo", info);
      }
      KJ_CASE_ONEOF(info, jsg::Ref<QueueEventInfo>) {
        tracker.trackField("eventInfo", info);
      }
      KJ_CASE_ONEOF(info, jsg::Ref<EmailEventInfo>) {
        tracker.trackField("eventInfo", info);
      }
      KJ_CASE_ONEOF(info, jsg::Ref<TailEventInfo>) {
        tracker.trackField("eventInfo", info);
      }
      KJ_CASE_ONEOF(info, jsg::Ref<CustomEventInfo>) {
        tracker.trackField("eventInfo", info);
      }
      KJ_CASE_ONEOF(info, jsg::Ref<HibernatableWebSocketEventInfo>) {
        tracker.trackField("eventInfo", info);
      }
    }
  }
  for (const auto& log: logs) {
    tracker.trackField("log", log);
  }
  for (const auto& exception: exceptions) {
    tracker.trackField("exception", exception);
  }
  for (const auto& event: diagnosticChannelEvents) {
    tracker.trackField("diagnosticChannelEvent", event);
  }
  tracker.trackField("scriptName", scriptName);
  tracker.trackField("scriptVersion", scriptVersion);
  tracker.trackField("dispatchNamespace", dispatchNamespace);
  KJ_IF_SOME(tags, scriptTags) {
    for (const auto& tag: tags) {
      tracker.trackField("scriptTag", tag);
    }
  }
  tracker.trackField("outcome", outcome);
}

void TraceItem::FetchEventInfo::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackField("request", request);
  tracker.trackField("response", response);
}

void TraceItem::TailEventInfo::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  for (const auto& event: consumedEvents) {
    tracker.trackField(nullptr, event);
  }
}

void TraceItem::HibernatableWebSocketEventInfo::visitForMemoryInfo(
    jsg::MemoryTracker& tracker) const {
  KJ_SWITCH_ONEOF(eventType) {
    KJ_CASE_ONEOF(message, jsg::Ref<Message>) {
      tracker.trackField("message", message);
    }
    KJ_CASE_ONEOF(close, jsg::Ref<Close>) {
      tracker.trackField("close", close);
    }
    KJ_CASE_ONEOF(error, jsg::Ref<Error>) {
      tracker.trackField("error", error);
    }
  }
}

}  // namespace workerd::api
