#pragma once

#include "trace-common.h"

#include <workerd/jsg/memory.h>

#include <kj/string.h>
#include <kj/time.h>

namespace workerd {

// This is the original implementation of how trace worker data was collected. All of the
// data is stored in an in-memory structure and delivered as a single unit to the trace
// worker only when the request is fully completed. The data is held in memory and capped
// at a specific limit. Once the limit is reached, new data is silently dropped.

// TODO(someday): See if we can merge similar code concepts...  Trace fills a role similar to
// MetricsCollector::Reporter::StageEvent, and Tracer fills a role similar to
// MetricsCollector::Request.  Currently, the major differences are:
//
//   - MetricsCollector::Request uses its destructor to measure a IoContext's wall time, so
//     it needs to live exactly as long as its IoContext.  Tracer currently needs to live as
//     long as both the IoContext and those of any subrequests.
//   - Due to the difference in lifetimes, results of each become available in a different order,
//     and intermediate values can be freed at different times.
//   - Request builds a vector of results, while Tracer builds a tree.

// TODO(cleanup) - worth separating into immutable Trace vs. mutable TraceBuilder?

// Collects trace information about the handling of a worker/pipeline fetch event.
class Trace final: public kj::Refcounted {
public:
  explicit Trace(trace::OnsetInfo&& onset = {});
  Trace(rpc::Trace::Reader reader);
  ~Trace() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(Trace);

// TODO(cleanup): Use the trace namespace directly and remove these aliases.
// These can be removed once all references are removed from the internal repo.
#define DEPRECATED_ALIAS(Name) using Name [[deprecated("Use trace::" #Name)]] = trace::Name;
  DEPRECATED_ALIAS(FetchEventInfo)
  DEPRECATED_ALIAS(FetchResponseInfo)
  DEPRECATED_ALIAS(JsRpcEventInfo)
  DEPRECATED_ALIAS(ScheduledEventInfo)
  DEPRECATED_ALIAS(AlarmEventInfo)
  DEPRECATED_ALIAS(QueueEventInfo)
  DEPRECATED_ALIAS(EmailEventInfo)
  DEPRECATED_ALIAS(HibernatableWebSocketEventInfo)
  DEPRECATED_ALIAS(CustomEventInfo)
  DEPRECATED_ALIAS(TraceEventInfo)
  DEPRECATED_ALIAS(EventInfo)
  DEPRECATED_ALIAS(DiagnosticChannelEvent)
  DEPRECATED_ALIAS(Log)
  DEPRECATED_ALIAS(Exception)
#undef DEPRECATED_ALIAS

  // Empty for toplevel worker.
  //  kj::Maybe<kj::String> stableId;

  // We treat the origin value as "unset".
  kj::Date eventTimestamp = kj::UNIX_EPOCH;

  kj::Maybe<trace::EventInfo> eventInfo;
  // TODO(someday): Support more event types.
  // TODO(someday): Work out what sort of information we may want to convey about the parent
  // trace, if any.

  trace::OnsetInfo onsetInfo{};
  trace::OutcomeInfo outcomeInfo{};

  kj::Vector<trace::Log> logs;
  // TODO(o11y): Convert this to actually store spans.
  kj::Vector<trace::Log> spans;
  // A request's trace can have multiple exceptions due to separate request/waitUntil tasks.
  kj::Vector<trace::Exception> exceptions;

  kj::Vector<trace::DiagnosticChannelEvent> diagnosticChannelEvents;

  kj::Maybe<trace::FetchResponseInfo> fetchResponseInfo;

  void setEventInfo(kj::Date timestamp, trace::EventInfo&& info);
  void setOutcomeInfo(trace::OutcomeInfo&& outcome);
  void addLog(trace::Log&& log, bool isSpan = false);
  void addException(trace::Exception&& exception);
  void addDiagnosticChannelEvent(trace::DiagnosticChannelEvent&& event);
  void addSpan(const trace::Span&& span, kj::String spanContext);
  void setFetchResponseInfo(trace::FetchResponseInfo&& info);

  bool truncated = false;
  bool exceededLogLimit = false;
  bool exceededExceptionLimit = false;
  bool exceededDiagnosticChannelEventLimit = false;
  // Trace data is recorded outside of the JS heap.  To avoid DoS, we keep an estimate of trace
  // data size, and we stop recording if too much is used.
  size_t bytesUsed = 0;
  size_t numSpans = 0;

  // Copy content from this trace into `builder`.
  void copyTo(rpc::Trace::Builder builder);

  // Adds all content from `reader` to this `Trace`. (Typically this trace is empty before the
  // call.)  Also applies filtering to the trace as if it were recorded with the given
  // pipelineLogLevel.
  void mergeFrom(rpc::Trace::Reader reader, PipelineLogLevel pipelineLogLevel);
};

}  // namespace workerd
