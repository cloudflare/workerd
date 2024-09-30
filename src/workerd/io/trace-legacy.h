#pragma once

#include "trace-common.h"

#include <workerd/jsg/memory.h>

#include <kj/compat/http.h>
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

  class DiagnosticChannelEvent {
  public:
    explicit DiagnosticChannelEvent(
        kj::Date timestamp, kj::String channel, kj::Array<kj::byte> message);
    DiagnosticChannelEvent(rpc::Trace::DiagnosticChannelEvent::Reader reader);
    DiagnosticChannelEvent(DiagnosticChannelEvent&&) = default;
    KJ_DISALLOW_COPY(DiagnosticChannelEvent);

    kj::Date timestamp;
    kj::String channel;
    kj::Array<kj::byte> message;

    void copyTo(rpc::Trace::DiagnosticChannelEvent::Builder builder);
  };

  class Log {
  public:
    explicit Log(kj::Date timestamp, LogLevel logLevel, kj::String message);
    Log(rpc::Trace::Log::Reader reader);
    Log(Log&&) = default;
    KJ_DISALLOW_COPY(Log);
    ~Log() noexcept(false) = default;

    // Only as accurate as Worker's Date.now(), for Spectre mitigation.
    kj::Date timestamp;

    LogLevel logLevel;
    // TODO(soon): Just string for now.  Eventually, capture serialized JS objects.
    kj::String message;

    void copyTo(rpc::Trace::Log::Builder builder);
  };

  class Exception {
  public:
    explicit Exception(
        kj::Date timestamp, kj::String name, kj::String message, kj::Maybe<kj::String> stack);
    Exception(rpc::Trace::Exception::Reader reader);
    Exception(Exception&&) = default;
    KJ_DISALLOW_COPY(Exception);
    ~Exception() noexcept(false) = default;

    // Only as accurate as Worker's Date.now(), for Spectre mitigation.
    kj::Date timestamp;

    kj::String name;
    kj::String message;

    kj::Maybe<kj::String> stack;

    void copyTo(rpc::Trace::Exception::Builder builder);
  };

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
