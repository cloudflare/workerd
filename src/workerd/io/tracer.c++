// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/io/tracer.h>
#include <workerd/util/sentry.h>
#include <workerd/util/thread-scopes.h>

#include <capnp/message.h>  // for capnp::clone()

namespace workerd {

namespace {

// Approximately how much external data we allow in a trace before we start ignoring requests.  We
// want this number to be big enough to be useful for tracing, but small enough to make it hard to
// DoS the C++ heap -- keeping in mind we can record a trace per handler run during a request. For
// streaming tail worker, this is the maximum size per tail event.
// TODO(streaming-tail): Add a clear indicator for events being truncated based on MAX_TRACE_BYTES
// so that developers can understand why this happens.
static constexpr size_t MAX_TRACE_BYTES = 256 * 1024;
}  // namespace

namespace tracing {
TailStreamWriter::TailStreamWriter(Reporter reporter): state(State(kj::mv(reporter))) {}

void TailStreamWriter::report(
    const InvocationSpanContext& context, TailEvent::Event&& event, kj::Date timestamp) {
  // Becomes a no-op if a terminal event (close) has been reported, or if the stream closed due to
  // not receiving a well-formed event handler. We need to disambiguate these cases as the former
  // indicates an implementation error resulting in trailing events whereas the latter case is
  // caused by a user error and events being reported after the stream being closed are expected –
  // reject events following an outcome event, but otherwise just exit if the state has been closed.
  // This could be an assert, but just log an error in case this is prevalent in some edge case.
  if (outcomeSeen) {
    KJ_LOG(ERROR, "reported tail stream event after stream close ", event, kj::getStackTrace());
  }
  auto& s = KJ_UNWRAP_OR_RETURN(state);

  // The onset event must be first and must only happen once.
  if (event.is<tracing::Onset>()) {
    KJ_ASSERT(!onsetSeen, "Tail stream onset already provided");
    onsetSeen = true;
  } else {
    KJ_ASSERT(onsetSeen, "Tail stream onset was not reported");
    if (event.is<tracing::Outcome>()) {
      outcomeSeen = true;
    }
  }

  // A zero spanId at the TailEvent level signifies that no spanId should be provided to the tail
  // worker (for Onset events). We go to great lengths to rule out getting an all-zero spanId by
  // chance (see SpanId::fromEntropy()), so this should be safe.
  tracing::TailEvent tailEvent(context.getTraceId(), context.getInvocationId(),
      context.getSpanId() == tracing::SpanId::nullId ? kj::none : kj::Maybe(context.getSpanId()),
      timestamp, s.sequence++, kj::mv(event));

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
    kj::Maybe<kj::String> durableObjectId,
    kj::Maybe<kj::Own<tracing::TailStreamWriter>> maybeTailStreamWriter) {
  auto trace = kj::refcounted<Trace>(kj::mv(stableId), kj::mv(scriptName), kj::mv(scriptVersion),
      kj::mv(dispatchNamespace), kj::mv(scriptId), kj::mv(scriptTags), kj::mv(entrypoint),
      executionModel, kj::mv(durableObjectId));
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
      parentPipeline(kj::mv(parentPipeline)),
      maybeTailStreamWriter(kj::mv(maybeTailStreamWriter)) {}

WorkerTracer::WorkerTracer(PipelineLogLevel pipelineLogLevel, ExecutionModel executionModel)
    : pipelineLogLevel(pipelineLogLevel),
      trace(kj::refcounted<Trace>(
          kj::none, kj::none, kj::none, kj::none, kj::none, nullptr, kj::none, executionModel)) {}

WorkerTracer::~WorkerTracer() noexcept(false) {
  // Report the outcome event, which should have been delivered by now. Note that this can happen
  // when there are no tail events delivered to the tracer at all (such as when a worker interface
  // gets set up without being used for an event), so this may not indicate an error.
  if (trace->outcome == EventOutcome::UNKNOWN) {
    return;
  }

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

    KJ_IF_SOME(fetchResponseInfo, trace->fetchResponseInfo) {
      writer->report(spanContext, tracing::Return({fetchResponseInfo.clone()}), completeTime);
    }

    if (isPredictableModeForTest()) {
      writer->report(spanContext,
          tracing::Outcome(trace->outcome, 0 * kj::MILLISECONDS, 0 * kj::MILLISECONDS),
          completeTime);
    } else {
      writer->report(spanContext, tracing::Outcome(trace->outcome, trace->cpuTime, trace->wallTime),
          completeTime);
    }
  }
};

constexpr kj::LiteralStringConst logSizeExceeded =
    "[\"Log size limit exceeded: More than 256KB of data (across console.log statements, exception, request metadata and headers) was logged during a single request. Subsequent data for this request will not be recorded in logs, appear when tailing this Worker's logs, or in Tail Workers.\"]"_kjc;

void WorkerTracer::addLog(const tracing::InvocationSpanContext& context,
    kj::Date timestamp,
    LogLevel logLevel,
    kj::String message) {
  if (pipelineLogLevel == PipelineLogLevel::NONE) {
    return;
  }

  // TODO(streaming-tail): Here we add the log to the trace object and the tail stream writer, if
  // available. If the given worker stage is only tailed by a streaming tail worker, adding the log
  // to the legacy trace object is not needed; this will be addressed in a future refactor.
  KJ_IF_SOME(writer, maybeTailStreamWriter) {
    // TODO(felix): Used for debug logging, remove after a few days.
    if (topLevelInvocationSpanContext == kj::none) {
      LOG_NOSENTRY(WARNING, "tried to send log before onset event", trace->entrypoint, isJsRpc);
    }
    // If message is too big on its own, truncate it.
    writer->report(context,
        {(tracing::Log(timestamp, logLevel,
            kj::str(message.first(kj::min(message.size(), MAX_TRACE_BYTES)))))},
        timestamp);
  }

  if (trace->exceededLogLimit) {
    return;
  }

  size_t messageSize = sizeof(tracing::Log) + message.size();
  if (trace->bytesUsed + messageSize > MAX_TRACE_BYTES) {
    // We use a JSON encoded array/string to match other console.log() recordings:
    trace->logs.add(timestamp, LogLevel::WARN, kj::str(logSizeExceeded));
    trace->exceededLogLimit = true;
    trace->truncated = true;
  } else {
    trace->bytesUsed += messageSize;
    trace->logs.add(timestamp, logLevel, kj::mv(message));
  }
}

void WorkerTracer::addSpan(CompleteSpan&& span) {
  // This is where we'll actually encode the span.
  if (pipelineLogLevel == PipelineLogLevel::NONE) {
    return;
  }

  adjustSpanTime(span);

  // TODO(cleanup): Set fixed timestamps for predictable mode. Drop this once we have removed the
  // code path for spans in LTW.
  if (isPredictableModeForTest()) {
    span.startTime = kj::UNIX_EPOCH;
    span.endTime = kj::UNIX_EPOCH;
  }

  // 48B for traceID, spanID, parentSpanID, start & end time.
  const int fixedSpanOverhead = 48;
  size_t messageSize = fixedSpanOverhead + span.operationName.size();
  for (const Span::TagMap::Entry& tag: span.tags) {
    messageSize += tag.key.size();
    KJ_SWITCH_ONEOF(tag.value) {
      KJ_CASE_ONEOF(str, kj::String) {
        messageSize += str.size();
      }
      KJ_CASE_ONEOF(val, bool) {
        messageSize++;
      }
      // int64_t and double
      KJ_CASE_ONEOF_DEFAULT {
        messageSize += sizeof(int64_t);
      }
    }
  }

  // Span events are transmitted together for now.
  KJ_IF_SOME(writer, maybeTailStreamWriter) {
    auto& topLevelContext =
        KJ_ASSERT_NONNULL(topLevelInvocationSpanContext, span, trace->entrypoint, isJsRpc);
    // Compose span events. For SpanOpen, an all-zero spanId is interpreted as having no spans above
    // this one, thus we use the Onset spanId instead (taken from topLevelContext). We go to great
    // lengths to rule out getting an all-zero spanId by chance (see SpanId::fromEntropy()), so this
    // should be safe.
    tracing::SpanId parentSpanId = span.parentSpanId;
    if (parentSpanId == tracing::SpanId::nullId) {
      parentSpanId = topLevelContext.getSpanId();
    }
    // TODO(o11y): Actually report the spanOpen event at span creation time
    auto spanOpenContext = tracing::InvocationSpanContext(
        topLevelContext.getTraceId(), topLevelContext.getInvocationId(), parentSpanId);
    auto spanComponentContext = tracing::InvocationSpanContext(
        topLevelContext.getTraceId(), topLevelContext.getInvocationId(), span.spanId);

    writer->report(spanOpenContext, tracing::SpanOpen(span.spanId, kj::str(span.operationName)),
        span.startTime);
    // If a span manages to exceed the size limit, truncate it by not providing span attributes.
    if (span.tags.size() && messageSize <= MAX_TRACE_BYTES) {
      tracing::CustomInfo attr = KJ_MAP(tag, span.tags) {
        return tracing::Attribute(kj::ConstString(kj::str(tag.key)), spanTagClone(tag.value));
      };
      writer->report(spanComponentContext, kj::mv(attr), span.startTime);
    }
    writer->report(spanComponentContext, tracing::SpanClose(), span.endTime);
  }

  // Note: spans will not be shipped to the production version of the legacy tail worker, so we
  // don't need an exceededSpanLimit variable for it.
  if (trace->bytesUsed + messageSize > MAX_TRACE_BYTES) {
    trace->truncated = true;
  } else {
    trace->bytesUsed += messageSize;
    trace->spans.add(kj::mv(span));
  }
}

void WorkerTracer::addException(const tracing::InvocationSpanContext& context,
    kj::Date timestamp,
    kj::String name,
    kj::String message,
    kj::Maybe<kj::String> stack) {
  // TODO(someday): For now, we're using logLevel == none as a hint to avoid doing anything
  //   expensive while tracing.  We may eventually want separate configuration for exceptions vs.
  //   logs.
  if (pipelineLogLevel == PipelineLogLevel::NONE) {
    return;
  }

  size_t messageSize = sizeof(tracing::Exception) + name.size() + message.size();
  KJ_IF_SOME(s, stack) {
    messageSize += s.size();
  }
  KJ_IF_SOME(writer, maybeTailStreamWriter) {
    auto maybeTruncatedName = name.first(kj::min(name.size(), MAX_TRACE_BYTES));
    auto maybeTruncatedMessage =
        message.first(kj::min(message.size(), MAX_TRACE_BYTES - maybeTruncatedName.size()));
    kj::Maybe<kj::String> maybeTruncatedStack;
    KJ_IF_SOME(s, stack) {
      maybeTruncatedStack = kj::heapString(s.first(kj::min(
          s.size(), MAX_TRACE_BYTES - maybeTruncatedName.size() - maybeTruncatedMessage.size())));
    }
    writer->report(context,
        {tracing::Exception(timestamp, kj::str(maybeTruncatedName), kj::str(maybeTruncatedMessage),
            kj::mv(maybeTruncatedStack))},
        timestamp);
  }

  if (trace->exceededExceptionLimit) {
    return;
  }

  if (trace->bytesUsed + messageSize > MAX_TRACE_BYTES) {
    trace->exceededExceptionLimit = true;
    trace->truncated = true;
    trace->exceptions.add(timestamp, kj::str("Error"),
        kj::str("Trace resource limit exceeded; subsequent exceptions not recorded."), kj::none);
  } else {
    trace->bytesUsed += messageSize;
    trace->exceptions.add(timestamp, kj::mv(name), kj::mv(message), kj::mv(stack));
  }
}

void WorkerTracer::addDiagnosticChannelEvent(const tracing::InvocationSpanContext& context,
    kj::Date timestamp,
    kj::String channel,
    kj::Array<kj::byte> message) {
  if (pipelineLogLevel == PipelineLogLevel::NONE) {
    return;
  }

  size_t messageSize = sizeof(tracing::DiagnosticChannelEvent) + channel.size() + message.size();
  KJ_IF_SOME(writer, maybeTailStreamWriter) {
    // Drop oversized diagnostic channel events instead of truncating them – a truncated message may
    // not be deserialized correctly.
    if (messageSize <= MAX_TRACE_BYTES) {
      writer->report(context,
          {tracing::DiagnosticChannelEvent(
              timestamp, kj::str(channel), kj::heapArray<kj::byte>(message))},
          timestamp);
    }
  }

  if (trace->exceededDiagnosticChannelEventLimit) {
    return;
  }

  if (trace->bytesUsed + messageSize > MAX_TRACE_BYTES) {
    trace->exceededDiagnosticChannelEventLimit = true;
    trace->truncated = true;
    trace->diagnosticChannelEvents.add(
        timestamp, kj::str("workerd.LimitExceeded"), kj::Array<kj::byte>());
  } else {
    trace->bytesUsed += messageSize;
    trace->diagnosticChannelEvents.add(timestamp, kj::mv(channel), kj::mv(message));
  }
}

void WorkerTracer::setEventInfo(
    IoContext::IncomingRequest& incomingRequest, tracing::EventInfo&& info) {
  // IoContext is available at this time, capture weakRef.
  KJ_ASSERT(weakIoContext == kj::none, "tracer can only be used for a single event");
  weakIoContext = incomingRequest.getContext().getWeakRef();
  setEventInfoInternal(
      incomingRequest.getInvocationSpanContext(), incomingRequest.now(), kj::mv(info));
}

void WorkerTracer::setEventInfoInternal(
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

  size_t eventSize = 0;
  KJ_SWITCH_ONEOF(info) {
    KJ_CASE_ONEOF(fetch, tracing::FetchEventInfo) {
      eventSize += fetch.url.size();
      for (const auto& header: fetch.headers) {
        eventSize += header.name.size() + header.value.size();
      }
      eventSize += fetch.cfJson.size();
      // Limit STW onset to MAX_TRACE_BYTES, beyond that dispatch a truncated event too.
      if (eventSize > MAX_TRACE_BYTES) {
        info = tracing::FetchEventInfo(fetch.method, {}, {}, {});
      }
    }
    KJ_CASE_ONEOF_DEFAULT {}
  }

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

    // Onset needs special handling for spanId: The top-level spanId is zero unless a trigger
    // context is available (not yet implemented). The inner spanId is taken from the invocation
    // span context, that span is being "opened" with the onset event. All other tail events have it
    // as its parent span ID, except for recursive SpanOpens (which have the parent span instead)
    // and Attribute/SpanClose events (which have the spanId opened in the corresponding SpanOpen).
    auto onsetContext = tracing::InvocationSpanContext(
        context.getTraceId(), context.getInvocationId(), tracing::SpanId::nullId);

    writer->report(onsetContext,
        tracing::Onset(context.getSpanId(), cloneEventInfo(info), kj::mv(workerInfo),
            attributes.releaseAsArray()),
        timestamp);
  }

  // truncation should only be needed for fetch events, since we only set eventSize there.
  if (trace->bytesUsed + eventSize > MAX_TRACE_BYTES && eventSize > 0) {
    trace->truncated = true;
    trace->logs.add(timestamp, LogLevel::WARN,
        kj::str("[\"Trace resource limit exceeded; could not capture event info.\"]"));
    trace->eventInfo =
        tracing::FetchEventInfo(info.get<tracing::FetchEventInfo>().method, {}, {}, {});
  } else {
    trace->bytesUsed += eventSize;
    trace->eventInfo = kj::mv(info);
  }
}

void WorkerTracer::setOutcome(EventOutcome outcome, kj::Duration cpuTime, kj::Duration wallTime) {
  trace->outcome = outcome;
  trace->cpuTime = cpuTime;
  trace->wallTime = wallTime;

  // Free the userRequestSpan – no more traces should come after this point (and thus the observer
  // and the reference to WorkerTracer it holds, unless there are more open spans)
  userRequestSpan = nullptr;

  // Defer reporting the actual outcome event to the WorkerTracer destructor: The outcome is
  // reported when the metrics request is deallocated, but with ctx.waitUntil() there might be spans
  // continuing to exist beyond that point. By the time the WorkerTracer is deallocated, the
  // IoContext and its task set will be done and any additional spans will have wrapped up.
  // This is somewhat at odds with the concept of "streaming" events, but benign as the WorkerTracer
  // wraps up right after the metrics request object in the average case and since the outcome has a
  // fixed size.
}

void WorkerTracer::recordTimestamp(kj::Date timestamp) {
  if (completeTime == kj::UNIX_EPOCH) {
    completeTime = timestamp;
  }
}

void BaseTracer::adjustSpanTime(CompleteSpan& span) {
  // To report I/O time, we need the IOContext to still be alive.
  // weakIoContext is only none if we are tracing via RPC (in this case span times have already been
  // adjusted) or if we failed to transmit an Onset event (in that case we'll get an error based on
  // missing topLevelInvocationSpanContext right after).
  if (weakIoContext != kj::none) {
    auto& weakIoCtx = KJ_ASSERT_NONNULL(weakIoContext);
    weakIoCtx->runIfAlive([&span](IoContext& context) { span.endTime = context.now(); });
    if (!weakIoCtx->isValid()) {
      // This can happen if we start a customEvent from this event and cancel it after this IoContext
      // gets destroyed. In that case we no longer have an IoContext available and can't get the
      // current time, but the outcome timestamp will have already been set. Since the outcome
      // timestamp is "late enough", simply use that.
      // TODO(o11y): fix this – spans should not be outliving the IoContext.
      if (completeTime != kj::UNIX_EPOCH) {
        span.endTime = completeTime;
      } else {
        // Otherwise, we can't actually get an end timestamp that makes sense. Report a zero-duration
        // span and log a warning (or fail assert in test mode).
        span.endTime = span.startTime;
        if (isPredictableModeForTest()) {
          KJ_FAIL_ASSERT("reported span after IoContext was deallocated", span.operationName);
        } else {
          LOG_WARNING_PERIODICALLY(
              "reported span after IoContext was deallocated", span.operationName);
        }
      }
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

  // Note: In the streaming model, fetchResponseInfo is dispatched when the tail worker returns.
  KJ_REQUIRE(KJ_REQUIRE_NONNULL(trace->eventInfo).is<tracing::FetchEventInfo>());
  KJ_ASSERT(trace->fetchResponseInfo == kj::none, "setFetchResponseInfo can only be called once");
  trace->fetchResponseInfo = kj::mv(info);
}

void BaseTracer::setUserRequestSpan(SpanParent&& span) {
  KJ_ASSERT(span.isObserved(), "span argument must be observed");
  KJ_ASSERT(!userRequestSpan.isObserved(), "setUserRequestSpan can only be called once");
  userRequestSpan = kj::mv(span);
}

void WorkerTracer::setWorkerAttribute(kj::ConstString key, Span::TagValue value) {
  attributes.add(tracing::Attribute{kj::mv(key), kj::mv(value)});
}

SpanParent BaseTracer::getUserRequestSpan() {
  return userRequestSpan.addRef();
}

void BaseTracer::setIsJsRpc() {
  isJsRpc = true;
}

void WorkerTracer::setJsRpcInfo(const tracing::InvocationSpanContext& context,
    kj::Date timestamp,
    const kj::ConstString& methodName) {
  // Update the method name in the already-set JsRpcEventInfo for LTW compatibility
  KJ_IF_SOME(info, trace->eventInfo) {
    KJ_SWITCH_ONEOF(info) {
      KJ_CASE_ONEOF(jsRpcInfo, tracing::JsRpcEventInfo) {
        jsRpcInfo.methodName = kj::str(methodName);
      }
      KJ_CASE_ONEOF_DEFAULT {}
    }
  }

  KJ_IF_SOME(writer, maybeTailStreamWriter) {
    auto attr = kj::heapArrayBuilder<tracing::Attribute>(1);
    attr.add(tracing::Attribute("jsrpc.method"_kjc, kj::str(methodName)));
    writer->report(context, attr.finish(), timestamp);
  }
}

}  // namespace workerd
