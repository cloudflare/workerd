// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "trace.h"

#include <workerd/api/global-scope.h>
#include <workerd/api/http.h>
#include <workerd/api/util.h>
#include <workerd/io/io-context.h>
#include <workerd/io/tracer.h>
#include <workerd/jsg/ser.h>
#include <workerd/util/own-util.h>
#include <workerd/util/thread-scopes.h>
#include <workerd/util/uncaught-exception-source.h>
#include <workerd/util/uuid.h>

#include <capnp/message.h>
#include <capnp/schema.h>
#include <capnp/serialize.h>
#include <kj/encoding.h>

namespace workerd::api {

TailEvent::TailEvent(jsg::Lock& js, kj::StringPtr type, kj::ArrayPtr<kj::Own<Trace>> events)
    : ExtendableEvent(kj::str(type)),
      events(KJ_MAP(e, events) -> jsg::Ref<TraceItem> { return js.alloc<TraceItem>(js, *e); }) {}

kj::Array<jsg::Ref<TraceItem>> TailEvent::getEvents() {
  return KJ_MAP(e, events) -> jsg::Ref<TraceItem> { return e.addRef(); };
}

namespace {
kj::Maybe<double> getTraceTimestamp(const Trace& trace) {
  if (isPredictableModeForTest()) {
    return 0.0;
  }
  if (trace.eventTimestamp == kj::UNIX_EPOCH) {
    return kj::none;
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
  return KJ_MAP(x, trace.logs) -> jsg::Ref<TraceLog> { return js.alloc<TraceLog>(js, trace, x); };
}

kj::Array<jsg::Ref<OTelSpan>> getTraceSpans(jsg::Lock& js, const Trace& trace) {
  return KJ_MAP(x, trace.spans) -> jsg::Ref<OTelSpan> { return js.alloc<OTelSpan>(x); };
}

kj::Array<jsg::Ref<TraceDiagnosticChannelEvent>> getTraceDiagnosticChannelEvents(
    jsg::Lock& js, const Trace& trace) {
  return KJ_MAP(x, trace.diagnosticChannelEvents) -> jsg::Ref<TraceDiagnosticChannelEvent> {
    return js.alloc<TraceDiagnosticChannelEvent>(trace, x);
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

kj::Array<jsg::Ref<TraceException>> getTraceExceptions(jsg::Lock& js, const Trace& trace) {
  return KJ_MAP(x, trace.exceptions) -> jsg::Ref<TraceException> { return js.alloc<TraceException>(trace, x); };
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

template <typename Enum>
kj::Maybe<Enum> strToEnum(kj::StringPtr str) {
  auto enums = capnp::Schema::from<Enum>().getEnumerants();
  for (uint i = 0; i < enums.size(); i++) {
    if (enums[i].getProto().getName() == str) {
      return static_cast<Enum>(i);
    }
  }
  return kj::none;  // Not found
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
            js.alloc<TraceItem::FetchEventInfo>(js, trace, fetch, trace.fetchResponseInfo));
      }
      KJ_CASE_ONEOF(jsRpc, tracing::JsRpcEventInfo) {
        return kj::Maybe(js.alloc<TraceItem::JsRpcEventInfo>(trace, jsRpc));
      }
      KJ_CASE_ONEOF(scheduled, tracing::ScheduledEventInfo) {
        return kj::Maybe(js.alloc<TraceItem::ScheduledEventInfo>(trace, scheduled));
      }
      KJ_CASE_ONEOF(alarm, tracing::AlarmEventInfo) {
        return kj::Maybe(js.alloc<TraceItem::AlarmEventInfo>(trace, alarm));
      }
      KJ_CASE_ONEOF(queue, tracing::QueueEventInfo) {
        return kj::Maybe(js.alloc<TraceItem::QueueEventInfo>(trace, queue));
      }
      KJ_CASE_ONEOF(email, tracing::EmailEventInfo) {
        return kj::Maybe(js.alloc<TraceItem::EmailEventInfo>(trace, email));
      }
      KJ_CASE_ONEOF(tracedTrace, tracing::TraceEventInfo) {
        return kj::Maybe(js.alloc<TraceItem::TailEventInfo>(js, trace, tracedTrace));
      }
      KJ_CASE_ONEOF(hibWs, tracing::HibernatableWebSocketEventInfo) {
        KJ_SWITCH_ONEOF(hibWs.type) {
          KJ_CASE_ONEOF(message, tracing::HibernatableWebSocketEventInfo::Message) {
            return kj::Maybe(
                js.alloc<TraceItem::HibernatableWebSocketEventInfo>(js, trace, message));
          }
          KJ_CASE_ONEOF(close, tracing::HibernatableWebSocketEventInfo::Close) {
            return kj::Maybe(js.alloc<TraceItem::HibernatableWebSocketEventInfo>(js, trace, close));
          }
          KJ_CASE_ONEOF(error, tracing::HibernatableWebSocketEventInfo::Error) {
            return kj::Maybe(js.alloc<TraceItem::HibernatableWebSocketEventInfo>(js, trace, error));
          }
        }
        KJ_UNREACHABLE;
      }
      KJ_CASE_ONEOF(resume, tracing::Resume) {
        // Resume events are not used with legacy trace
        KJ_UNREACHABLE;
      }
      KJ_CASE_ONEOF(custom, tracing::CustomEventInfo) {
        return kj::Maybe(js.alloc<TraceItem::CustomEventInfo>(trace, custom));
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
      exceptions(getTraceExceptions(js, trace)),
      diagnosticChannelEvents(getTraceDiagnosticChannelEvents(js, trace)),
      scriptName(trace.scriptName.map([](auto& name) { return kj::str(name); })),
      entrypoint(trace.entrypoint.map([](auto& name) { return kj::str(name); })),
      scriptVersion(getTraceScriptVersion(trace)),
      dispatchNamespace(trace.dispatchNamespace.map([](auto& ns) { return kj::str(ns); })),
      scriptTags(getTraceScriptTags(trace)),
      executionModel(enumToStr(trace.executionModel)),
      spans(getTraceSpans(js, trace)),
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

void TraceItem::serialize(jsg::Lock& js, jsg::Serializer& serializer) {
  capnp::MallocMessageBuilder traceMessage;
  auto trace = traceMessage.initRoot<rpc::Trace>();

  auto outcome_ = strToEnum<workerd::EventOutcome>(outcome);
  KJ_IF_SOME(o, outcome_) {
    trace.setOutcome(o);
  }

  auto executionModel_ = strToEnum<rpc::Trace::ExecutionModel>(executionModel);
  KJ_IF_SOME(em, executionModel_) {
    trace.setExecutionModel(em);
  }

  trace.setCpuTime(cpuTime);
  trace.setWallTime(wallTime);
  trace.setTruncated(truncated);

  KJ_IF_SOME(et, eventTimestamp) {
    auto timestampNs = static_cast<int64_t>(et * (kj::MILLISECONDS / kj::NANOSECONDS));
    trace.setEventTimestampNs(timestampNs);
  }

  KJ_IF_SOME(sn, scriptName) {
    trace.setScriptName(sn);
  }

  KJ_IF_SOME(e, entrypoint) {
    trace.setEntrypoint(e);
  }

  KJ_IF_SOME(dn, dispatchNamespace) {
    trace.setDispatchNamespace(dn);
  }

  KJ_IF_SOME(sv, scriptVersion) {
    auto version = trace.initScriptVersion();
    KJ_IF_SOME(id, sv.id) {
      KJ_IF_SOME(uuid, UUID::fromString(id)) {
        auto versionId = version.initId();
        versionId.setUpper(uuid.getUpper());
        versionId.setLower(uuid.getLower());
      }
    }
    KJ_IF_SOME(tag, sv.tag) {
      version.setTag(tag);
    }
    KJ_IF_SOME(message, sv.message) {
      version.setMessage(message);
    }
  }

  KJ_IF_SOME(tags, scriptTags) {
    auto list = trace.initScriptTags(tags.size());
    for (auto i: kj::indices(tags)) {
      list.set(i, tags[i]);
    }
  }

  if (logs.size() > 0) {
    auto logList = trace.initLogs(logs.size());
    for (auto i: kj::indices(logs)) {
      auto logBuilder = logList[i];

      auto timestampNs =
          static_cast<int64_t>(logs[i]->getTimestamp() * (kj::MILLISECONDS / kj::NANOSECONDS));
      logBuilder.setTimestampNs(timestampNs);

      // Convert level string back to enum using strToEnum
      auto levelStr = logs[i]->getLevel();
      KJ_IF_SOME(level, strToEnum<rpc::Trace::Log::Level>(levelStr)) {
        logBuilder.setLogLevel(level);
      }

      auto messageJson = js.serializeJson(logs[i]->getMessage(js));
      logBuilder.setMessage(messageJson);
    }
  }

  if (exceptions.size() > 0) {
    auto exceptionList = trace.initExceptions(exceptions.size());
    for (auto i: kj::indices(exceptions)) {
      auto exceptionBuilder = exceptionList[i];

      auto timestampNs = static_cast<int64_t>(
          exceptions[i]->getTimestamp() * (kj::MILLISECONDS / kj::NANOSECONDS));
      exceptionBuilder.setTimestampNs(timestampNs);

      exceptionBuilder.setName(exceptions[i]->getName());
      exceptionBuilder.setMessage(exceptions[i]->getMessage());

      KJ_IF_SOME(stack, exceptions[i]->getStack(js)) {
        exceptionBuilder.setStack(stack);
      }
    }
  }

  if (diagnosticChannelEvents.size() > 0) {
    auto diagnosticList = trace.initDiagnosticChannelEvents(diagnosticChannelEvents.size());
    for (auto i: kj::indices(diagnosticChannelEvents)) {
      auto diagnosticBuilder = diagnosticList[i];

      auto timestampNs = static_cast<int64_t>(
          diagnosticChannelEvents[i]->getTimestamp() * (kj::MILLISECONDS / kj::NANOSECONDS));
      diagnosticBuilder.setTimestampNs(timestampNs);

      diagnosticBuilder.setChannel(diagnosticChannelEvents[i]->getChannel());

      auto jsValue = diagnosticChannelEvents[i]->getMessage(js);
      jsg::Serializer serializer(js);
      serializer.write(js, jsValue);
      auto released = serializer.release();
      diagnosticBuilder.setMessage(released.data);
    }
  }

  KJ_IF_SOME(info, eventInfo) {
    KJ_SWITCH_ONEOF(info) {
      KJ_CASE_ONEOF(info, jsg::Ref<FetchEventInfo>) {
        auto eventInfo = trace.initEventInfo();
        auto fetchInfo = eventInfo.initFetch();

        auto request = info->getRequest();

        auto methodStr = request->getMethod();
        KJ_IF_SOME(kjMethod, kj::tryParseHttpMethod(methodStr)) {
          auto capnpMethod = static_cast<capnp::HttpMethod>(kjMethod);
          fetchInfo.setMethod(capnpMethod);
        }

        fetchInfo.setUrl(request->getUrl());

        KJ_IF_SOME(cfObj, request->getCf(js)) {
          auto cfJson = js.serializeJson(cfObj.addRef(js));
          fetchInfo.setCfJson(cfJson);
        } else {
          fetchInfo.setCfJson("");
        }

        auto headersDict = request->getHeaders(js);
        auto headersList = fetchInfo.initHeaders(headersDict.fields.size());

        for (auto i: kj::indices(headersDict.fields)) {
          auto headerBuilder = headersList[i];
          headerBuilder.setName(headersDict.fields[i].name);
          headerBuilder.setValue(headersDict.fields[i].value);
        }

        KJ_IF_SOME(response, info->getResponse()) {
          auto responseBuilder = trace.initResponse();
          responseBuilder.setStatusCode(response->getStatus());
        }
      }
      KJ_CASE_ONEOF(info, jsg::Ref<JsRpcEventInfo>) {
        auto eventInfo = trace.initEventInfo();
        auto jsRpc = eventInfo.initJsRpc();
        jsRpc.setMethodName(info->getRpcMethod());
      }
      KJ_CASE_ONEOF(info, jsg::Ref<ScheduledEventInfo>) {
        auto eventInfo = trace.initEventInfo();
        auto scheduled = eventInfo.initScheduled();
        scheduled.setScheduledTime(info->getScheduledTime());
        scheduled.setCron(info->getCron());
      }
      KJ_CASE_ONEOF(info, jsg::Ref<AlarmEventInfo>) {
        auto eventInfo = trace.initEventInfo();
        auto alarm = eventInfo.initAlarm();
        // Convert kj::Date to milliseconds since epoch
        auto scheduledTimeMs = (info->getScheduledTime() - kj::UNIX_EPOCH) / kj::MILLISECONDS;
        alarm.setScheduledTimeMs(scheduledTimeMs);
      }
      KJ_CASE_ONEOF(info, jsg::Ref<QueueEventInfo>) {
        auto eventInfo = trace.initEventInfo();
        auto queue = eventInfo.initQueue();
        queue.setQueueName(info->getQueueName());
        queue.setBatchSize(info->getBatchSize());
      }
      KJ_CASE_ONEOF(info, jsg::Ref<EmailEventInfo>) {
        auto eventInfo = trace.initEventInfo();
        auto email = eventInfo.initEmail();
        email.setMailFrom(info->getMailFrom());
        email.setRcptTo(info->getRcptTo());
        email.setRawSize(info->getRawSize());
      }
      KJ_CASE_ONEOF(info, jsg::Ref<TailEventInfo>) {
        auto eventInfo = trace.initEventInfo();
        auto tailEvent = eventInfo.initTrace();

        auto consumedEvents = info->getConsumedEvents();
        auto tracesList = tailEvent.initTraces(consumedEvents.size());

        for (auto i: kj::indices(consumedEvents)) {
          auto traceItemBuilder = tracesList[i];
          KJ_IF_SOME(scriptName, consumedEvents[i]->getScriptName()) {
            traceItemBuilder.setScriptName(scriptName);
          }
        }
      }
      KJ_CASE_ONEOF(info, jsg::Ref<HibernatableWebSocketEventInfo>) {
        auto eventInfo = trace.initEventInfo();
        auto hibernatableWs = eventInfo.initHibernatableWebSocket();

        auto eventType = info->getEvent();
        KJ_SWITCH_ONEOF(eventType) {
          KJ_CASE_ONEOF(message, jsg::Ref<HibernatableWebSocketEventInfo::Message>) {
            hibernatableWs.getType().setMessage();
          }
          KJ_CASE_ONEOF(close, jsg::Ref<HibernatableWebSocketEventInfo::Close>) {
            auto closeGroup = hibernatableWs.getType().initClose();
            closeGroup.setCode(close->getCode());
            closeGroup.setWasClean(close->getWasClean());
          }
          KJ_CASE_ONEOF(error, jsg::Ref<HibernatableWebSocketEventInfo::Error>) {
            hibernatableWs.getType().setError();
          }
        }
      }
      KJ_CASE_ONEOF(info, jsg::Ref<CustomEventInfo>) {
        auto eventInfo = trace.initEventInfo();
        eventInfo.initCustom();
      }
    }
  };

  if (spans.size() > 0) {
    auto spansList = trace.initSpans(spans.size());
    for (auto i: kj::indices(spans)) {
      auto spanBuilder = spansList[i];

      spanBuilder.setOperationName(spans[i]->getOperation());

      auto startTimeNs = (spans[i]->getStartTime() - kj::UNIX_EPOCH) / kj::NANOSECONDS;
      auto endTimeNs = (spans[i]->getEndTime() - kj::UNIX_EPOCH) / kj::NANOSECONDS;
      spanBuilder.setStartTimeNs(startTimeNs);
      spanBuilder.setEndTimeNs(endTimeNs);

      // Convert hex span ID back to uint64
      // OTelSpan stores IDs as hex strings - parse them as-is without byte order conversion
      auto spanIdHex = spans[i]->getSpanID();
      auto spanIdWithPrefix = kj::str("0x", spanIdHex);
      KJ_IF_SOME(spanIdValue, spanIdWithPrefix.tryParseAs<uint64_t>()) {
        spanBuilder.setSpanId(spanIdValue);
      }

      auto parentSpanIdHex = spans[i]->getParentSpanID();
      if (parentSpanIdHex.size() > 0) {
        auto parentSpanIdWithPrefix = kj::str("0x", parentSpanIdHex);
        KJ_IF_SOME(parentSpanIdValue, parentSpanIdWithPrefix.tryParseAs<uint64_t>()) {
          spanBuilder.setParentSpanId(parentSpanIdValue);
        }
      } else {
        spanBuilder.setParentSpanId(0);
      }

      auto spanTags = spans[i]->getTags();
      auto tagsList = spanBuilder.initTags(spanTags.size());
      for (auto j: kj::indices(spanTags)) {
        auto tagBuilder = tagsList[j];
        tagBuilder.setKey(spanTags[j].key);

        auto& tagValue = spanTags[j].value;
        auto valueBuilder = tagBuilder.initValue();
        KJ_SWITCH_ONEOF(tagValue) {
          KJ_CASE_ONEOF(str, kj::String) {
            valueBuilder.setString(str);
          }
          KJ_CASE_ONEOF(b, bool) {
            valueBuilder.setBool(b);
          }
          KJ_CASE_ONEOF(d, double) {
            valueBuilder.setFloat64(d);
          }
          KJ_CASE_ONEOF(i, int64_t) {
            valueBuilder.setInt64(i);
          }
        }
      }
    }
  }
  auto words = capnp::messageToFlatArray(traceMessage);
  auto bytes = words.asBytes();
  serializer.writeLengthDelimited(bytes);
}

jsg::Ref<TraceItem> TraceItem::deserialize(
    jsg::Lock& js, rpc::SerializationTag tag, jsg::Deserializer& deserializer) {
  auto bytes = deserializer.readLengthDelimitedBytes();

  // Ensure alignment by copying to word-aligned memory
  auto alignedBytes =
      kj::heapArray<capnp::word>((bytes.size() + sizeof(capnp::word) - 1) / sizeof(capnp::word));
  memcpy(alignedBytes.begin(), bytes.begin(), bytes.size());

  capnp::FlatArrayMessageReader messageReader(alignedBytes.asPtr());
  auto bundle = messageReader.getRoot<rpc::Trace>();

  Trace trace(bundle);
  return js.alloc<TraceItem>(js, trace);
}

jsg::JsValue TraceItem::toJSON(jsg::Lock& js) {
  auto obj = js.obj();

  // Basic fields
  obj.set(js, "outcome", js.str(outcome));
  obj.set(js, "executionModel", js.str(executionModel));
  obj.set(js, "cpuTime", js.num(cpuTime));
  obj.set(js, "wallTime", js.num(wallTime));
  obj.set(js, "truncated", js.boolean(truncated));

  // Optional fields
  KJ_IF_SOME(et, eventTimestamp) {
    obj.set(js, "eventTimestamp", js.num(et));
  }
  KJ_IF_SOME(sn, scriptName) {
    obj.set(js, "scriptName", js.str(sn));
  }
  KJ_IF_SOME(ep, entrypoint) {
    obj.set(js, "entrypoint", js.str(ep));
  }
  KJ_IF_SOME(dn, dispatchNamespace) {
    obj.set(js, "dispatchNamespace", js.str(dn));
  }
  KJ_IF_SOME(st, scriptTags) {
    auto tagsArray = js.arr();
    for (auto i: kj::indices(st)) {
      tagsArray.add(js, js.str(st[i]));
    }
    obj.set(js, "scriptTags", tagsArray);
  }
  KJ_IF_SOME(sv, scriptVersion) {
    auto svObj = js.obj();
    KJ_IF_SOME(id, sv.id) {
      svObj.set(js, "id", js.str(id));
    }
    KJ_IF_SOME(tag, sv.tag) {
      svObj.set(js, "tag", js.str(tag));
    }
    KJ_IF_SOME(msg, sv.message) {
      svObj.set(js, "message", js.str(msg));
    }
    obj.set(js, "scriptVersion", svObj);
  }

  // Convert arrays using their toJSON methods
  if (logs.size() > 0) {
    auto logsArray = js.arr();
    for (auto i: kj::indices(logs)) {
      logsArray.add(js, logs[i]->toJSON(js));
    }
    obj.set(js, "logs", logsArray);
  }

  if (exceptions.size() > 0) {
    auto exceptionsArray = js.arr();
    for (auto i: kj::indices(exceptions)) {
      exceptionsArray.add(js, exceptions[i]->toJSON(js));
    }
    obj.set(js, "exceptions", exceptionsArray);
  }

  if (diagnosticChannelEvents.size() > 0) {
    auto eventsArray = js.arr();
    for (auto i: kj::indices(diagnosticChannelEvents)) {
      eventsArray.add(js, diagnosticChannelEvents[i]->toJSON(js));
    }
    obj.set(js, "diagnosticsChannelEvents", eventsArray);
  }

  if (spans.size() > 0) {
    auto spansArray = js.arr();
    for (auto i: kj::indices(spans)) {
      spansArray.add(js, spans[i]->toJSON(js));
    }
    obj.set(js, "spans", spansArray);
  }

  // Handle eventInfo using their toJSON methods
  KJ_IF_SOME(ei, eventInfo) {
    KJ_SWITCH_ONEOF(ei) {
      KJ_CASE_ONEOF(fetchEvent, jsg::Ref<FetchEventInfo>) {
        obj.set(js, "event", fetchEvent->toJSON(js));
      }
      KJ_CASE_ONEOF(jsRpcEvent, jsg::Ref<JsRpcEventInfo>) {
        obj.set(js, "event", jsRpcEvent->toJSON(js));
      }
      KJ_CASE_ONEOF(scheduledEvent, jsg::Ref<ScheduledEventInfo>) {
        obj.set(js, "event", scheduledEvent->toJSON(js));
      }
      KJ_CASE_ONEOF(alarmEvent, jsg::Ref<AlarmEventInfo>) {
        obj.set(js, "event", alarmEvent->toJSON(js));
      }
      KJ_CASE_ONEOF(queueEvent, jsg::Ref<QueueEventInfo>) {
        obj.set(js, "event", queueEvent->toJSON(js));
      }
      KJ_CASE_ONEOF(emailEvent, jsg::Ref<EmailEventInfo>) {
        obj.set(js, "event", emailEvent->toJSON(js));
      }
      KJ_CASE_ONEOF(tailEvent, jsg::Ref<TailEventInfo>) {
        obj.set(js, "event", tailEvent->toJSON(js));
      }
      KJ_CASE_ONEOF(customEvent, jsg::Ref<CustomEventInfo>) {
        obj.set(js, "event", customEvent->toJSON(js));
      }
      KJ_CASE_ONEOF(hibernatableWebSocketEvent, jsg::Ref<HibernatableWebSocketEventInfo>) {
        obj.set(js, "event", hibernatableWebSocketEvent->toJSON(js));
      }
    }
  }

  return jsg::JsValue(obj);
}

TraceItem::FetchEventInfo::FetchEventInfo(jsg::Lock& js,
    const Trace& trace,
    const tracing::FetchEventInfo& eventInfo,
    kj::Maybe<const tracing::FetchResponseInfo&> responseInfo)
    : request(js.alloc<Request>(js, trace, eventInfo)),
      response(responseInfo.map([&](auto& info) { return js.alloc<Response>(trace, info); })) {}

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

jsg::Dict<jsg::ByteString, jsg::ByteString> TraceItem::FetchEventInfo::Request::getHeaders(
    jsg::Lock& js) {
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

jsg::Ref<TraceItem::FetchEventInfo::Request> TraceItem::FetchEventInfo::Request::getUnredacted(
    jsg::Lock& js) {
  return js.alloc<Request>(*detail, false /* details are not redacted */);
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
    jsg::Lock& js, const tracing::TraceEventInfo& eventInfo) {
  return KJ_MAP(t, eventInfo.traces) -> jsg::Ref<TraceItem::TailEventInfo::TailItem> {
    return js.alloc<TraceItem::TailEventInfo::TailItem>(t);
  };
}

TraceItem::TailEventInfo::TailEventInfo(
    jsg::Lock& js, const Trace& trace, const tracing::TraceEventInfo& eventInfo)
    : consumedEvents(getConsumedEventsFromEventInfo(js, eventInfo)) {}

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

jsg::JsValue TraceDiagnosticChannelEvent::toJSON(jsg::Lock& js) {
  auto obj = js.obj();
  obj.set(js, "timestamp", js.num(timestamp));
  obj.set(js, "channel", js.str(channel));
  obj.set(js, "message", getMessage(js));
  return jsg::JsValue(obj);
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

TraceItem::HibernatableWebSocketEventInfo::HibernatableWebSocketEventInfo(jsg::Lock& js,
    const Trace& trace,
    const tracing::HibernatableWebSocketEventInfo::Message eventInfo)
    : eventType(js.alloc<TraceItem::HibernatableWebSocketEventInfo::Message>(trace, eventInfo)) {}

TraceItem::HibernatableWebSocketEventInfo::HibernatableWebSocketEventInfo(jsg::Lock& js,
    const Trace& trace,
    const tracing::HibernatableWebSocketEventInfo::Close eventInfo)
    : eventType(js.alloc<TraceItem::HibernatableWebSocketEventInfo::Close>(trace, eventInfo)) {}

TraceItem::HibernatableWebSocketEventInfo::HibernatableWebSocketEventInfo(jsg::Lock& js,
    const Trace& trace,
    const tracing::HibernatableWebSocketEventInfo::Error eventInfo)
    : eventType(js.alloc<TraceItem::HibernatableWebSocketEventInfo::Error>(trace, eventInfo)) {}

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

jsg::JsValue OTelSpan::toJSON(jsg::Lock& js) {
  auto obj = js.obj();
  obj.set(js, "spanId", js.str(spanId));
  obj.set(js, "parentSpanId", js.str(parentSpanId));
  obj.set(js, "operation", js.str(operation));
  obj.set(js, "startTime", js.date(startTime));
  obj.set(js, "endTime", js.date(endTime));

  // Convert tags array
  auto tagsArray = js.arr();
  for (auto i: kj::indices(tags)) {
    auto tagObj = js.obj();
    tagObj.set(js, "key", js.str(tags[i].key));

    // Handle the TagValue union
    KJ_SWITCH_ONEOF(tags[i].value) {
      KJ_CASE_ONEOF(str, kj::String) {
        tagObj.set(js, "value", js.str(str));
      }
      KJ_CASE_ONEOF(b, bool) {
        tagObj.set(js, "value", js.boolean(b));
      }
      KJ_CASE_ONEOF(d, double) {
        tagObj.set(js, "value", js.num(d));
      }
      KJ_CASE_ONEOF(i, int64_t) {
        tagObj.set(js, "value", js.num(static_cast<double>(i)));
      }
    }
    tagsArray.add(js, tagObj);
  }
  obj.set(js, "tags", tagsArray);

  return jsg::JsValue(obj);
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

jsg::JsValue TraceLog::toJSON(jsg::Lock& js) {
  auto obj = js.obj();
  obj.set(js, "timestamp", js.num(timestamp));
  obj.set(js, "level", js.str(level));
  obj.set(js, "message", jsg::JsValue(message.getHandle(js)));
  return jsg::JsValue(obj);
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

jsg::JsValue TraceException::toJSON(jsg::Lock& js) {
  auto obj = js.obj();
  obj.set(js, "timestamp", js.num(timestamp));
  obj.set(js, "name", js.str(name));
  obj.set(js, "message", js.str(message));
  KJ_IF_SOME(s, stack) {
    obj.set(js, "stack", js.str(s));
  }
  return jsg::JsValue(obj);
}

TraceMetrics::TraceMetrics(uint cpuTime, uint wallTime): cpuTime(cpuTime), wallTime(wallTime) {}

jsg::Ref<TraceMetrics> UnsafeTraceMetrics::fromTrace(jsg::Lock& js, jsg::Ref<TraceItem> item) {
  return js.alloc<TraceMetrics>(item->getCpuTime(), item->getWallTime());
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
    t.setEventInfo(
        context.getInvocationSpanContext(), context.now(), tracing::TraceEventInfo(traces));
  }

  auto nonEmptyTraces = kj::Vector<kj::Own<Trace>>(kj::size(traces));
  for (auto& trace: traces) {
    if (trace->eventInfo != kj::none) {
      nonEmptyTraces.add(kj::addRef(*trace));
    }
  }

  // Add the actual JS as a wait until because the handler may be an event listener which can't
  // wait around for async resolution. We're relying on `drain()` below to persist `incomingRequest`
  // and its members until this task completes.
  auto entrypointName = entrypointNamePtr.map([](auto s) { return kj::str(s); });
  try {
    co_await context.run(
        [&context, nonEmptyTraces = nonEmptyTraces.asPtr(), entrypointName = kj::mv(entrypointName),
            props = kj::mv(props)](Worker::Lock& lock) mutable {
      jsg::AsyncContextFrame::StorageScope traceScope = context.makeAsyncTraceScope(lock);

      auto handler = lock.getExportedHandler(entrypointName, kj::mv(props), context.getActor());
      return lock.getGlobalScope().sendTraces(nonEmptyTraces, lock, handler);
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

// toJSON implementations for all EventInfo types

jsg::JsValue TraceItem::FetchEventInfo::toJSON(jsg::Lock& js) {
  auto obj = js.obj();
  obj.set(js, "request", request->toJSON(js));
  KJ_IF_SOME(resp, response) {
    obj.set(js, "response", resp->toJSON(js));
  }
  return jsg::JsValue(obj);
}

jsg::JsValue TraceItem::FetchEventInfo::Request::toJSON(jsg::Lock& js) {
  auto obj = js.obj();
  obj.set(js, "method", js.str(getMethod()));
  obj.set(js, "url", js.str(getUrl()));

  // Convert headers dict to object
  auto headers = getHeaders(js);
  auto headersObj = js.obj();
  for (auto& entry: headers.fields) {
    headersObj.set(js, entry.name, js.str(entry.value));
  }
  obj.set(js, "headers", headersObj);

  KJ_IF_SOME(cf, getCf(js)) {
    obj.set(js, "cf", jsg::JsValue(cf.getHandle(js)));
  }

  return jsg::JsValue(obj);
}

jsg::JsValue TraceItem::FetchEventInfo::Response::toJSON(jsg::Lock& js) {
  auto obj = js.obj();
  obj.set(js, "status", js.num(getStatus()));
  return jsg::JsValue(obj);
}

jsg::JsValue TraceItem::JsRpcEventInfo::toJSON(jsg::Lock& js) {
  auto obj = js.obj();
  obj.set(js, "rpcMethod", js.str(getRpcMethod()));
  return jsg::JsValue(obj);
}

jsg::JsValue TraceItem::ScheduledEventInfo::toJSON(jsg::Lock& js) {
  auto obj = js.obj();
  obj.set(js, "scheduledTime", js.num(getScheduledTime()));
  obj.set(js, "cron", js.str(getCron()));
  return jsg::JsValue(obj);
}

jsg::JsValue TraceItem::AlarmEventInfo::toJSON(jsg::Lock& js) {
  auto obj = js.obj();
  obj.set(js, "scheduledTime", js.date(getScheduledTime()));
  return jsg::JsValue(obj);
}

jsg::JsValue TraceItem::QueueEventInfo::toJSON(jsg::Lock& js) {
  auto obj = js.obj();
  obj.set(js, "queue", js.str(getQueueName()));
  obj.set(js, "batchSize", js.num(getBatchSize()));
  return jsg::JsValue(obj);
}

jsg::JsValue TraceItem::EmailEventInfo::toJSON(jsg::Lock& js) {
  auto obj = js.obj();
  obj.set(js, "mailFrom", js.str(getMailFrom()));
  obj.set(js, "rcptTo", js.str(getRcptTo()));
  obj.set(js, "rawSize", js.num(getRawSize()));
  return jsg::JsValue(obj);
}

jsg::JsValue TraceItem::TailEventInfo::toJSON(jsg::Lock& js) {
  auto obj = js.obj();
  auto eventsArray = js.arr();
  auto consumedEvents = getConsumedEvents();
  for (auto i: kj::indices(consumedEvents)) {
    eventsArray.add(js, consumedEvents[i]->toJSON(js));
  }
  obj.set(js, "consumedEvents", eventsArray);
  return jsg::JsValue(obj);
}

jsg::JsValue TraceItem::TailEventInfo::TailItem::toJSON(jsg::Lock& js) {
  auto obj = js.obj();
  KJ_IF_SOME(sn, getScriptName()) {
    obj.set(js, "scriptName", js.str(sn));
  }
  return jsg::JsValue(obj);
}

jsg::JsValue TraceItem::HibernatableWebSocketEventInfo::toJSON(jsg::Lock& js) {
  auto obj = js.obj();
  auto event = getEvent();
  KJ_SWITCH_ONEOF(event) {
    KJ_CASE_ONEOF(message, jsg::Ref<Message>) {
      obj.set(js, "getWebSocketEvent", message->toJSON(js));
    }
    KJ_CASE_ONEOF(close, jsg::Ref<Close>) {
      obj.set(js, "getWebSocketEvent", close->toJSON(js));
    }
    KJ_CASE_ONEOF(error, jsg::Ref<Error>) {
      obj.set(js, "getWebSocketEvent", error->toJSON(js));
    }
  }
  return jsg::JsValue(obj);
}

jsg::JsValue TraceItem::HibernatableWebSocketEventInfo::Message::toJSON(jsg::Lock& js) {
  auto obj = js.obj();
  obj.set(js, "webSocketEventType", js.str(getWebSocketEventType()));
  return jsg::JsValue(obj);
}

jsg::JsValue TraceItem::HibernatableWebSocketEventInfo::Close::toJSON(jsg::Lock& js) {
  auto obj = js.obj();
  obj.set(js, "webSocketEventType", js.str(getWebSocketEventType()));
  obj.set(js, "code", js.num(getCode()));
  obj.set(js, "wasClean", js.boolean(getWasClean()));
  return jsg::JsValue(obj);
}

jsg::JsValue TraceItem::HibernatableWebSocketEventInfo::Error::toJSON(jsg::Lock& js) {
  auto obj = js.obj();
  obj.set(js, "webSocketEventType", js.str(getWebSocketEventType()));
  return jsg::JsValue(obj);
}

jsg::JsValue TraceItem::CustomEventInfo::toJSON(jsg::Lock& js) {
  auto obj = js.obj();
  // CustomEventInfo doesn't expose any properties currently
  return jsg::JsValue(obj);
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
