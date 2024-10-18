// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include "trace-common.h"

#include <workerd/jsg/memory.h>

#include <kj/string.h>
#include <kj/time.h>

namespace workerd {

// ======================================================================================
// Represents a trace span. `Span` objects are delivered to `SpanObserver`s for recording. To
// create a `Span`, use a `SpanBuilder`. (Used in the legacy trace api)
// Note that this is not the same thing as a trace::Span. This class is part of the legacy
// API for representing spans using log messages instead.
struct Span final {
  using TagValue = kj::OneOf<bool, int64_t, double, kj::String>;
  // TODO(someday): Support binary bytes, too.
  using TagMap = kj::HashMap<kj::ConstString, TagValue>;
  using Tag = TagMap::Entry;

  struct Log {
    kj::Date timestamp;
    Tag tag;
  };

  kj::ConstString operationName;
  kj::Date startTime;
  kj::Date endTime;
  TagMap tags;
  kj::Vector<Log> logs;

  // We set an arbitrary (-ish) cap on log messages for safety. If we drop logs because of this,
  // we report how many in a final "dropped_logs" log.
  //
  // At the risk of being too clever, I chose a limit that is one below a power of two so that
  // we'll typically have space for one last element available for the "dropped_logs" log without
  // needing to grow the vector.
  static constexpr uint16_t MAX_LOGS = 1023;
  kj::uint droppedLogs = 0;

  explicit Span(kj::ConstString operationName, kj::Date startTime)
      : operationName(kj::mv(operationName)),
        startTime(startTime),
        endTime(startTime) {}
  Span(Span&&) = default;
  Span& operator=(Span&&) = default;
  KJ_DISALLOW_COPY(Span);
};

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
  explicit Trace(trace::Onset&& onset = trace::Onset());
  Trace(rpc::Trace::Reader reader);
  ~Trace() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(Trace);

  // We treat the origin value as "unset".
  kj::Date eventTimestamp = kj::UNIX_EPOCH;

  trace::Onset onsetInfo;
  trace::Outcome outcomeInfo{};
  kj::Duration cpuTime;
  kj::Duration wallTime;

  kj::Vector<trace::Log> logs;
  // TODO(o11y): Convert this to actually store spans.
  kj::Vector<trace::Log> spans;
  // A request's trace can have multiple exceptions due to separate request/waitUntil tasks.
  kj::Vector<trace::Exception> exceptions;

  kj::Vector<trace::DiagnosticChannelEvent> diagnosticChannelEvents;

  kj::Maybe<trace::FetchResponseInfo> fetchResponseInfo;

  void setEventInfo(kj::Date timestamp, trace::EventInfo&& info);
  void setOutcome(trace::Outcome&& outcome);
  void setFetchResponseInfo(trace::FetchResponseInfo&& info);
  void addSpan(const Span&& span, kj::String spanContext);
  void addLog(trace::Log&& log, bool isSpan = false);
  void addException(trace::Exception&& exception);
  void addDiagnosticChannelEvent(trace::DiagnosticChannelEvent&& event);
  void addMetrics(trace::Metrics&& metrics);

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
