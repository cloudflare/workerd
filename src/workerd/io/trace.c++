// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/io/trace.h>
#include <workerd/util/thread-scopes.h>

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
static constexpr size_t MAX_USER_SPANS = 512;

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
    ExecutionModel executionModel,
    kj::Maybe<kj::String> scriptId,
    kj::Maybe<kj::String> stableId,
    kj::Maybe<kj::String> scriptName,
    kj::Maybe<kj::Own<ScriptVersion::Reader>> scriptVersion,
    kj::Maybe<kj::String> dispatchNamespace,
    kj::Array<kj::String> scriptTags,
    kj::Maybe<kj::String> entrypoint) {
  auto trace = kj::refcounted<Trace>(trace::OnsetInfo{.stableId = kj::mv(stableId),
    .scriptName = kj::mv(scriptName),
    .scriptVersion = kj::mv(scriptVersion),
    .dispatchNamespace = kj::mv(dispatchNamespace),
    .scriptId = kj::mv(scriptId),
    .scriptTags = kj::mv(scriptTags),
    .entrypoint = kj::mv(entrypoint),
    .executionModel = executionModel});
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
WorkerTracer::WorkerTracer(PipelineLogLevel pipelineLogLevel, ExecutionModel executionModel)
    : pipelineLogLevel(pipelineLogLevel),
      trace(kj::refcounted<Trace>(trace::OnsetInfo {
        .executionModel = executionModel,
      })),
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
  // Drop any spans beyond MAX_USER_SPANS.
  if (trace->numSpans >= MAX_USER_SPANS) {
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

void WorkerTracer::setOutcomeInfo(trace::OutcomeInfo&& info) {
  trace->setOutcomeInfo(kj::mv(info));
}

void WorkerTracer::setOutcome(EventOutcome outcome) {
  trace->outcomeInfo.outcome = outcome;
}

void WorkerTracer::setCPUTime(kj::Duration cpuTime) {
  trace->outcomeInfo.cpuTime = cpuTime;
}

void WorkerTracer::setWallTime(kj::Duration wallTime) {
  trace->outcomeInfo.wallTime = wallTime;
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

ScopedDurationTagger::ScopedDurationTagger(
    SpanBuilder& span, kj::ConstString key, const kj::MonotonicClock& timer)
    : span(span),
      key(kj::mv(key)),
      timer(timer),
      startTime(timer.now()) {}

ScopedDurationTagger::~ScopedDurationTagger() noexcept(false) {
  auto duration = timer.now() - startTime;
  if (isPredictableModeForTest()) {
    duration = 0 * kj::NANOSECONDS;
  }
  span.setTag(kj::mv(key), duration / kj::NANOSECONDS);
}

}  // namespace workerd
