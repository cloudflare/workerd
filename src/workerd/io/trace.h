// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "jaeger.h"
#include <kj/async.h>
#include <kj/one-of.h>
#include <kj/refcount.h>
#include <kj/string.h>
#include <kj/time.h>
#include <kj/vector.h>
#include <workerd/io/outcome.capnp.h>
#include <workerd/io/worker-interface.capnp.h>

namespace kj {
  enum class HttpMethod;
  class EntropySource;
}

namespace workerd {

typedef rpc::Trace::Log::Level LogLevel;

enum class PipelineLogLevel {
  // WARNING: This must be kept in sync with PipelineDef::LogLevel (which is not in the OSS
  //   release).
  NONE, FULL
};

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
class Trace final : public kj::Refcounted {
  // Collects trace information about the handling of a worker/pipline fetch event.
public:
  explicit Trace(kj::Maybe<kj::String> stableId, kj::Maybe<kj::String> scriptName, kj::Maybe<kj::String> dispatchNamespace);
  Trace(rpc::Trace::Reader reader);
  ~Trace() noexcept(false);
  KJ_DISALLOW_COPY(Trace);

  class FetchEventInfo {
  public:
    class Header;

    explicit FetchEventInfo(kj::HttpMethod method, kj::String url, kj::String cfJson,
        kj::Array<Header> headers);
    FetchEventInfo(rpc::Trace::FetchEventInfo::Reader reader);

    class Header {
    public:
      explicit Header(kj::String name, kj::String value);
      Header(rpc::Trace::FetchEventInfo::Header::Reader reader);

      kj::String name;
      kj::String value;

      void copyTo(rpc::Trace::FetchEventInfo::Header::Builder builder);
    };

    kj::HttpMethod method;
    kj::String url;
    kj::String cfJson;
    // TODO(perf): It might be more efficient to store some sort of parsed JSON result instead?
    kj::Array<Header> headers;

    void copyTo(rpc::Trace::FetchEventInfo::Builder builder);
  };

  class ScheduledEventInfo {
  public:
    explicit ScheduledEventInfo(double scheduledTime, kj::String cron);
    ScheduledEventInfo(rpc::Trace::ScheduledEventInfo::Reader reader);

    double scheduledTime;
    kj::String cron;

    void copyTo(rpc::Trace::ScheduledEventInfo::Builder builder);
  };

  class AlarmEventInfo {
  public:
    explicit AlarmEventInfo(kj::Date scheduledTime);
    AlarmEventInfo(rpc::Trace::AlarmEventInfo::Reader reader);

    kj::Date scheduledTime;

    void copyTo(rpc::Trace::AlarmEventInfo::Builder builder);
  };

  class FetchResponseInfo {
  public:
    explicit FetchResponseInfo(uint16_t statusCode);
    FetchResponseInfo(rpc::Trace::FetchResponseInfo::Reader reader);

    uint16_t statusCode;

    void copyTo(rpc::Trace::FetchResponseInfo::Builder builder);
  };

  class Log {
  public:
    explicit Log(kj::Date timestamp, LogLevel logLevel, kj::String message);
    Log(rpc::Trace::Log::Reader reader);
    Log(Log&&) = default;
    KJ_DISALLOW_COPY(Log);
    ~Log() noexcept(false) = default;

    kj::Date timestamp;
    // Only as accurate as Worker's Date.now(), for Spectre mitigation.

    LogLevel logLevel;
    kj::String message;
    // TODO(soon): Just string for now.  Eventually, capture serialized JS objects.

    void copyTo(rpc::Trace::Log::Builder builder);
  };

  class Exception {
  public:
    explicit Exception(kj::Date timestamp, kj::String name, kj::String message);
    Exception(rpc::Trace::Exception::Reader reader);
    Exception(Exception&&) = default;
    KJ_DISALLOW_COPY(Exception);
    ~Exception() noexcept(false) = default;

    kj::Date timestamp;
    // Only as accurate as Worker's Date.now(), for Spectre mitigation.

    kj::String name;
    kj::String message;
    // TODO(someday): record exception source, line/column number, stack trace?

    void copyTo(rpc::Trace::Exception::Builder builder);
  };

  kj::Maybe<kj::String> stableId;
  // Empty for toplevel worker.

  kj::Date eventTimestamp = kj::UNIX_EPOCH;
  // We treat the origin value as "unset".

  typedef kj::OneOf<FetchEventInfo, ScheduledEventInfo, AlarmEventInfo> EventInfo;
  kj::Maybe<EventInfo> eventInfo;
  // TODO(someday): Support more event types.
  // TODO(someday): Work out what sort of information we may want to convey about the parent
  // trace, if any.

  kj::Maybe<kj::String> scriptName;
  kj::Maybe<kj::String> dispatchNamespace;

  kj::Vector<Log> logs;
  kj::Vector<Exception> exceptions;
  // A request's trace can have multiple exceptions due to separate request/waitUntil tasks.

  EventOutcome outcome = EventOutcome::UNKNOWN;

  kj::Maybe<FetchResponseInfo> fetchResponseInfo;

  kj::Duration cpuTime;
  kj::Duration wallTime;

  bool exceededLogLimit = false;
  bool exceededExceptionLimit = false;
  size_t bytesUsed = 0;
  // Trace data is recorded outside of the JS heap.  To avoid DoS, we keep an estimate of trace
  // data size, and we stop recording if too much is used.

  // TODO(someday): Eventually, want to capture: customer-facing spans, metrics, user data

  void copyTo(rpc::Trace::Builder builder);
  // Copy content from this trace into `builder`.

  void mergeFrom(rpc::Trace::Reader reader, PipelineLogLevel pipelineLogLevel);
  // Adds all content from `reader` to this `Trace`. (Typically this trace is empty before the
  // call.)  Also applies filtering to the trace as if it were recorded with the given
  // pipelineLogLevel.
};

class Tracer final : public kj::Refcounted {
  // Records a worker stage's trace information into a Trace object.  When all references to the
  // Tracer are released, its Trace is considered complete and ready for submission.
public:
  class Span final {
  public:
    explicit Span(kj::Own<Tracer> tracer, kj::Maybe<Jaeger::SpanData> spanData);
    ~Span() noexcept(false);
    Span(Span&&) = default;
    Span& operator=(Span&&) = default;

    Tracer& getTracer() { return *tracer; }
    kj::Maybe<Jaeger::SpanContext> getSpanContext();

    void setOperationName(kj::StringPtr name);
    // `name` should generally be available when creating a span. This method is
    // only needed in cases where the real operation name is known after the
    // span is created.

    using TagValue = Jaeger::SpanData::TagValue;
    void setTag(kj::StringPtr key, TagValue value);
    // `key` must point to memory that will remain valid all the way until this span's data is
    // serialized.

    void addLog(kj::Date timestamp, kj::StringPtr key, TagValue value);
    // `key` must point to memory that will remain valid all the way until this span's data is
    // serialized.
    //
    // The differences between this and `setTag()` is that logs are timestamped and may have
    // duplicate keys.

  private:
    kj::Own<Tracer> tracer;
    kj::Maybe<Jaeger::SpanData> spanData;
    kj::TimePoint durationStartTime;

    static constexpr auto MAX_LOGS = 1023;
    uint droppedLogs = 0;
    // We set an arbitrary (-ish) cap on log messages for safety. If we drop logs because of this,
    // we report how many in a final "dropped_logs" log.
    //
    // At the risk of being too clever, I chose a limit that is one below a power of two so that
    // we'll typically have space for one last element available for the "dropped_logs" log without
    // needing to grow the vector.

    friend Tracer;
  };
  class JaegerSpanSubmitter {
  public:
    virtual void submitSpan(Jaeger::SpanData data) = 0;
  };

  explicit Tracer(kj::EntropySource& entropySource,
      kj::Maybe<kj::Own<Tracer>> parent, kj::Maybe<Jaeger::SpanContext> parentSpanContext,
      kj::Maybe<JaegerSpanSubmitter&> jaegerSpanSubmitter, kj::Own<void> ownJaegerSpanSubmitter);
  KJ_DISALLOW_COPY(Tracer);

  kj::Own<Tracer> makeSubtracer(kj::Maybe<Jaeger::SpanContext> overrideParent);

  kj::Maybe<Jaeger::SpanContext> getParentSpanContext() { return parentSpanContext; }
  // Returns the Jaeger span under which this tracer is running.

  Span makeSpan(kj::StringPtr operationName, kj::Date overrideStartTime,
      kj::Maybe<Span&> overrideParent = nullptr);
  Span makeSpan(kj::StringPtr operationName, kj::Date overrideStartTime,
      kj::Maybe<Jaeger::SpanContext&> overrideParent);
  Span makeSpan(kj::StringPtr operationName, kj::Maybe<Span&> overrideParent = nullptr);
  Span makeSpan(kj::StringPtr operationName, kj::Maybe<Jaeger::SpanContext&> overrideParent);
  // Makes a Jaeger tracing span, automatically tracking the beginning and end of the span via
  // lifetime.  Jaeger spans are for internal profiling, so we record more precise timing than we
  // expose to customers.

private:
  Jaeger::SpanId makeSpanId();

  kj::EntropySource& entropySource;
  // Entropy source used for initializing new Jaeger spans.

  kj::Maybe<kj::Own<Tracer>> parent;
  kj::Maybe<Jaeger::SpanContext> parentSpanContext;
  kj::Maybe<JaegerSpanSubmitter&> jaegerSpanSubmitter;
  kj::Own<void> ownJaegerSpanSubmitter;
  uint64_t predictableJaegerSpanId = 1;
};

kj::Maybe<Tracer::Span> mapMakeSpan(
    kj::Maybe<kj::Own<Tracer>>& tracer, kj::StringPtr operationName,
    kj::Maybe<Tracer::Span&> overrideParent = nullptr);
kj::Maybe<Tracer::Span> mapMakeSpan(
    kj::Maybe<Tracer&> tracer, kj::StringPtr operationName,
    kj::Maybe<Tracer::Span&> overrideParent = nullptr);
// Convenience function to conditionally call `tracer.makeSpan()` on a `Maybe<Own<Tracer>>`, similar
// to `mapAddRef()`. Also returns nullptr if the Maybe contains a Tracer with no parent Jaeger span.
//
// TODO(cleanup): Tracing suffers from Maybe-overload. There's probably a better interface design.

kj::Maybe<Tracer::Span> mapMakeSpan(
    kj::Maybe<Tracer::Span&> parent, kj::StringPtr operationName);
// Like above but gets the `tracer` from `parent.getTracer()` (if parent is non-null).

kj::Maybe<Jaeger::SpanContext> mapGetParentSpanContext(kj::Maybe<kj::Own<Tracer>>& tracer);
kj::Maybe<Jaeger::SpanContext> mapGetParentSpanContext(kj::Maybe<Tracer&> tracer);
// If tracer is non-null, return tracer->getParentSpanContext().

// =======================================================================================

class MaybeSpan {
  // Convenience wrapper around a Maybe<Tracer::Span>.

public:
  MaybeSpan() = default;
  explicit MaybeSpan(Tracer::Span span): span(kj::mv(span)) {}
  MaybeSpan(kj::Maybe<Tracer::Span> span): span(kj::mv(span)) {}
  MaybeSpan& operator=(std::nullptr_t) {
    span = nullptr;
    return *this;
  };

  bool operator==(std::nullptr_t) { return span == nullptr; }

  explicit operator bool() { return span != nullptr; }
  // TODO(cleanup): Remove this, per KJ style people should always use `== nullptr`.

  operator kj::Maybe<Tracer::Span&>() & { return span; }
  // TODO(cleanup): Remove this. It's a temporary helper while refactoring.

  kj::Maybe<Jaeger::SpanContext> getSpanContext() {
    KJ_IF_MAYBE(s, span) {
      return s->getSpanContext();
    } else {
      return nullptr;
    }
  }

  void setOperationName(kj::StringPtr name) {
    KJ_IF_MAYBE(s, span) {
      s->setOperationName(name);
    }
  }

  void setTag(kj::StringPtr key, Tracer::Span::TagValue value) {
    KJ_IF_MAYBE(s, span) {
      s->setTag(key, kj::mv(value));
    }
  }

  void addLog(kj::Date timestamp, kj::StringPtr key, Tracer::Span::TagValue value) {
    KJ_IF_MAYBE(s, span) {
      s->addLog(timestamp, key, kj::mv(value));
    }
  }

  MaybeSpan makeChild(kj::StringPtr operationName) {
    return mapMakeSpan(span, operationName);
  }

private:
  kj::Maybe<Tracer::Span> span;

  friend class MaybeTracer;
};

class MaybeTracer {
  // Counterpart to MaybeSpan. MaybeTracer's intent is to support only the Jaeger-tracing aspect of
  // Tracer.

public:
  MaybeTracer() = default;
  MaybeTracer(std::nullptr_t) {}
  MaybeTracer(MaybeTracer&&) = default;
  MaybeTracer& operator=(MaybeTracer&&) = default;

  explicit MaybeTracer(kj::Maybe<kj::Own<Tracer>> tracer) : tracer(kj::mv(tracer)) {}

  explicit MaybeTracer(kj::Maybe<Tracer::Span&> span);
  explicit MaybeTracer(MaybeSpan& span): MaybeTracer(span.span) {}
  // Convenience constructor from Tracer::Span& to make a MaybeTracer whose parent is that span.

  bool operator==(std::nullptr_t) { return tracer == nullptr; }

  kj::Maybe<Jaeger::SpanContext> getSpanContext() {
    KJ_IF_MAYBE(t, tracer) {
      return (**t).getParentSpanContext();
    } else {
      return nullptr;
    }
  }

  MaybeTracer addRef() {
    return MaybeTracer(tracer.map([](kj::Own<Tracer>& t) { return kj::addRef(*t); }));
  }

  MaybeSpan makeSpan(kj::StringPtr operationName,
                     kj::Maybe<Jaeger::SpanContext> overrideParent = nullptr) {
    KJ_IF_MAYBE(t, tracer) {
      return MaybeSpan((*t)->makeSpan(operationName, overrideParent));
    } else {
      return {};
    }
  }

private:
  kj::Maybe<kj::Own<Tracer>> tracer;
};

class WorkerTracer;

class PipelineTracer final : public kj::Refcounted {
  // A tracer which records traces for a set of stages. All traces for a pipeline's stages and
  // possible subpipeline stages are recorded here, where they can be used to call a pipeline's
  // trace worker.

public:
  explicit PipelineTracer(kj::Maybe<kj::Own<PipelineTracer>> parentPipeline = nullptr)
      : parentTracer(kj::mv(parentPipeline)) {}
  // Creates a pipeline tracer (with a possible parent).

  ~PipelineTracer() noexcept(false);
  KJ_DISALLOW_COPY(PipelineTracer);

  kj::Promise<kj::Array<kj::Own<Trace>>> onComplete();
  // Returns a promise that fulfills when traces are complete.  Only one such promise can
  // exist at a time.

  kj::Own<PipelineTracer> makePipelineSubtracer() {
    // Makes a tracer for a subpipeline.
    return kj::refcounted<PipelineTracer>(kj::addRef(*this));
  }

  kj::Own<WorkerTracer> makeWorkerTracer(PipelineLogLevel pipelineLogLevel,
                                         kj::Maybe<kj::String> stableId,
                                         kj::Maybe<kj::String> scriptName,
                                         kj::Maybe<kj::String> dispatchNamespace);
  // Makes a tracer for a worker stage.

private:
  kj::Vector<kj::Own<Trace>> traces;
  kj::Maybe<kj::Own<kj::PromiseFulfiller<kj::Array<kj::Own<Trace>>>>> completeFulfiller;

  kj::Maybe<kj::Own<PipelineTracer>> parentTracer;

  friend class WorkerTracer;
};

class WorkerTracer final : public kj::Refcounted {
  // Records a worker stage's trace information into a Trace object.  When all references to the
  // Tracer are released, its Trace is considered complete and ready for submission. If the Trace to
  // write to isn't provided (that already exists in a PipelineTracer), the trace must by extracted
  // via extractTrace.

public:
  explicit WorkerTracer(kj::Own<PipelineTracer> parentPipeline,
      kj::Own<Trace> trace, PipelineLogLevel pipelineLogLevel);
  explicit WorkerTracer(PipelineLogLevel pipelineLogLevel);
  KJ_DISALLOW_COPY(WorkerTracer);

  void log(kj::Date timestamp, LogLevel logLevel, kj::String message);
  // Adds log line to trace.  For Spectre, timestamp should only be as accurate as JS Date.now().

  // TODO(soon): Eventually:
  //void setMetrics(...) // Or get from MetricsCollector::Request directly?

  void addException(kj::Date timestamp, kj::String name, kj::String message);

  void setEventInfo(kj::Date timestamp, Trace::EventInfo&&);
  // Adds info about the event that triggered the trace.  Must not be called more than once.

  void setFetchResponseInfo(Trace::FetchResponseInfo&&);
  // Adds info about the response. Must not be called more than once, and only
  // after passing a FetchEventInfo to setEventInfo().

  void setOutcome(EventOutcome outcome);

  void setCPUTime(kj::Duration cpuTime);

  void setWallTime(kj::Duration wallTime);

  void extractTrace(rpc::Trace::Builder builder);
  // Used only for a Trace in a process sandbox. Copies the content of this tracer's trace to the
  // builder.

  void setTrace(rpc::Trace::Reader reader);
  // Sets the main trace of this Tracer to match the content of `reader`. This is used in the
  // parent process after receiving a trace from a process sandbox.

private:
  PipelineLogLevel pipelineLogLevel;
  kj::Own<Trace> trace;

  // own an instance of the pipeline to make sure it doesn't get destroyed
  // before we're finished tracing
  kj::Maybe<kj::Own<PipelineTracer>> parentPipeline;
};

// =======================================================================================

inline kj::String truncateScriptId(kj::StringPtr id) {
  // Helper function used when setting "truncated_script_id" tags. Truncates the scriptId to 10
  // characters.
  auto truncatedId = id.slice(0, kj::min(id.size(), 10));
  return kj::str(truncatedId);
}

} // namespace workerd
