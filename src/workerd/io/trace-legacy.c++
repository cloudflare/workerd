#include "trace-legacy.h"

#include <capnp/message.h>
#include <capnp/schema.h>

namespace workerd {

Trace::Trace(trace::OnsetInfo&& onset): onsetInfo(kj::mv(onset)) {}
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

}  // namespace workerd
