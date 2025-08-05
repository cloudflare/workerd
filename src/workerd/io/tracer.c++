// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/io/tracer.h>
#include <workerd/util/thread-scopes.h>

#include <capnp/message.h>  // for capnp::clone()

namespace workerd {

namespace {

// Approximately how much external data we allow in a trace before we start ignoring requests.  We
// want this number to be big enough to be useful for tracing, but small enough to make it hard to
// DoS the C++ heap -- keeping in mind we can record a trace per handler run during a request.
static constexpr size_t MAX_TRACE_BYTES = 256 * 1024;
// Limit spans to at most 512, it could be difficult to fit e.g. 1024 spans within MAX_TRACE_BYTES
// unless most of the included spans do not include tags. If use cases arise where this amount is
// insufficient, merge smaller spans together or drop smaller spans.
static constexpr size_t MAX_USER_SPANS = 512;

}  // namespace

namespace tracing {
TailStreamWriter::TailStreamWriter(Reporter reporter, TimeSource timeSource)
    : state(State(kj::mv(reporter), kj::mv(timeSource))) {}

void TailStreamWriter::report(const InvocationSpanContext& context, TailEvent::Event&& event) {
  // Becomes a no-op if a terminal event (close or hibernate) has been reported, of if the stream
  // closed due to not receiving a well-formed event handler. We need to disambiguate these cases as
  // the former indicates an implementation error resulting in trailing events whereas the latter
  // case is caused by a user error and events being reported after the stream being closed are
  // expected – reject events following an outcome event, but otherwise just exit if the state has
  // been closed.
  // This could be an assert, but just log an error in case this is prevalent in some edge case.
  if (outcomeSeen) {
    KJ_LOG(ERROR, "reported tail stream event after stream close ", event, kj::getStackTrace());
  }
  auto& s = KJ_UNWRAP_OR_RETURN(state);

  // The onset set must be first and most only happen once.
  if (event.is<tracing::Onset>()) {
    KJ_ASSERT(!onsetSeen, "Tail stream onset already provided");
    onsetSeen = true;
  } else {
    KJ_ASSERT(onsetSeen, "Tail stream onset was not reported");
    if (event.is<tracing::Outcome>() || event.is<tracing::Hibernate>()) {
      outcomeSeen = true;
    }
  }

  tracing::TailEvent tailEvent(context, s.timeSource(), s.sequence++, kj::mv(event));

  // If the reporter returns false, then we will treat it as a close signal.
  if (!s.reporter(kj::mv(tailEvent))) state = kj::none;
}
}  // namespace tracing

PipelineTracer::~PipelineTracer() noexcept(false) {
  KJ_IF_SOME(f, completeFulfiller) {
    f.get()->fulfill(traces.releaseAsArray());
  }
}

void PipelineTracer::addTracesFromChild(kj::ArrayPtr<kj::Own<Trace>> traces) {
  for (auto& t: traces) {
    this->traces.add(kj::addRef(*t));
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
    kj::Maybe<kj::String> entrypoint,
    kj::Maybe<kj::Own<tracing::TailStreamWriter>> maybeTailStreamWriter) {
  auto trace = kj::refcounted<Trace>(kj::mv(stableId), kj::mv(scriptName), kj::mv(scriptVersion),
      kj::mv(dispatchNamespace), kj::mv(scriptId), kj::mv(scriptTags), kj::mv(entrypoint),
      executionModel);
  traces.add(kj::addRef(*trace));
  return kj::refcounted<WorkerTracer>(
      addRefToThis(), kj::mv(trace), pipelineLogLevel, kj::mv(maybeTailStreamWriter));
}

void PipelineTracer::addTrace(rpc::Trace::Reader reader) {
  traces.add(kj::refcounted<Trace>(reader));
}

void PipelineTracer::addTailStreamWriter(kj::Own<tracing::TailStreamWriter>&& writer) {
  tailStreamWriters.add(kj::mv(writer));
}

WorkerTracer::WorkerTracer(kj::Rc<PipelineTracer> parentPipeline,
    kj::Own<Trace> trace,
    PipelineLogLevel pipelineLogLevel,
    kj::Maybe<kj::Own<tracing::TailStreamWriter>> maybeTailStreamWriter)
    : pipelineLogLevel(pipelineLogLevel),
      trace(kj::mv(trace)),
      userRequestSpan(nullptr),
      parentPipeline(kj::mv(parentPipeline)),
      maybeTailStreamWriter(kj::mv(maybeTailStreamWriter)) {}

WorkerTracer::WorkerTracer(PipelineLogLevel pipelineLogLevel, ExecutionModel executionModel)
    : pipelineLogLevel(pipelineLogLevel),
      trace(kj::refcounted<Trace>(
          kj::none, kj::none, kj::none, kj::none, kj::none, nullptr, kj::none, executionModel)),
      userRequestSpan(nullptr) {}

constexpr kj::LiteralStringConst logSizeExceeded =
    "[\"Log size limit exceeded: More than 256KB of data (across console.log statements, exception, request metadata and headers) was logged during a single request. Subsequent data for this request will not be recorded in logs, appear when tailing this Worker's logs, or in Tail Workers.\"]"_kjc;

void WorkerTracer::addLog(const tracing::InvocationSpanContext& context,
    kj::Date timestamp,
    LogLevel logLevel,
    kj::String message) {
  if (trace->exceededLogLimit) {
    return;
  }
  if (pipelineLogLevel == PipelineLogLevel::NONE) {
    return;
  }
  size_t newSize = trace->bytesUsed + sizeof(tracing::Log) + message.size();
  if (newSize > MAX_TRACE_BYTES) {
    trace->exceededLogLimit = true;
    trace->truncated = true;
    // We use a JSON encoded array/string to match other console.log() recordings:
    trace->logs.add(timestamp, LogLevel::WARN, kj::str(logSizeExceeded));
    return;
  }
  trace->bytesUsed = newSize;
  // TODO(streaming-tail): Here we add the log to the trace object and the tail stream writer, if
  // available. If the given worker stage is only tailed by a streaming tail worker, adding the log
  // to the legacy trace object is not needed; this will be addressed in a future refactor.
  KJ_IF_SOME(writer, maybeTailStreamWriter) {
    writer->report(context, {(tracing::Log(timestamp, logLevel, kj::str(message)))});
  }
  trace->logs.add(timestamp, logLevel, kj::mv(message));
}

void WorkerTracer::addSpan(CompleteSpan&& span) {
  // This is where we'll actually encode the span.
  // Drop any spans beyond MAX_USER_SPANS.
  if (trace->spans.size() >= MAX_USER_SPANS) {
    return;
  }

  if (trace->exceededLogLimit) {
    return;
  }
  if (pipelineLogLevel == PipelineLogLevel::NONE) {
    return;
  }

  // 48B for traceID, spanID, parentSpanID, start & end time.
  const int fixedSpanOverhead = 48;
  size_t newSize = trace->bytesUsed + fixedSpanOverhead + span.operationName.size();
  for (const Span::TagMap::Entry& tag: span.tags) {
    newSize += tag.key.size();
    KJ_SWITCH_ONEOF(tag.value) {
      KJ_CASE_ONEOF(str, kj::String) {
        newSize += str.size();
      }
      KJ_CASE_ONEOF(val, bool) {
        newSize++;
      }
      // int64_t and double
      KJ_CASE_ONEOF_DEFAULT {
        newSize += sizeof(int64_t);
      }
    }
  }

  if (newSize > MAX_TRACE_BYTES) {
    trace->exceededLogLimit = true;
    trace->truncated = true;
    trace->logs.add(span.endTime, LogLevel::WARN, kj::str(logSizeExceeded));
    return;
  }

  // Span events are transmitted together for now.
  KJ_IF_SOME(writer, maybeTailStreamWriter) {
    // TODO(o11y): Provide correct nested spans
    // TODO(o11y): Propagate span context when context entropy is not available for RPC-based worker
    // invocations as indicated by isTrigger
    auto& topLevelContext = KJ_ASSERT_NONNULL(topLevelInvocationSpanContext, span);
    tracing::InvocationSpanContext context = [&]() {
      if (topLevelContext.isTrigger()) {
        return topLevelContext.clone();
      } else {
        return topLevelContext.newChild();
      }
    }();

    // Compose span events
    // TODO(o11y): Actually report the spanOpen event at span creation time
    writer->report(context, tracing::SpanOpen(span.parentSpanId, kj::str(span.operationName)));
    if (span.tags.size()) {
      kj::Array<tracing::Attribute> attr = KJ_MAP(tag, span.tags) {
        return tracing::Attribute(kj::ConstString(kj::str(tag.key)), spanTagClone(tag.value));
      };
      writer->report(context, kj::mv(attr));
    }
    writer->report(context, tracing::SpanClose());
  }

  trace->bytesUsed = newSize;
  trace->spans.add(kj::mv(span));
}

void WorkerTracer::addException(const tracing::InvocationSpanContext& context,
    kj::Date timestamp,
    kj::String name,
    kj::String message,
    kj::Maybe<kj::String> stack) {
  if (trace->exceededExceptionLimit) {
    return;
  }
  // TODO(someday): For now, we're using logLevel == none as a hint to avoid doing anything
  //   expensive while tracing.  We may eventually want separate configuration for exceptions vs.
  //   logs.
  if (pipelineLogLevel == PipelineLogLevel::NONE) {
    return;
  }
  size_t newSize = trace->bytesUsed + sizeof(tracing::Exception) + name.size() + message.size();
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
  KJ_IF_SOME(writer, maybeTailStreamWriter) {
    writer->report(context,
        {tracing::Exception(timestamp, kj::str(name), kj::str(message), mapCopyString(stack))});
  }
  trace->exceptions.add(timestamp, kj::mv(name), kj::mv(message), kj::mv(stack));
}

void WorkerTracer::addDiagnosticChannelEvent(const tracing::InvocationSpanContext& context,
    kj::Date timestamp,
    kj::String channel,
    kj::Array<kj::byte> message) {
  if (trace->exceededDiagnosticChannelEventLimit) {
    return;
  }
  if (pipelineLogLevel == PipelineLogLevel::NONE) {
    return;
  }
  size_t newSize =
      trace->bytesUsed + sizeof(tracing::DiagnosticChannelEvent) + channel.size() + message.size();
  if (newSize > MAX_TRACE_BYTES) {
    trace->exceededDiagnosticChannelEventLimit = true;
    trace->truncated = true;
    trace->diagnosticChannelEvents.add(
        timestamp, kj::str("workerd.LimitExceeded"), kj::Array<kj::byte>());
    return;
  }
  trace->bytesUsed = newSize;

  KJ_IF_SOME(writer, maybeTailStreamWriter) {
    writer->report(context,
        {tracing::DiagnosticChannelEvent(
            timestamp, kj::str(channel), kj::heapArray<kj::byte>(message))});
  }
  trace->diagnosticChannelEvents.add(timestamp, kj::mv(channel), kj::mv(message));
}

void WorkerTracer::setEventInfo(
    const tracing::InvocationSpanContext& context, kj::Date timestamp, tracing::EventInfo&& info) {
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
  this->topLevelInvocationSpanContext = context.clone();

  size_t newSize = trace->bytesUsed;
  KJ_SWITCH_ONEOF(info) {
    KJ_CASE_ONEOF(fetch, tracing::FetchEventInfo) {
      newSize += fetch.url.size();
      for (const auto& header: fetch.headers) {
        newSize += header.name.size() + header.value.size();
      }
      newSize += fetch.cfJson.size();
      if (newSize > MAX_TRACE_BYTES) {
        trace->truncated = true;
        trace->logs.add(timestamp, LogLevel::WARN,
            kj::str("[\"Trace resource limit exceeded; could not capture event info.\"]"));
        trace->eventInfo = tracing::FetchEventInfo(fetch.method, {}, {}, {});
        return;
      }
    }
    KJ_CASE_ONEOF_DEFAULT {}
  }
  trace->bytesUsed = newSize;

  KJ_IF_SOME(writer, maybeTailStreamWriter) {
    // Provide WorkerInfo to the streaming tail worker if available. This data is provided when the
    // WorkerTracer is created, but the actual onset event is the best time to send it.
    auto workerInfo = tracing::Onset::WorkerInfo{
      .executionModel = trace->executionModel,
      .scriptName = mapCopyString(trace->scriptName),
      .scriptVersion =
          trace->scriptVersion.map([](auto& scriptVersion) -> kj::Own<ScriptVersion::Reader> {
      return capnp::clone(*scriptVersion);
    }),
      .dispatchNamespace = mapCopyString(trace->dispatchNamespace),
      .scriptId = mapCopyString(trace->scriptId),
      .scriptTags = KJ_MAP(tag, trace->scriptTags) { return kj::str(tag); },
      .entrypoint = mapCopyString(trace->entrypoint),
    };

    writer->report(context, tracing::Onset(cloneEventInfo(info), kj::mv(workerInfo), kj::none));
  }
  trace->eventInfo = kj::mv(info);
}

void WorkerTracer::setOutcome(EventOutcome outcome, kj::Duration cpuTime, kj::Duration wallTime) {
  trace->outcome = outcome;
  trace->cpuTime = cpuTime;
  trace->wallTime = wallTime;

  // All spans must have wrapped up before the outcome is reported. Report the top-level "worker"
  // span by deallocating its span builder.
  userRequestSpan = nullptr;

  // Do not attempt to report an outcome event if logging is disabled, as with other event types.
  if (pipelineLogLevel == PipelineLogLevel::NONE) {
    return;
  }
  // For worker events where we never set the event info (such as WorkerEntrypoint::test() used in
  // wd_test), we never set up a tail stream and accordingly should not report an outcome
  // event. Worker events that should be traced need to set the event info at the start of the
  // invocation to submit the onset event before any other tail events.
  KJ_IF_SOME(writer, maybeTailStreamWriter) {
    auto& spanContext = KJ_UNWRAP_OR_RETURN(topLevelInvocationSpanContext);
    if (isPredictableModeForTest()) {
      writer->report(
          spanContext, tracing::Outcome(outcome, 0 * kj::MILLISECONDS, 0 * kj::MILLISECONDS));
    } else {
      writer->report(spanContext, tracing::Outcome(outcome, cpuTime, wallTime));
    }
  }
}

void WorkerTracer::setFetchResponseInfo(tracing::FetchResponseInfo&& info) {
  // Match the behavior of setEventInfo(). Any resolution of the TODO comments
  // in setEventInfo() that are related to this check while probably also affect
  // this function.
  if (pipelineLogLevel == PipelineLogLevel::NONE) {
    return;
  }

  KJ_REQUIRE(KJ_REQUIRE_NONNULL(trace->eventInfo).is<tracing::FetchEventInfo>());
  KJ_ASSERT(trace->fetchResponseInfo == kj::none, "setFetchResponseInfo can only be called once");
  trace->fetchResponseInfo = kj::mv(info);

  KJ_IF_SOME(writer, maybeTailStreamWriter) {
    auto& spanContext = KJ_UNWRAP_OR_RETURN(topLevelInvocationSpanContext);
    writer->report(
        spanContext, tracing::Return({KJ_ASSERT_NONNULL(trace->fetchResponseInfo).clone()}));
  }
}

void WorkerTracer::setUserRequestSpan(SpanBuilder&& span) {
  KJ_ASSERT(!userRequestSpan.isObserved(), "setUserRequestSpan can only be called once");
  userRequestSpan = kj::mv(span);
}

SpanParent WorkerTracer::getUserRequestSpan() {
  return userRequestSpan;
}

}  // namespace workerd
