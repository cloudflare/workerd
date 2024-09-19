// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/io/trace.h>
#include <workerd/util/thread-scopes.h>

#include <capnp/message.h>
#include <capnp/schema.h>
#include <kj/debug.h>
#include <kj/time.h>

#include <cstdlib>

namespace workerd {

// Approximately how much external data we allow in a trace before we start ignoring requests.  We
// want this number to be big enough to be useful for tracing, but small enough to make it hard to
// DoS the C++ heap -- keeping in mind we can record a trace per handler run during a request.
static constexpr size_t MAX_TRACE_BYTES = 128 * 1024;
// Limit spans to at most 512, it could be difficult to fit e.g. 1024 spans within MAX_TRACE_BYTES
// unless most of the included spans do not include tags. If use cases arise where this amount is
// insufficient, merge smaller spans together or drop smaller spans.
static constexpr size_t MAX_LIME_SPANS = 512;

namespace {

static kj::HttpMethod validateMethod(capnp::HttpMethod method) {
  KJ_REQUIRE(method <= capnp::HttpMethod::UNSUBSCRIBE, "unknown method", method);
  return static_cast<kj::HttpMethod>(method);
}

}  // namespace

Trace::FetchEventInfo::FetchEventInfo(
    kj::HttpMethod method, kj::String url, kj::String cfJson, kj::Array<Header> headers)
    : method(method),
      url(kj::mv(url)),
      cfJson(kj::mv(cfJson)),
      headers(kj::mv(headers)) {}

Trace::FetchEventInfo::FetchEventInfo(rpc::Trace::FetchEventInfo::Reader reader)
    : method(validateMethod(reader.getMethod())),
      url(kj::str(reader.getUrl())),
      cfJson(kj::str(reader.getCfJson())) {
  kj::Vector<Header> v;
  v.addAll(reader.getHeaders());
  headers = v.releaseAsArray();
}

void Trace::FetchEventInfo::copyTo(rpc::Trace::FetchEventInfo::Builder builder) {
  builder.setMethod(static_cast<capnp::HttpMethod>(method));
  builder.setUrl(url);
  builder.setCfJson(cfJson);

  auto list = builder.initHeaders(headers.size());
  for (auto i: kj::indices(headers)) {
    headers[i].copyTo(list[i]);
  }
}

Trace::FetchEventInfo::Header::Header(kj::String name, kj::String value)
    : name(kj::mv(name)),
      value(kj::mv(value)) {}

Trace::FetchEventInfo::Header::Header(rpc::Trace::FetchEventInfo::Header::Reader reader)
    : name(kj::str(reader.getName())),
      value(kj::str(reader.getValue())) {}

void Trace::FetchEventInfo::Header::copyTo(rpc::Trace::FetchEventInfo::Header::Builder builder) {
  builder.setName(name);
  builder.setValue(value);
}

Trace::JsRpcEventInfo::JsRpcEventInfo(kj::String methodName): methodName(kj::mv(methodName)) {}

Trace::JsRpcEventInfo::JsRpcEventInfo(rpc::Trace::JsRpcEventInfo::Reader reader)
    : methodName(kj::str(reader.getMethodName())) {}

void Trace::JsRpcEventInfo::copyTo(rpc::Trace::JsRpcEventInfo::Builder builder) {
  builder.setMethodName(methodName);
}

Trace::ScheduledEventInfo::ScheduledEventInfo(double scheduledTime, kj::String cron)
    : scheduledTime(scheduledTime),
      cron(kj::mv(cron)) {}

Trace::ScheduledEventInfo::ScheduledEventInfo(rpc::Trace::ScheduledEventInfo::Reader reader)
    : scheduledTime(reader.getScheduledTime()),
      cron(kj::str(reader.getCron())) {}

void Trace::ScheduledEventInfo::copyTo(rpc::Trace::ScheduledEventInfo::Builder builder) {
  builder.setScheduledTime(scheduledTime);
  builder.setCron(cron);
}

Trace::AlarmEventInfo::AlarmEventInfo(kj::Date scheduledTime): scheduledTime(scheduledTime) {}

Trace::AlarmEventInfo::AlarmEventInfo(rpc::Trace::AlarmEventInfo::Reader reader)
    : scheduledTime(reader.getScheduledTimeMs() * kj::MILLISECONDS + kj::UNIX_EPOCH) {}

void Trace::AlarmEventInfo::copyTo(rpc::Trace::AlarmEventInfo::Builder builder) {
  builder.setScheduledTimeMs((scheduledTime - kj::UNIX_EPOCH) / kj::MILLISECONDS);
}

Trace::QueueEventInfo::QueueEventInfo(kj::String queueName, uint32_t batchSize)
    : queueName(kj::mv(queueName)),
      batchSize(batchSize) {}

Trace::QueueEventInfo::QueueEventInfo(rpc::Trace::QueueEventInfo::Reader reader)
    : queueName(kj::heapString(reader.getQueueName())),
      batchSize(reader.getBatchSize()) {}

void Trace::QueueEventInfo::copyTo(rpc::Trace::QueueEventInfo::Builder builder) {
  builder.setQueueName(queueName);
  builder.setBatchSize(batchSize);
}

Trace::EmailEventInfo::EmailEventInfo(kj::String mailFrom, kj::String rcptTo, uint32_t rawSize)
    : mailFrom(kj::mv(mailFrom)),
      rcptTo(kj::mv(rcptTo)),
      rawSize(rawSize) {}

Trace::EmailEventInfo::EmailEventInfo(rpc::Trace::EmailEventInfo::Reader reader)
    : mailFrom(kj::heapString(reader.getMailFrom())),
      rcptTo(kj::heapString(reader.getRcptTo())),
      rawSize(reader.getRawSize()) {}

void Trace::EmailEventInfo::copyTo(rpc::Trace::EmailEventInfo::Builder builder) {
  builder.setMailFrom(mailFrom);
  builder.setRcptTo(rcptTo);
  builder.setRawSize(rawSize);
}

kj::Vector<Trace::TraceEventInfo::TraceItem> getTraceItemsFromTraces(
    kj::ArrayPtr<kj::Own<Trace>> traces) {
  return KJ_MAP(t, traces) -> Trace::TraceEventInfo::TraceItem {
    return Trace::TraceEventInfo::TraceItem(
        t->scriptName.map([](auto& scriptName) { return kj::str(scriptName); }));
  };
}

Trace::TraceEventInfo::TraceEventInfo(kj::ArrayPtr<kj::Own<Trace>> traces)
    : traces(getTraceItemsFromTraces(traces)) {}

kj::Vector<Trace::TraceEventInfo::TraceItem> getTraceItemsFromReader(
    rpc::Trace::TraceEventInfo::Reader reader) {
  return KJ_MAP(r, reader.getTraces()) -> Trace::TraceEventInfo::TraceItem {
    return Trace::TraceEventInfo::TraceItem(r);
  };
}

Trace::TraceEventInfo::TraceEventInfo(rpc::Trace::TraceEventInfo::Reader reader)
    : traces(getTraceItemsFromReader(reader)) {}

void Trace::TraceEventInfo::copyTo(rpc::Trace::TraceEventInfo::Builder builder) {
  auto list = builder.initTraces(traces.size());
  for (auto i: kj::indices(traces)) {
    traces[i].copyTo(list[i]);
  }
}

Trace::TraceEventInfo::TraceItem::TraceItem(kj::Maybe<kj::String> scriptName)
    : scriptName(kj::mv(scriptName)) {}

Trace::TraceEventInfo::TraceItem::TraceItem(rpc::Trace::TraceEventInfo::TraceItem::Reader reader)
    : scriptName(kj::str(reader.getScriptName())) {}

void Trace::TraceEventInfo::TraceItem::copyTo(
    rpc::Trace::TraceEventInfo::TraceItem::Builder builder) {
  KJ_IF_SOME(name, scriptName) {
    builder.setScriptName(name);
  }
}

Trace::DiagnosticChannelEvent::DiagnosticChannelEvent(
    kj::Date timestamp, kj::String channel, kj::Array<kj::byte> message)
    : timestamp(timestamp),
      channel(kj::mv(channel)),
      message(kj::mv(message)) {}

Trace::DiagnosticChannelEvent::DiagnosticChannelEvent(
    rpc::Trace::DiagnosticChannelEvent::Reader reader)
    : timestamp(kj::UNIX_EPOCH + reader.getTimestampNs() * kj::NANOSECONDS),
      channel(kj::heapString(reader.getChannel())),
      message(kj::heapArray<kj::byte>(reader.getMessage())) {}

void Trace::DiagnosticChannelEvent::copyTo(rpc::Trace::DiagnosticChannelEvent::Builder builder) {
  builder.setTimestampNs((timestamp - kj::UNIX_EPOCH) / kj::NANOSECONDS);
  builder.setChannel(channel);
  builder.setMessage(message);
}

Trace::HibernatableWebSocketEventInfo::HibernatableWebSocketEventInfo(Type type): type(type) {}

Trace::HibernatableWebSocketEventInfo::HibernatableWebSocketEventInfo(
    rpc::Trace::HibernatableWebSocketEventInfo::Reader reader)
    : type(readFrom(reader)) {}

void Trace::HibernatableWebSocketEventInfo::copyTo(
    rpc::Trace::HibernatableWebSocketEventInfo::Builder builder) {
  auto typeBuilder = builder.initType();
  KJ_SWITCH_ONEOF(type) {
    KJ_CASE_ONEOF(_, Message) {
      typeBuilder.setMessage();
    }
    KJ_CASE_ONEOF(close, Close) {
      auto closeBuilder = typeBuilder.initClose();
      closeBuilder.setCode(close.code);
      closeBuilder.setWasClean(close.wasClean);
    }
    KJ_CASE_ONEOF(_, Error) {
      typeBuilder.setError();
    }
  }
}

Trace::HibernatableWebSocketEventInfo::Type Trace::HibernatableWebSocketEventInfo::readFrom(
    rpc::Trace::HibernatableWebSocketEventInfo::Reader reader) {
  auto type = reader.getType();
  switch (type.which()) {
    case rpc::Trace::HibernatableWebSocketEventInfo::Type::MESSAGE: {
      return Message{};
    }
    case rpc::Trace::HibernatableWebSocketEventInfo::Type::CLOSE: {
      auto close = type.getClose();
      return Close{
        .code = close.getCode(),
        .wasClean = close.getWasClean(),
      };
    }
    case rpc::Trace::HibernatableWebSocketEventInfo::Type::ERROR: {
      return Error{};
    }
  }
}

Trace::FetchResponseInfo::FetchResponseInfo(uint16_t statusCode): statusCode(statusCode) {}

Trace::FetchResponseInfo::FetchResponseInfo(rpc::Trace::FetchResponseInfo::Reader reader)
    : statusCode(reader.getStatusCode()) {}

void Trace::FetchResponseInfo::copyTo(rpc::Trace::FetchResponseInfo::Builder builder) {
  builder.setStatusCode(statusCode);
}

Trace::Log::Log(kj::Date timestamp, LogLevel logLevel, kj::String message)
    : timestamp(timestamp),
      logLevel(logLevel),
      message(kj::mv(message)) {}

Trace::Exception::Exception(
    kj::Date timestamp, kj::String name, kj::String message, kj::Maybe<kj::String> stack)
    : timestamp(timestamp),
      name(kj::mv(name)),
      message(kj::mv(message)),
      stack(kj::mv(stack)) {}

Trace::Trace(kj::Maybe<kj::String> stableId,
    kj::Maybe<kj::String> scriptName,
    kj::Maybe<kj::Own<ScriptVersion::Reader>> scriptVersion,
    kj::Maybe<kj::String> dispatchNamespace,
    kj::Maybe<kj::String> scriptId,
    kj::Array<kj::String> scriptTags,
    kj::Maybe<kj::String> entrypoint)
    : stableId(kj::mv(stableId)),
      scriptName(kj::mv(scriptName)),
      scriptVersion(kj::mv(scriptVersion)),
      dispatchNamespace(kj::mv(dispatchNamespace)),
      scriptId(kj::mv(scriptId)),
      scriptTags(kj::mv(scriptTags)),
      entrypoint(kj::mv(entrypoint)) {}
Trace::Trace(rpc::Trace::Reader reader) {
  mergeFrom(reader, PipelineLogLevel::FULL);
}

Trace::~Trace() noexcept(false) {}

void Trace::copyTo(rpc::Trace::Builder builder) {
  {
    auto list = builder.initLogs(logs.size() + spans.size());
    for (auto i: kj::indices(logs)) {
      logs[i].copyTo(list[i]);
    }
    // Add spans represented as logs to the logs object.
    for (auto i: kj::indices(spans)) {
      spans[i].copyTo(list[i + logs.size()]);
    }
  }

  {
    auto list = builder.initExceptions(exceptions.size());
    for (auto i: kj::indices(exceptions)) {
      exceptions[i].copyTo(list[i]);
    }
  }

  builder.setTruncated(truncated);
  builder.setOutcome(outcome);
  builder.setCpuTime(cpuTime / kj::MILLISECONDS);
  builder.setWallTime(wallTime / kj::MILLISECONDS);
  KJ_IF_SOME(name, scriptName) {
    builder.setScriptName(name);
  }
  KJ_IF_SOME(version, scriptVersion) {
    builder.setScriptVersion(*version);
  }
  KJ_IF_SOME(id, scriptId) {
    builder.setScriptId(id);
  }
  KJ_IF_SOME(ns, dispatchNamespace) {
    builder.setDispatchNamespace(ns);
  }

  {
    auto list = builder.initScriptTags(scriptTags.size());
    for (auto i: kj::indices(scriptTags)) {
      list.set(i, scriptTags[i]);
    }
  }

  KJ_IF_SOME(e, entrypoint) {
    builder.setEntrypoint(e);
  }

  builder.setEventTimestampNs((eventTimestamp - kj::UNIX_EPOCH) / kj::NANOSECONDS);

  auto eventInfoBuilder = builder.initEventInfo();
  KJ_IF_SOME(e, eventInfo) {
    KJ_SWITCH_ONEOF(e) {
      KJ_CASE_ONEOF(fetch, FetchEventInfo) {
        auto fetchBuilder = eventInfoBuilder.initFetch();
        fetch.copyTo(fetchBuilder);
      }
      KJ_CASE_ONEOF(jsRpc, JsRpcEventInfo) {
        auto jsRpcBuilder = eventInfoBuilder.initJsRpc();
        jsRpc.copyTo(jsRpcBuilder);
      }
      KJ_CASE_ONEOF(scheduled, ScheduledEventInfo) {
        auto scheduledBuilder = eventInfoBuilder.initScheduled();
        scheduled.copyTo(scheduledBuilder);
      }
      KJ_CASE_ONEOF(alarm, AlarmEventInfo) {
        auto alarmBuilder = eventInfoBuilder.initAlarm();
        alarm.copyTo(alarmBuilder);
      }
      KJ_CASE_ONEOF(queue, QueueEventInfo) {
        auto queueBuilder = eventInfoBuilder.initQueue();
        queue.copyTo(queueBuilder);
      }
      KJ_CASE_ONEOF(email, EmailEventInfo) {
        auto emailBuilder = eventInfoBuilder.initEmail();
        email.copyTo(emailBuilder);
      }
      KJ_CASE_ONEOF(trace, TraceEventInfo) {
        auto traceBuilder = eventInfoBuilder.initTrace();
        trace.copyTo(traceBuilder);
      }
      KJ_CASE_ONEOF(hibWs, HibernatableWebSocketEventInfo) {
        auto hibWsBuilder = eventInfoBuilder.initHibernatableWebSocket();
        hibWs.copyTo(hibWsBuilder);
      }
      KJ_CASE_ONEOF(custom, CustomEventInfo) {
        eventInfoBuilder.initCustom();
      }
    }
  } else {
    eventInfoBuilder.setNone();
  }

  KJ_IF_SOME(fetchResponseInfo, this->fetchResponseInfo) {
    auto fetchResponseInfoBuilder = builder.initResponse();
    fetchResponseInfo.copyTo(fetchResponseInfoBuilder);
  }

  {
    auto list = builder.initDiagnosticChannelEvents(diagnosticChannelEvents.size());
    for (auto i: kj::indices(diagnosticChannelEvents)) {
      diagnosticChannelEvents[i].copyTo(list[i]);
    }
  }
}

void Trace::Log::copyTo(rpc::Trace::Log::Builder builder) {
  builder.setTimestampNs((timestamp - kj::UNIX_EPOCH) / kj::NANOSECONDS);
  builder.setLogLevel(logLevel);
  builder.setMessage(message);
}

void Trace::Exception::copyTo(rpc::Trace::Exception::Builder builder) {
  builder.setTimestampNs((timestamp - kj::UNIX_EPOCH) / kj::NANOSECONDS);
  builder.setName(name);
  builder.setMessage(message);
  KJ_IF_SOME(s, stack) {
    builder.setStack(s);
  }
}

void Trace::mergeFrom(rpc::Trace::Reader reader, PipelineLogLevel pipelineLogLevel) {
  // Sandboxed workers currently record their traces as if the pipeline log level were set to
  // "full", so we may need to filter out the extra data after receiving the traces back.
  if (pipelineLogLevel != PipelineLogLevel::NONE) {
    logs.addAll(reader.getLogs());
    exceptions.addAll(reader.getExceptions());
    diagnosticChannelEvents.addAll(reader.getDiagnosticChannelEvents());
  }

  truncated = reader.getTruncated();
  outcome = reader.getOutcome();
  cpuTime = reader.getCpuTime() * kj::MILLISECONDS;
  wallTime = reader.getWallTime() * kj::MILLISECONDS;

  // mergeFrom() is called both when deserializing traces from a sandboxed
  // worker and when deserializing traces sent to a sandboxed trace worker. In
  // the former case, the trace's scriptName (and other fields like
  // scriptVersion) are already set and the deserialized value is missing, so
  // we need to be careful not to overwrite the set value.
  if (reader.hasScriptName()) {
    scriptName = kj::str(reader.getScriptName());
  }

  if (reader.hasScriptVersion()) {
    scriptVersion = capnp::clone(reader.getScriptVersion());
  }

  if (reader.hasScriptId()) {
    scriptId = kj::str(reader.getScriptId());
  }

  if (reader.hasDispatchNamespace()) {
    dispatchNamespace = kj::str(reader.getDispatchNamespace());
  }

  if (auto tags = reader.getScriptTags(); tags.size() > 0) {
    scriptTags = KJ_MAP(tag, tags) { return kj::str(tag); };
  }

  if (reader.hasEntrypoint()) {
    entrypoint = kj::str(reader.getEntrypoint());
  }

  eventTimestamp = kj::UNIX_EPOCH + reader.getEventTimestampNs() * kj::NANOSECONDS;

  if (pipelineLogLevel == PipelineLogLevel::NONE) {
    eventInfo = kj::none;
  } else {
    auto e = reader.getEventInfo();
    switch (e.which()) {
      case rpc::Trace::EventInfo::Which::FETCH:
        eventInfo = FetchEventInfo(e.getFetch());
        break;
      case rpc::Trace::EventInfo::Which::JS_RPC:
        eventInfo = JsRpcEventInfo(e.getJsRpc());
        break;
      case rpc::Trace::EventInfo::Which::SCHEDULED:
        eventInfo = ScheduledEventInfo(e.getScheduled());
        break;
      case rpc::Trace::EventInfo::Which::ALARM:
        eventInfo = AlarmEventInfo(e.getAlarm());
        break;
      case rpc::Trace::EventInfo::Which::QUEUE:
        eventInfo = QueueEventInfo(e.getQueue());
        break;
      case rpc::Trace::EventInfo::Which::EMAIL:
        eventInfo = EmailEventInfo(e.getEmail());
        break;
      case rpc::Trace::EventInfo::Which::TRACE:
        eventInfo = TraceEventInfo(e.getTrace());
        break;
      case rpc::Trace::EventInfo::Which::HIBERNATABLE_WEB_SOCKET:
        eventInfo = HibernatableWebSocketEventInfo(e.getHibernatableWebSocket());
        break;
      case rpc::Trace::EventInfo::Which::CUSTOM:
        eventInfo = CustomEventInfo(e.getCustom());
        break;
      case rpc::Trace::EventInfo::Which::NONE:
        eventInfo = kj::none;
        break;
    }
  }

  if (reader.hasResponse()) {
    fetchResponseInfo = FetchResponseInfo(reader.getResponse());
  }
}

Trace::Log::Log(rpc::Trace::Log::Reader reader)
    : timestamp(kj::UNIX_EPOCH + reader.getTimestampNs() * kj::NANOSECONDS),
      logLevel(reader.getLogLevel()),
      message(kj::str(reader.getMessage())) {}
Trace::Exception::Exception(rpc::Trace::Exception::Reader reader)
    : timestamp(kj::UNIX_EPOCH + reader.getTimestampNs() * kj::NANOSECONDS),
      name(kj::str(reader.getName())),
      message(kj::str(reader.getMessage())) {
  if (reader.hasStack()) {
    stack = kj::str(reader.getStack());
  }
}

SpanBuilder& SpanBuilder::operator=(SpanBuilder&& other) {
  end();
  observer = kj::mv(other.observer);
  span = kj::mv(other.span);
  return *this;
}

SpanBuilder::~SpanBuilder() noexcept(false) {
  end();
}

void SpanBuilder::end() {
  KJ_IF_SOME(o, observer) {
    KJ_IF_SOME(s, span) {
      s.endTime = kj::systemPreciseCalendarClock().now();
      o->report(s);
      span = kj::none;
    }
  }
}

void SpanBuilder::setOperationName(kj::ConstString operationName) {
  KJ_IF_SOME(s, span) {
    s.operationName = kj::mv(operationName);
  }
}

void SpanBuilder::setTag(kj::ConstString key, TagValue value) {
  KJ_IF_SOME(s, span) {
    auto keyPtr = key.asPtr();
    s.tags.upsert(
        kj::mv(key), kj::mv(value), [keyPtr](TagValue& existingValue, TagValue&& newValue) {
      // This is a programming error, but not a serious one. We could alternatively just emit
      // duplicate tags and leave the Jaeger UI in charge of warning about them.
      [[maybe_unused]] static auto logged = [keyPtr]() {
        KJ_LOG(WARNING, "overwriting previous tag", keyPtr);
        return true;
      }();
      existingValue = kj::mv(newValue);
    });
  }
}

void SpanBuilder::addLog(kj::Date timestamp, kj::ConstString key, TagValue value) {
  KJ_IF_SOME(s, span) {
    if (s.logs.size() >= Span::MAX_LOGS) {
      ++s.droppedLogs;
    } else {
      s.logs.add(Span::Log{.timestamp = timestamp,
        .tag = {
          .key = kj::mv(key),
          .value = kj::mv(value),
        }});
    }
  }
}

PipelineTracer::~PipelineTracer() noexcept(false) {
  KJ_IF_SOME(p, parentTracer) {
    for (auto& t: traces) {
      p->traces.add(kj::addRef(*t));
    }
  }
  KJ_IF_SOME(f, completeFulfiller) {
    f.get()->fulfill(traces.releaseAsArray());
  }
}

kj::Promise<kj::Array<kj::Own<Trace>>> PipelineTracer::onComplete() {
  KJ_REQUIRE(completeFulfiller == kj::none, "onComplete() can only be called once");

  auto paf = kj::newPromiseAndFulfiller<kj::Array<kj::Own<Trace>>>();
  completeFulfiller = kj::mv(paf.fulfiller);
  return kj::mv(paf.promise);
}

kj::Own<WorkerTracer> PipelineTracer::makeWorkerTracer(PipelineLogLevel pipelineLogLevel,
    kj::Maybe<kj::String> scriptId,
    kj::Maybe<kj::String> stableId,
    kj::Maybe<kj::String> scriptName,
    kj::Maybe<kj::Own<ScriptVersion::Reader>> scriptVersion,
    kj::Maybe<kj::String> dispatchNamespace,
    kj::Array<kj::String> scriptTags,
    kj::Maybe<kj::String> entrypoint) {
  auto trace = kj::refcounted<Trace>(kj::mv(stableId), kj::mv(scriptName), kj::mv(scriptVersion),
      kj::mv(dispatchNamespace), kj::mv(scriptId), kj::mv(scriptTags), kj::mv(entrypoint));
  traces.add(kj::addRef(*trace));
  return kj::refcounted<WorkerTracer>(kj::addRef(*this), kj::mv(trace), pipelineLogLevel);
}

void PipelineTracer::addTrace(rpc::Trace::Reader reader) {
  traces.add(kj::refcounted<Trace>(reader));
}

WorkerTracer::WorkerTracer(
    kj::Own<PipelineTracer> parentPipeline, kj::Own<Trace> trace, PipelineLogLevel pipelineLogLevel)
    : pipelineLogLevel(pipelineLogLevel),
      trace(kj::mv(trace)),
      parentPipeline(kj::mv(parentPipeline)),
      self(kj::refcounted<WeakRef<WorkerTracer>>(kj::Badge<WorkerTracer>{}, *this)) {}
WorkerTracer::WorkerTracer(PipelineLogLevel pipelineLogLevel)
    : pipelineLogLevel(pipelineLogLevel),
      trace(kj::refcounted<Trace>(
          kj::none, kj::none, kj::none, kj::none, kj::none, nullptr, kj::none)),
      self(kj::refcounted<WeakRef<WorkerTracer>>(kj::Badge<WorkerTracer>{}, *this)) {}

void WorkerTracer::log(kj::Date timestamp, LogLevel logLevel, kj::String message, bool isSpan) {
  if (trace->exceededLogLimit) {
    return;
  }
  if (pipelineLogLevel == PipelineLogLevel::NONE) {
    return;
  }
  size_t newSize = trace->bytesUsed + sizeof(Trace::Log) + message.size();
  if (newSize > MAX_TRACE_BYTES) {
    trace->exceededLogLimit = true;
    trace->truncated = true;
    // We use a JSON encoded array/string to match other console.log() recordings:
    trace->logs.add(timestamp, LogLevel::WARN,
        kj::str(
            "[\"Log size limit exceeded: More than 128KB of data (across console.log statements, exception, request metadata and headers) was logged during a single request. Subsequent data for this request will not be recorded in logs, appear when tailing this Worker's logs, or in Tail Workers.\"]"));
    return;
  }
  trace->bytesUsed = newSize;
  if (isSpan) {
    trace->spans.add(timestamp, logLevel, kj::mv(message));
    trace->numSpans++;
    return;
  }
  trace->logs.add(timestamp, logLevel, kj::mv(message));
}

void WorkerTracer::addSpan(const Span& span, kj::String spanContext) {
  // This is where we'll actually encode the span for now.
  // Drop any spans beyond MAX_LIME_SPANS.
  if (trace->numSpans >= MAX_LIME_SPANS) {
    return;
  }
  if (isPredictableModeForTest()) {
    // Do not emit span duration information in predictable mode.
    log(span.endTime, LogLevel::LOG, kj::str("[\"span: ", span.operationName, "\"]"), true);
  } else {
    // Time since Unix epoch in seconds, with millisecond precision
    double epochSecondsStart = (span.startTime - kj::UNIX_EPOCH) / kj::MILLISECONDS / 1000.0;
    double epochSecondsEnd = (span.endTime - kj::UNIX_EPOCH) / kj::MILLISECONDS / 1000.0;
    auto message = kj::str("[\"span: ", span.operationName, " ", kj::mv(spanContext), " ",
        epochSecondsStart, " ", epochSecondsEnd, "\"]");
    log(span.endTime, LogLevel::LOG, kj::mv(message), true);
  }

  // TODO(cleanup): Create a function in kj::OneOf to automatically convert to a given type (i.e
  // String) to avoid having to handle each type explicitly here.
  for (const Span::TagMap::Entry& tag: span.tags) {
    auto value = [&]() {
      KJ_SWITCH_ONEOF(tag.value) {
        KJ_CASE_ONEOF(str, kj::String) {
          return kj::str(str);
        }
        KJ_CASE_ONEOF(val, int64_t) {
          return kj::str(val);
        }
        KJ_CASE_ONEOF(val, double) {
          return kj::str(val);
        }
        KJ_CASE_ONEOF(val, bool) {
          return kj::str(val);
        }
      }
      KJ_UNREACHABLE;
    }();
    kj::String message = kj::str("[\"tag: "_kj, tag.key, " => "_kj, value, "\"]");
    log(span.endTime, LogLevel::LOG, kj::mv(message), true);
  }
}

void WorkerTracer::addException(
    kj::Date timestamp, kj::String name, kj::String message, kj::Maybe<kj::String> stack) {
  if (trace->exceededExceptionLimit) {
    return;
  }
  // TODO(someday): For now, we're using logLevel == none as a hint to avoid doing anything
  //   expensive while tracing.  We may eventually want separate configuration for exceptions vs.
  //   logs.
  if (pipelineLogLevel == PipelineLogLevel::NONE) {
    return;
  }
  size_t newSize = trace->bytesUsed + sizeof(Trace::Exception) + name.size() + message.size();
  KJ_IF_SOME(s, stack) {
    newSize += s.size();
  }
  if (newSize > MAX_TRACE_BYTES) {
    trace->exceededExceptionLimit = true;
    trace->truncated = true;
    trace->exceptions.add(timestamp, kj::str("Error"),
        kj::str("Trace resource limit exceeded; subsequent exceptions not recorded."), kj::none);
    return;
  }
  trace->bytesUsed = newSize;
  trace->exceptions.add(timestamp, kj::mv(name), kj::mv(message), kj::mv(stack));
}

void WorkerTracer::addDiagnosticChannelEvent(
    kj::Date timestamp, kj::String channel, kj::Array<kj::byte> message) {
  if (trace->exceededDiagnosticChannelEventLimit) {
    return;
  }
  if (pipelineLogLevel == PipelineLogLevel::NONE) {
    return;
  }
  size_t newSize =
      trace->bytesUsed + sizeof(Trace::DiagnosticChannelEvent) + channel.size() + message.size();
  if (newSize > MAX_TRACE_BYTES) {
    trace->exceededDiagnosticChannelEventLimit = true;
    trace->truncated = true;
    trace->diagnosticChannelEvents.add(
        timestamp, kj::str("workerd.LimitExceeded"), kj::Array<kj::byte>());
    return;
  }
  trace->bytesUsed = newSize;
  trace->diagnosticChannelEvents.add(timestamp, kj::mv(channel), kj::mv(message));
}

void WorkerTracer::setEventInfo(kj::Date timestamp, Trace::EventInfo&& info) {
  KJ_ASSERT(trace->eventInfo == kj::none, "tracer can only be used for a single event");

  // TODO(someday): For now, we're using logLevel == none as a hint to avoid doing anything
  //   expensive while tracing.  We may eventually want separate configuration for event info vs.
  //   logs.
  // TODO(perf): Find a way to allow caller to avoid the cost of generation if the info struct
  //   won't be used?
  if (pipelineLogLevel == PipelineLogLevel::NONE) {
    return;
  }

  trace->eventTimestamp = timestamp;

  size_t newSize = trace->bytesUsed;
  KJ_SWITCH_ONEOF(info) {
    KJ_CASE_ONEOF(fetch, Trace::FetchEventInfo) {
      newSize += fetch.url.size();
      for (const auto& header: fetch.headers) {
        newSize += header.name.size() + header.value.size();
      }
      newSize += fetch.cfJson.size();
      if (newSize > MAX_TRACE_BYTES) {
        trace->truncated = true;
        trace->logs.add(timestamp, LogLevel::WARN,
            kj::str("[\"Trace resource limit exceeded; could not capture event info.\"]"));
        trace->eventInfo = Trace::FetchEventInfo(fetch.method, {}, {}, {});
        return;
      }
    }
    KJ_CASE_ONEOF(_, Trace::JsRpcEventInfo) {}
    KJ_CASE_ONEOF(_, Trace::ScheduledEventInfo) {}
    KJ_CASE_ONEOF(_, Trace::AlarmEventInfo) {}
    KJ_CASE_ONEOF(_, Trace::QueueEventInfo) {}
    KJ_CASE_ONEOF(_, Trace::EmailEventInfo) {}
    KJ_CASE_ONEOF(_, Trace::TraceEventInfo) {}
    KJ_CASE_ONEOF(_, Trace::HibernatableWebSocketEventInfo) {}
    KJ_CASE_ONEOF(_, Trace::CustomEventInfo) {}
  }
  trace->bytesUsed = newSize;
  trace->eventInfo = kj::mv(info);
}

void WorkerTracer::setOutcome(EventOutcome outcome) {
  trace->outcome = outcome;
}

void WorkerTracer::setCPUTime(kj::Duration cpuTime) {
  trace->cpuTime = cpuTime;
}

void WorkerTracer::setWallTime(kj::Duration wallTime) {
  trace->wallTime = wallTime;
}

void WorkerTracer::setFetchResponseInfo(Trace::FetchResponseInfo&& info) {
  // Match the behavior of setEventInfo(). Any resolution of the TODO comments
  // in setEventInfo() that are related to this check while probably also affect
  // this function.
  if (pipelineLogLevel == PipelineLogLevel::NONE) {
    return;
  }

  KJ_REQUIRE(KJ_REQUIRE_NONNULL(trace->eventInfo).is<Trace::FetchEventInfo>());
  KJ_ASSERT(trace->fetchResponseInfo == kj::none, "setFetchResponseInfo can only be called once");
  trace->fetchResponseInfo = kj::mv(info);
}

void WorkerTracer::extractTrace(rpc::Trace::Builder builder) {
  trace->copyTo(builder);
}

void WorkerTracer::setTrace(rpc::Trace::Reader reader) {
  trace->mergeFrom(reader, pipelineLogLevel);
}

}  // namespace workerd
