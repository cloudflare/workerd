// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "trace-legacy.h"

#include <workerd/util/thread-scopes.h>

#include <capnp/message.h>
#include <capnp/schema.h>

namespace workerd {

namespace {
// Approximately how much external data we allow in a trace before we start ignoring requests.  We
// want this number to be big enough to be useful for tracing, but small enough to make it hard to
// DoS the C++ heap -- keeping in mind we can record a trace per handler run during a request.
static constexpr size_t MAX_TRACE_BYTES = 128 * 1024;
// Limit spans to at most 512, it could be difficult to fit e.g. 1024 spans within MAX_TRACE_BYTES
// unless most of the included spans do not include tags. If use cases arise where this amount is
// insufficient, merge smaller spans together or drop smaller spans.
static constexpr size_t MAX_USER_SPANS = 512;
}  // namespace

Trace::Trace(trace::Onset&& onset): onsetInfo(kj::mv(onset)) {}
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
  builder.setOutcome(outcomeInfo.outcome);
  builder.setCpuTime(cpuTime / kj::MILLISECONDS);
  builder.setWallTime(wallTime / kj::MILLISECONDS);
  KJ_IF_SOME(name, onsetInfo.scriptName) {
    builder.setScriptName(name);
  }
  KJ_IF_SOME(version, onsetInfo.scriptVersion) {
    builder.setScriptVersion(*version);
  }
  KJ_IF_SOME(id, onsetInfo.scriptId) {
    builder.setScriptId(id);
  }
  KJ_IF_SOME(ns, onsetInfo.dispatchNamespace) {
    builder.setDispatchNamespace(ns);
  }

  {
    auto list = builder.initScriptTags(onsetInfo.scriptTags.size());
    for (auto i: kj::indices(onsetInfo.scriptTags)) {
      list.set(i, onsetInfo.scriptTags[i]);
    }
  }

  KJ_IF_SOME(e, onsetInfo.entrypoint) {
    builder.setEntrypoint(e);
  }
  builder.setExecutionModel(onsetInfo.executionModel);

  builder.setEventTimestampNs((eventTimestamp - kj::UNIX_EPOCH) / kj::NANOSECONDS);

  auto eventInfoBuilder = builder.initEventInfo();
  KJ_IF_SOME(e, eventInfo) {
    KJ_SWITCH_ONEOF(e) {
      KJ_CASE_ONEOF(fetch, trace::FetchEventInfo) {
        auto fetchBuilder = eventInfoBuilder.initFetch();
        fetch.copyTo(fetchBuilder);
      }
      KJ_CASE_ONEOF(jsRpc, trace::JsRpcEventInfo) {
        auto jsRpcBuilder = eventInfoBuilder.initJsRpc();
        jsRpc.copyTo(jsRpcBuilder);
      }
      KJ_CASE_ONEOF(scheduled, trace::ScheduledEventInfo) {
        auto scheduledBuilder = eventInfoBuilder.initScheduled();
        scheduled.copyTo(scheduledBuilder);
      }
      KJ_CASE_ONEOF(alarm, trace::AlarmEventInfo) {
        auto alarmBuilder = eventInfoBuilder.initAlarm();
        alarm.copyTo(alarmBuilder);
      }
      KJ_CASE_ONEOF(queue, trace::QueueEventInfo) {
        auto queueBuilder = eventInfoBuilder.initQueue();
        queue.copyTo(queueBuilder);
      }
      KJ_CASE_ONEOF(email, trace::EmailEventInfo) {
        auto emailBuilder = eventInfoBuilder.initEmail();
        email.copyTo(emailBuilder);
      }
      KJ_CASE_ONEOF(trace, trace::TraceEventInfo) {
        auto traceBuilder = eventInfoBuilder.initTrace();
        trace.copyTo(traceBuilder);
      }
      KJ_CASE_ONEOF(hibWs, trace::HibernatableWebSocketEventInfo) {
        auto hibWsBuilder = eventInfoBuilder.initHibernatableWebSocket();
        hibWs.copyTo(hibWsBuilder);
      }
      KJ_CASE_ONEOF(custom, trace::CustomEventInfo) {
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

void Trace::mergeFrom(rpc::Trace::Reader reader, PipelineLogLevel pipelineLogLevel) {
  // Sandboxed workers currently record their traces as if the pipeline log level were set to
  // "full", so we may need to filter out the extra data after receiving the traces back.
  if (pipelineLogLevel != PipelineLogLevel::NONE) {
    logs.addAll(reader.getLogs());
    exceptions.addAll(reader.getExceptions());
    diagnosticChannelEvents.addAll(reader.getDiagnosticChannelEvents());
  }

  truncated = reader.getTruncated();
  outcomeInfo.outcome = reader.getOutcome();
  cpuTime = reader.getCpuTime() * kj::MILLISECONDS;
  wallTime = reader.getWallTime() * kj::MILLISECONDS;

  // mergeFrom() is called both when deserializing traces from a sandboxed
  // worker and when deserializing traces sent to a sandboxed trace worker. In
  // the former case, the trace's scriptName (and other fields like
  // scriptVersion) are already set and the deserialized value is missing, so
  // we need to be careful not to overwrite the set value.
  if (reader.hasScriptName()) {
    onsetInfo.scriptName = kj::str(reader.getScriptName());
  }

  if (reader.hasScriptVersion()) {
    onsetInfo.scriptVersion = capnp::clone(reader.getScriptVersion());
  }

  if (reader.hasScriptId()) {
    onsetInfo.scriptId = kj::str(reader.getScriptId());
  }

  if (reader.hasDispatchNamespace()) {
    onsetInfo.dispatchNamespace = kj::str(reader.getDispatchNamespace());
  }

  if (auto tags = reader.getScriptTags(); tags.size() > 0) {
    onsetInfo.scriptTags = KJ_MAP(tag, tags) { return kj::str(tag); };
  }

  if (reader.hasEntrypoint()) {
    onsetInfo.entrypoint = kj::str(reader.getEntrypoint());
  }
  onsetInfo.executionModel = reader.getExecutionModel();

  eventTimestamp = kj::UNIX_EPOCH + reader.getEventTimestampNs() * kj::NANOSECONDS;

  if (pipelineLogLevel == PipelineLogLevel::NONE) {
    eventInfo = kj::none;
  } else {
    auto e = reader.getEventInfo();
    switch (e.which()) {
      case rpc::Trace::EventInfo::Which::FETCH:
        eventInfo = trace::FetchEventInfo(e.getFetch());
        break;
      case rpc::Trace::EventInfo::Which::JS_RPC:
        eventInfo = trace::JsRpcEventInfo(e.getJsRpc());
        break;
      case rpc::Trace::EventInfo::Which::SCHEDULED:
        eventInfo = trace::ScheduledEventInfo(e.getScheduled());
        break;
      case rpc::Trace::EventInfo::Which::ALARM:
        eventInfo = trace::AlarmEventInfo(e.getAlarm());
        break;
      case rpc::Trace::EventInfo::Which::QUEUE:
        eventInfo = trace::QueueEventInfo(e.getQueue());
        break;
      case rpc::Trace::EventInfo::Which::EMAIL:
        eventInfo = trace::EmailEventInfo(e.getEmail());
        break;
      case rpc::Trace::EventInfo::Which::TRACE:
        eventInfo = trace::TraceEventInfo(e.getTrace());
        break;
      case rpc::Trace::EventInfo::Which::HIBERNATABLE_WEB_SOCKET:
        eventInfo = trace::HibernatableWebSocketEventInfo(e.getHibernatableWebSocket());
        break;
      case rpc::Trace::EventInfo::Which::CUSTOM:
        eventInfo = trace::CustomEventInfo(e.getCustom());
        break;
      case rpc::Trace::EventInfo::Which::NONE:
        eventInfo = kj::none;
        break;
    }
  }

  if (reader.hasResponse()) {
    fetchResponseInfo = trace::FetchResponseInfo(reader.getResponse());
  }
}

void Trace::setEventInfo(kj::Date timestamp, trace::EventInfo&& info) {
  KJ_ASSERT(eventInfo == kj::none, "tracer can only be used for a single event");
  eventTimestamp = timestamp;

  size_t newSize = bytesUsed;
  KJ_SWITCH_ONEOF(info) {
    KJ_CASE_ONEOF(fetch, trace::FetchEventInfo) {
      newSize += fetch.url.size();
      for (const auto& header: fetch.headers) {
        newSize += header.name.size() + header.value.size();
      }
      newSize += fetch.cfJson.size();
      if (newSize > MAX_TRACE_BYTES) {
        truncated = true;
        logs.add(timestamp, LogLevel::WARN,
            kj::str("[\"Trace resource limit exceeded; could not capture event info.\"]"));
        eventInfo = trace::FetchEventInfo(fetch.method, {}, {}, {});
        return;
      }
    }
    KJ_CASE_ONEOF(_, trace::JsRpcEventInfo) {}
    KJ_CASE_ONEOF(_, trace::ScheduledEventInfo) {}
    KJ_CASE_ONEOF(_, trace::AlarmEventInfo) {}
    KJ_CASE_ONEOF(_, trace::QueueEventInfo) {}
    KJ_CASE_ONEOF(_, trace::EmailEventInfo) {}
    KJ_CASE_ONEOF(_, trace::TraceEventInfo) {}
    KJ_CASE_ONEOF(_, trace::HibernatableWebSocketEventInfo) {}
    KJ_CASE_ONEOF(_, trace::CustomEventInfo) {}
  }
  bytesUsed = newSize;
  eventInfo = kj::mv(info);
}

void Trace::setOutcome(trace::Outcome&& info) {
  outcomeInfo = kj::mv(info);
}

void Trace::addLog(trace::Log&& log, bool isSpan) {
  if (exceededLogLimit) {
    return;
  }
  size_t newSize = bytesUsed + sizeof(trace::Log) + log.message.size();
  if (newSize > MAX_TRACE_BYTES) {
    exceededLogLimit = true;
    truncated = true;
    // We use a JSON encoded array/string to match other console.log() recordings:
    logs.add(log.timestamp, LogLevel::WARN,
        kj::str(
            "[\"Log size limit exceeded: More than 128KB of data (across console.log statements, exception, request metadata and headers) was logged during a single request. Subsequent data for this request will not be recorded in logs, appear when tailing this Worker's logs, or in Tail Workers.\"]"));
    return;
  }
  bytesUsed = newSize;
  if (isSpan) {
    spans.add(kj::mv(log));
    numSpans++;
    return;
  }
  logs.add(kj::mv(log));
}

void Trace::addException(trace::Exception&& exception) {
  if (exceededExceptionLimit) {
    return;
  }
  size_t newSize =
      bytesUsed + sizeof(trace::Exception) + exception.name.size() + exception.message.size();
  KJ_IF_SOME(s, exception.stack) {
    newSize += s.size();
  }
  if (newSize > MAX_TRACE_BYTES) {
    exceededExceptionLimit = true;
    truncated = true;
    exceptions.add(exception.timestamp, kj::str("Error"),
        kj::str("Trace resource limit exceeded; subsequent exceptions not recorded."), kj::none);
    return;
  }
  bytesUsed = newSize;
  exceptions.add(kj::mv(exception));
}

void Trace::addDiagnosticChannelEvent(trace::DiagnosticChannelEvent&& event) {
  if (exceededDiagnosticChannelEventLimit) {
    return;
  }
  size_t newSize = bytesUsed + sizeof(trace::DiagnosticChannelEvent) + event.channel.size() +
      event.message.size();
  if (newSize > MAX_TRACE_BYTES) {
    exceededDiagnosticChannelEventLimit = true;
    truncated = true;
    diagnosticChannelEvents.add(
        event.timestamp, kj::str("workerd.LimitExceeded"), kj::Array<kj::byte>());
    return;
  }
  bytesUsed = newSize;
  diagnosticChannelEvents.add(kj::mv(event));
}

void Trace::addSpan(const Span&& span, kj::String spanContext) {
  // This is where we'll actually encode the span for now.
  // Drop any spans beyond MAX_USER_SPANS.
  if (numSpans >= MAX_USER_SPANS) {
    return;
  }
  if (isPredictableModeForTest()) {
    // Do not emit span duration information in predictable mode.
    addLog(trace::Log(span.endTime, LogLevel::LOG, kj::str("[\"span: ", span.operationName, "\"]")),
        true);
  } else {
    // Time since Unix epoch in seconds, with millisecond precision
    double epochSecondsStart = (span.startTime - kj::UNIX_EPOCH) / kj::MILLISECONDS / 1000.0;
    double epochSecondsEnd = (span.endTime - kj::UNIX_EPOCH) / kj::MILLISECONDS / 1000.0;
    auto message = kj::str("[\"span: ", span.operationName, " ", kj::mv(spanContext), " ",
        epochSecondsStart, " ", epochSecondsEnd, "\"]");
    addLog(trace::Log(span.endTime, LogLevel::LOG, kj::mv(message)), true);
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
    addLog(trace::Log(span.endTime, LogLevel::LOG, kj::mv(message)), true);
  }
}

void Trace::setFetchResponseInfo(trace::FetchResponseInfo&& info) {
  KJ_REQUIRE(KJ_REQUIRE_NONNULL(eventInfo).is<trace::FetchEventInfo>());
  KJ_ASSERT(fetchResponseInfo == kj::none, "setFetchResponseInfo can only be called once");
  fetchResponseInfo = kj::mv(info);
}

void Trace::addMetrics(trace::Metrics&& metrics) {
  for (auto& metric: metrics) {
    if (metric.keyMatches(trace::Metric::Common::CPU_TIME)) {
      // The CPU_TIME metric will always be a int64_t converted from a kj::Duration
      // If it's not, we'll ignore it.
      cpuTime = static_cast<int64_t>(metric.value) * kj::MILLISECONDS;
    } else if (metric.keyMatches(trace::Metric::Common::WALL_TIME)) {
      // The WALL_TIME metric will always be a int64_t converted from a kj::Duration
      // If it's not, we'll ignore it.
      wallTime = static_cast<int64_t>(metric.value) * kj::MILLISECONDS;
    }
  }
}

}  // namespace workerd
