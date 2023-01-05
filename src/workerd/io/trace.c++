// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "trace.h"
#include <capnp/schema.h>
#include <kj/compat/http.h>
#include <kj/debug.h>
#include <cstdlib>
#include <workerd/util/thread-scopes.h>

namespace workerd {

static constexpr size_t MAX_TRACE_BYTES = 128 * 1024;
// Approximately how much external data we allow in a trace before we start ignoring requests.  We
// want this number to be big enough to be useful for tracing, but small enough to make it hard to
// DoS the C++ heap -- keeping in mind we can record a trace per handler run during a request.

namespace {

static kj::HttpMethod validateMethod(capnp::HttpMethod method) {
  KJ_REQUIRE(method <= capnp::HttpMethod::UNSUBSCRIBE, "unknown method", method);
  return static_cast<kj::HttpMethod>(method);
}

} // namespace

Trace::FetchEventInfo::FetchEventInfo(kj::HttpMethod method, kj::String url, kj::String cfJson,
    kj::Array<Header> headers)
    : method(method), url(kj::mv(url)), cfJson(kj::mv(cfJson)), headers(kj::mv(headers)) {}

Trace::FetchEventInfo::FetchEventInfo(rpc::Trace::FetchEventInfo::Reader reader)
    : method(validateMethod(reader.getMethod())),
      url(kj::str(reader.getUrl())),
      cfJson(kj::str(reader.getCfJson()))
{
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
    : name(kj::mv(name)), value(kj::mv(value)) {}

Trace::FetchEventInfo::Header::Header(rpc::Trace::FetchEventInfo::Header::Reader reader)
    : name(kj::str(reader.getName())), value(kj::str(reader.getValue())) {}

void Trace::FetchEventInfo::Header::copyTo(rpc::Trace::FetchEventInfo::Header::Builder builder) {
  builder.setName(name);
  builder.setValue(value);
}

Trace::ScheduledEventInfo::ScheduledEventInfo(double scheduledTime, kj::String cron)
    : scheduledTime(scheduledTime), cron(kj::mv(cron)) {}

Trace::ScheduledEventInfo::ScheduledEventInfo(rpc::Trace::ScheduledEventInfo::Reader reader)
    : scheduledTime(reader.getScheduledTime()), cron(kj::str(reader.getCron())) {}

void Trace::ScheduledEventInfo::copyTo(rpc::Trace::ScheduledEventInfo::Builder builder) {
  builder.setScheduledTime(scheduledTime);
  builder.setCron(cron);
}

Trace::AlarmEventInfo::AlarmEventInfo(kj::Date scheduledTime)
    : scheduledTime(scheduledTime) {}

Trace::AlarmEventInfo::AlarmEventInfo(rpc::Trace::AlarmEventInfo::Reader reader)
    : scheduledTime(reader.getScheduledTimeMs() * kj::MILLISECONDS + kj::UNIX_EPOCH) {}

void Trace::AlarmEventInfo::copyTo(rpc::Trace::AlarmEventInfo::Builder builder) {
  builder.setScheduledTimeMs((scheduledTime - kj::UNIX_EPOCH) / kj::MILLISECONDS);
}

Trace::QueueEventInfo::QueueEventInfo(kj::String queueName, uint32_t batchSize)
    : queueName(kj::mv(queueName)), batchSize(batchSize) {}

Trace::QueueEventInfo::QueueEventInfo(rpc::Trace::QueueEventInfo::Reader reader)
    : queueName(kj::heapString(reader.getQueueName())), batchSize(reader.getBatchSize()) {}

void Trace::QueueEventInfo::copyTo(rpc::Trace::QueueEventInfo::Builder builder) {
  builder.setQueueName(queueName);
  builder.setBatchSize(batchSize);
}

Trace::EmailEventInfo::EmailEventInfo(kj::String mailFrom, kj::String rcptTo, uint32_t rawSize)
    : mailFrom(kj::mv(mailFrom)), rcptTo(kj::mv(rcptTo)), rawSize(rawSize) {}

Trace::EmailEventInfo::EmailEventInfo(rpc::Trace::EmailEventInfo::Reader reader)
    : mailFrom(kj::heapString(reader.getMailFrom())),
      rcptTo(kj::heapString(reader.getRcptTo())),
      rawSize(reader.getRawSize()) {}

void Trace::EmailEventInfo::copyTo(rpc::Trace::EmailEventInfo::Builder builder) {
  builder.setMailFrom(mailFrom);
  builder.setRcptTo(rcptTo);
  builder.setRawSize(rawSize);
}

Trace::FetchResponseInfo::FetchResponseInfo(uint16_t statusCode)
    : statusCode(statusCode) {}

Trace::FetchResponseInfo::FetchResponseInfo(rpc::Trace::FetchResponseInfo::Reader reader)
    : statusCode(reader.getStatusCode()) {}

void Trace::FetchResponseInfo::copyTo(rpc::Trace::FetchResponseInfo::Builder builder) {
  builder.setStatusCode(statusCode);
}

Trace::Log::Log(kj::Date timestamp, LogLevel logLevel, kj::String message)
    : timestamp(timestamp),
      logLevel(logLevel),
      message(kj::mv(message)) {}

Trace::Exception::Exception(kj::Date timestamp, kj::String name, kj::String message)
    : timestamp(timestamp), name(kj::mv(name)), message(kj::mv(message)) {}

Trace::Trace(kj::Maybe<kj::String> stableId, kj::Maybe<kj::String> scriptName,
  kj::Maybe<kj::String> dispatchNamespace, kj::Array<kj::String> scriptTags)
    : stableId(kj::mv(stableId)),
    scriptName(kj::mv(scriptName)),
    dispatchNamespace(kj::mv(dispatchNamespace)),
    scriptTags(kj::mv(scriptTags)) {}
Trace::Trace(rpc::Trace::Reader reader) {
  mergeFrom(reader, PipelineLogLevel::FULL);
}

Trace::~Trace() noexcept(false) {}

void Trace::copyTo(rpc::Trace::Builder builder) {
  {
    auto list = builder.initLogs(logs.size());
    for (auto i: kj::indices(logs)) {
      logs[i].copyTo(list[i]);
    }
  }

  {
    auto list = builder.initExceptions(exceptions.size());
    for (auto i: kj::indices(exceptions)) {
      exceptions[i].copyTo(list[i]);
    }
  }

  builder.setOutcome(outcome);
  builder.setCpuTime(cpuTime / kj::MILLISECONDS);
  builder.setWallTime(wallTime / kj::MILLISECONDS);
  KJ_IF_MAYBE(s, scriptName) {
    builder.setScriptName(*s);
  }
  KJ_IF_MAYBE(s, dispatchNamespace) {
    builder.setDispatchNamespace(*s);
  }

  {
    auto list = builder.initScriptTags(scriptTags.size());
    for (auto i: kj::indices(scriptTags)) {
      list.set(i, scriptTags[i]);
    }
  }
  builder.setEventTimestampNs((eventTimestamp - kj::UNIX_EPOCH) / kj::NANOSECONDS);

  auto eventInfoBuilder = builder.initEventInfo();
  KJ_IF_MAYBE(e, eventInfo) {
    KJ_SWITCH_ONEOF(*e) {
      KJ_CASE_ONEOF(fetch, FetchEventInfo) {
        auto fetchBuilder = eventInfoBuilder.initFetch();
        fetch.copyTo(fetchBuilder);
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
      KJ_CASE_ONEOF(custom, CustomEventInfo) {
        eventInfoBuilder.initCustom();
      }
    }
  } else {
    eventInfoBuilder.setNone();
  }

  KJ_IF_MAYBE(fetchResponseInfo, this->fetchResponseInfo) {
    auto fetchResponseInfoBuilder = builder.initResponse();
    fetchResponseInfo->copyTo(fetchResponseInfoBuilder);
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
}

void Trace::mergeFrom(rpc::Trace::Reader reader, PipelineLogLevel pipelineLogLevel) {
  // Sandboxed workers currently record their traces as if the pipeline log level were set to
  // "full", so we may need to filter out the extra data after receiving the traces back.
  if (pipelineLogLevel != PipelineLogLevel::NONE) {
    logs.addAll(reader.getLogs());
    exceptions.addAll(reader.getExceptions());
  }

  outcome = reader.getOutcome();
  cpuTime = reader.getCpuTime() * kj::MILLISECONDS;
  wallTime = reader.getWallTime() * kj::MILLISECONDS;

  // mergeFrom() is called both when deserializing traces from a sandboxed
  // worker and when deserializing traces sent to a sandboxed trace worker.  In
  // the former case, the trace's scriptName is already set and the deserialized
  // value is missing, so we need to be careful not to overwrite the set value.
  if (reader.hasScriptName()) {
    scriptName = kj::str(reader.getScriptName());
  }

  if (reader.hasDispatchNamespace()) {
    dispatchNamespace = kj::str(reader.getDispatchNamespace());
  }

  if (auto tags = reader.getScriptTags(); tags.size() > 0) {
    scriptTags = KJ_MAP(tag, tags) { return kj::str(tag); };
  }

  eventTimestamp = kj::UNIX_EPOCH + reader.getEventTimestampNs() * kj::NANOSECONDS;

  if (pipelineLogLevel == PipelineLogLevel::NONE) {
    eventInfo = nullptr;
  } else {
    auto e = reader.getEventInfo();
    switch (e.which()) {
      case rpc::Trace::EventInfo::Which::FETCH:
        eventInfo = FetchEventInfo(e.getFetch());
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
      case rpc::Trace::EventInfo::Which::CUSTOM:
        eventInfo = CustomEventInfo(e.getCustom());
        break;
      case rpc::Trace::EventInfo::Which::NONE:
        eventInfo = nullptr;
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
      message(kj::str(reader.getMessage())) {}

SpanBuilder& SpanBuilder::operator=(SpanBuilder &&other) {
  end();
  state = kj::mv(other.state);
  return *this;
}

SpanBuilder::~SpanBuilder() noexcept(false) {
  end();
}

void SpanBuilder::end() {
  KJ_IF_MAYBE(s, state) {
    s->span.endTime = kj::systemPreciseCalendarClock().now();
    s->observer->report(s->span);
    state = nullptr;
  }
}

void SpanBuilder::setOperationName(kj::StringPtr operationName) {
  KJ_IF_MAYBE(s, state) {
    s->span.operationName = operationName;
  }
}

void SpanBuilder::setTag(kj::StringPtr key, TagValue value) {
  KJ_IF_MAYBE(s, state) {
    s->span.tags.upsert(key, kj::mv(value), [key](TagValue& existingValue, TagValue&& newValue) {
      // This is a programming error, but not a serious one. We could alternatively just emit
      // duplicate tags and leave the Jaeger UI in charge of warning about them.
      [[maybe_unused]] static auto logged = [key]() {
        KJ_LOG(WARNING, "overwriting previous tag", key);
        return true;
      }();
      existingValue = kj::mv(newValue);
    });
  }
}

void SpanBuilder::addLog(kj::Date timestamp, kj::StringPtr key, TagValue value) {
  KJ_IF_MAYBE(s, state) {
    if (s->span.logs.size() >= Span::MAX_LOGS) {
      ++s->span.droppedLogs;
    } else {
      s->span.logs.add(Span::Log {
        .timestamp = timestamp,
        .tag = {
          .key = key,
          .value = kj::mv(value),
        }
      });
    }
  }
}

PipelineTracer::~PipelineTracer() noexcept(false) {
  KJ_IF_MAYBE(p, parentTracer) {
    for (auto& t: traces) {
      (*p)->traces.add(kj::addRef(*t));
    }
  }
  KJ_IF_MAYBE(f, completeFulfiller) {
    f->get()->fulfill(traces.releaseAsArray());
  }
}

kj::Promise<kj::Array<kj::Own<Trace>>> PipelineTracer::onComplete() {
  KJ_REQUIRE(completeFulfiller == nullptr, "onComplete() can only be called once");

  auto paf = kj::newPromiseAndFulfiller<kj::Array<kj::Own<Trace>>>();
  completeFulfiller = kj::mv(paf.fulfiller);
  return kj::mv(paf.promise);
}

kj::Own<WorkerTracer> PipelineTracer::makeWorkerTracer(
    PipelineLogLevel pipelineLogLevel, kj::Maybe<kj::String> stableId,
    kj::Maybe<kj::String> scriptName,  kj::Maybe<kj::String> dispatchNamespace, kj::Array<kj::String> scriptTags) {
  auto trace = kj::refcounted<Trace>(kj::mv(stableId), kj::mv(scriptName), kj::mv(dispatchNamespace), kj::mv(scriptTags));
  traces.add(kj::addRef(*trace));
  return kj::refcounted<WorkerTracer>(kj::addRef(*this), kj::mv(trace), pipelineLogLevel);
}

WorkerTracer::WorkerTracer(kj::Own<PipelineTracer> parentPipeline,
      kj::Own<Trace> trace, PipelineLogLevel pipelineLogLevel)
    : pipelineLogLevel(pipelineLogLevel), trace(kj::mv(trace)),
      parentPipeline(kj::mv(parentPipeline)) {}
WorkerTracer::WorkerTracer(PipelineLogLevel pipelineLogLevel)
    : pipelineLogLevel(pipelineLogLevel),
      trace(kj::refcounted<Trace>(nullptr, nullptr, nullptr, nullptr)) {}

void WorkerTracer::log(kj::Date timestamp, LogLevel logLevel, kj::String message) {
  if (trace->exceededLogLimit) {
    return;
  }
  if (pipelineLogLevel == PipelineLogLevel::NONE) {
    return;
  }
  size_t newSize = trace->bytesUsed + sizeof(Trace::Log) + message.size();
  if (newSize > MAX_TRACE_BYTES) {
    trace->exceededLogLimit = true;
    // We use a JSON encoded array/string to match other console.log() recordings:
    trace->logs.add(
        timestamp, LogLevel::WARN,
        kj::str("[\"Trace resource limit exceeded; subsequent logs not recorded.\"]"));
    return;
  }
  trace->bytesUsed = newSize;
  trace->logs.add(timestamp, logLevel, kj::mv(message));
}

void WorkerTracer::addException(kj::Date timestamp, kj::String name, kj::String message) {
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
  if (newSize > MAX_TRACE_BYTES) {
    trace->exceededExceptionLimit = true;
    trace->exceptions.add(
        timestamp, kj::str("Error"),
        kj::str("Trace resource limit exceeded; subsequent exceptions not recorded."));
    return;
  }
  trace->bytesUsed = newSize;
  trace->exceptions.add(timestamp, kj::mv(name), kj::mv(message));
}

void WorkerTracer::setEventInfo(kj::Date timestamp, Trace::EventInfo&& info) {
  KJ_ASSERT(trace->eventInfo == nullptr, "tracer can only be used for a single event");

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
        trace->logs.add(
            timestamp, LogLevel::WARN,
            kj::str("[\"Trace resource limit exceeded; could not capture event info.\"]"));
        trace->eventInfo = Trace::FetchEventInfo(fetch.method, {}, {}, {});
        return;
      }
    }
    KJ_CASE_ONEOF(_, Trace::ScheduledEventInfo) {}
    KJ_CASE_ONEOF(_, Trace::AlarmEventInfo) {}
    KJ_CASE_ONEOF(_, Trace::QueueEventInfo) {}
    KJ_CASE_ONEOF(_, Trace::EmailEventInfo) {}
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
  // in setEventInfo() that are related to this check whill probably also affect
  // this function.
  if (pipelineLogLevel == PipelineLogLevel::NONE) {
    return;
  }

  KJ_REQUIRE(KJ_REQUIRE_NONNULL(trace->eventInfo).is<Trace::FetchEventInfo>());
  KJ_ASSERT(trace->fetchResponseInfo == nullptr,
            "setFetchResponseInfo can only be called once");
  trace->fetchResponseInfo = kj::mv(info);
}

void WorkerTracer::extractTrace(rpc::Trace::Builder builder) {
  trace->copyTo(builder);
}

void WorkerTracer::setTrace(rpc::Trace::Reader reader) {
  trace->mergeFrom(reader, pipelineLogLevel);
}

} // namespace workerd
