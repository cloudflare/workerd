#pragma once

#include "trace-common.h"

#include <workerd/jsg/memory.h>

#include <kj/string.h>
#include <kj/time.h>

namespace workerd {

// Provides the implementation of the original Trace Worker Trace API. This version holds
// the trace data in a limited, in-memory buffer that is sent to the Trace Worker only after
// the request is fully completed.

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

  // TODO(cleanup): Use the trace namespace directly and remove these aliases
  using FetchEventInfo = trace::FetchEventInfo;
  using FetchResponseInfo = trace::FetchResponseInfo;
  using JsRpcEventInfo = trace::JsRpcEventInfo;
  using ScheduledEventInfo = trace::ScheduledEventInfo;
  using AlarmEventInfo = trace::AlarmEventInfo;
  using QueueEventInfo = trace::QueueEventInfo;
  using EmailEventInfo = trace::EmailEventInfo;
  using HibernatableWebSocketEventInfo = trace::HibernatableWebSocketEventInfo;
  using CustomEventInfo = trace::CustomEventInfo;
  using TraceEventInfo = trace::TraceEventInfo;
  using EventInfo = trace::EventInfo;

  using DiagnosticChannelEvent = trace::DiagnosticChannelEvent;
  using Log = trace::Log;
  using Exception = trace::Exception;

  // Empty for toplevel worker.
  kj::Maybe<kj::String> stableId;

  // We treat the origin value as "unset".
  kj::Date eventTimestamp = kj::UNIX_EPOCH;

  kj::Maybe<trace::EventInfo> eventInfo;
  // TODO(someday): Support more event types.
  // TODO(someday): Work out what sort of information we may want to convey about the parent
  // trace, if any.

  trace::OnsetInfo onsetInfo{};

  kj::Vector<Log> logs;
  // TODO(o11y): Convert this to actually store spans.
  kj::Vector<Log> spans;
  // A request's trace can have multiple exceptions due to separate request/waitUntil tasks.
  kj::Vector<Exception> exceptions;

  kj::Vector<DiagnosticChannelEvent> diagnosticChannelEvents;

  EventOutcome outcome = EventOutcome::UNKNOWN;

  kj::Maybe<FetchResponseInfo> fetchResponseInfo;

  kj::Duration cpuTime;
  kj::Duration wallTime;

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
