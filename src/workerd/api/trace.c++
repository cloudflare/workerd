// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "trace.h"
#include <workerd/api/http.h>
#include <workerd/api/util.h>
#include <workerd/io/io-context.h>
#include <capnp/schema.h>
#include <workerd/util/thread-scopes.h>

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

kj::Maybe<kj::StringPtr> TraceItem::getScriptName() {
  return trace->scriptName;
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
    case LogLevel::DEBUG: return "debug"_kj;
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

}  // namespace workerd::api
