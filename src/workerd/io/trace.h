// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/async.h>
#include <kj/one-of.h>
#include <kj/refcount.h>
#include <kj/string.h>
#include <kj/time.h>
#include <kj/vector.h>
#include <kj/map.h>
#include <workerd/io/outcome.capnp.h>
#include <workerd/io/worker-interface.capnp.h>
#include <workerd/util/own-util.h>

namespace kj {
  enum class HttpMethod;
  class EntropySource;
}

namespace workerd {

using kj::byte;
using kj::uint;

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
  explicit Trace(kj::Maybe<kj::String> stableId, kj::Maybe<kj::String> scriptName, kj::Maybe<kj::String> dispatchNamespace, kj::Array<kj::String> scriptTags);
  Trace(rpc::Trace::Reader reader);
  ~Trace() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(Trace);

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

  class QueueEventInfo {
  public:
    explicit QueueEventInfo(kj::String queueName, uint32_t batchSize);
    QueueEventInfo(rpc::Trace::QueueEventInfo::Reader reader);

    kj::String queueName;
    uint32_t batchSize;

    void copyTo(rpc::Trace::QueueEventInfo::Builder builder);
  };

  class EmailEventInfo {
  public:
    explicit EmailEventInfo(kj::String mailFrom, kj::String rcptTo, uint32_t rawSize);
    EmailEventInfo(rpc::Trace::EmailEventInfo::Reader reader);

    kj::String mailFrom;
    kj::String rcptTo;
    uint32_t rawSize;

    void copyTo(rpc::Trace::EmailEventInfo::Builder builder);
  };

  class CustomEventInfo {
  public:
    explicit CustomEventInfo() {};
    CustomEventInfo(rpc::Trace::CustomEventInfo::Reader reader) {};

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

  typedef kj::OneOf<FetchEventInfo, ScheduledEventInfo, AlarmEventInfo, QueueEventInfo,
          EmailEventInfo, CustomEventInfo> EventInfo;
  kj::Maybe<EventInfo> eventInfo;
  // TODO(someday): Support more event types.
  // TODO(someday): Work out what sort of information we may want to convey about the parent
  // trace, if any.

  kj::Maybe<kj::String> scriptName;
  kj::Maybe<kj::String> dispatchNamespace;
  kj::Array<kj::String> scriptTags;

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

// =======================================================================================

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
  KJ_DISALLOW_COPY_AND_MOVE(PipelineTracer);

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
                                         kj::Maybe<kj::String> dispatchNamespace,
                                         kj::Array<kj::String> scriptTags);
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
  KJ_DISALLOW_COPY_AND_MOVE(WorkerTracer);

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

// =======================================================================================
// Span tracing
//
// TODO(cleanup): As of now, this aspect of tracing is actually not related to the rest of this
//   file. Most of this file defines the interface to feed Trace Workers. Span tracing, however,
//   is currently designed to feed tracing of the Workers Runtime itself for the benefit of the
//   developers of the runtime.
//
//   However, we might potentially want to give trace workers some access to span tracing as well.
//   But, that hasn't been designed yet, and it's not clear if that would be based on the same
//   concept of spans or completely separate. In the latter case, these classes should probably
//   move to a diffeent header.

class SpanBuilder;
class SpanObserver;

struct Span {
  // Represents a trace span. `Span` objects are delivered to `SpanObserver`s for recording. To
  // create a `Span`, use a `SpanBuilder`.

public:
  using TagValue = kj::OneOf<bool, int64_t, double, kj::String>;
  // TODO(someday): Support binary bytes, too.
  using TagMap = kj::HashMap<kj::StringPtr, TagValue>;
  using Tag = TagMap::Entry;

  struct Log {
    kj::Date timestamp;
    Tag tag;
  };

  kj::StringPtr operationName;
  kj::Date startTime;
  kj::Date endTime;
  TagMap tags;
  kj::Vector<Log> logs;

  static constexpr auto MAX_LOGS = 1023;
  uint droppedLogs = 0;
  // We set an arbitrary (-ish) cap on log messages for safety. If we drop logs because of this,
  // we report how many in a final "dropped_logs" log.
  //
  // At the risk of being too clever, I chose a limit that is one below a power of two so that
  // we'll typically have space for one last element available for the "dropped_logs" log without
  // needing to grow the vector.

  explicit Span(kj::StringPtr operationName, kj::Date startTime)
      : operationName(operationName), startTime(startTime), endTime(startTime) {}
};

class SpanParent {
  // An opaque token which can be used to create child spans of some parent. This is typically
  // passed down from a caller to a callee when the caller wants to allow the callee to create
  // spans for itself that show up as children of the caller's span, but the caller does not
  // want to give the callee any other ability to modify the parent span.

public:
  SpanParent(SpanBuilder& builder);

  SpanParent(decltype(nullptr)) {}
  // Make a SpanParent that causes children not to be reported anywhere.

  SpanParent(kj::Maybe<kj::Own<SpanObserver>> observer): observer(kj::mv(observer)) {}

  SpanParent(SpanParent&& other) = default;
  SpanParent& operator=(SpanParent&& other) = default;
  KJ_DISALLOW_COPY(SpanParent);

  SpanParent addRef();

  SpanBuilder newChild(kj::StringPtr operationName,
      kj::Date startTime = kj::systemPreciseCalendarClock().now());
  // Create a new child span.
  //
  // `operationName` should be a string literal with infinite lifetime.

  bool isObserved() { return observer != nullptr; }
  // Useful to skip unnecessary code when not observed.

  kj::Maybe<SpanObserver&> getObserver() { return observer; }
  // Get the underlying SpanObserver representing the parent span.
  //
  // This is needed in particular when making outbound network requests that must be annotated with
  // trace IDs in a way that is specific to the trace back-end being used. The caller must downcast
  // the `SpanObserver` to the expected observer type in order to extract the trace ID.

private:
  kj::Maybe<kj::Own<SpanObserver>> observer;

  friend class SpanBuilder;
};

class SpanBuilder {
  // Interface for writing a span. Essentially, this is a mutable interface to a `Span` object,
  // given only to the code which is meant to create the span, whereas code that merely collects
  // and reports spans gets the `Span` type.
  //
  // The reason we use a separate builder type rather than rely on constness is so that the methods
  // can be no-ops when there is no observer, avoiding unnecessary allocations. To allow for this,
  // SpanBuilder is designed to be write-only -- you cannot read back the content. Only the
  // observer (if there is one) receives the content.

public:
  explicit SpanBuilder(kj::Maybe<kj::Own<SpanObserver>> observer, kj::StringPtr operationName,
                       kj::Date startTime = kj::systemPreciseCalendarClock().now())
      { KJ_IF_MAYBE(o, observer) { state.emplace(kj::mv(*o), operationName, startTime); } }
  // Create a new top-level span that will report to the given observer. If the observer is null,
  // no data is collected.
  //
  // `operationName` should be a string literal with infinite lifetime.

  SpanBuilder(decltype(nullptr)) {}
  // Make a SpanBuilder that ignores all calls. (Useful if you want to assign it later.)

  SpanBuilder(SpanBuilder&& other) = default;
  SpanBuilder& operator=(SpanBuilder&& other);  // ends the existing span and starts a new one
  KJ_DISALLOW_COPY(SpanBuilder);

  ~SpanBuilder() noexcept(false);

  void end();
  // Finishes and submits the span. This is done implicitly by the destructor, but sometimes it's
  // useful to be able to submit early. The SpanBuilder ignores all further method calls after this
  // is invoked.

  bool isObserved() { return state != nullptr; }
  // Useful to skip unnecessary code when not observed.

  SpanBuilder newChild(kj::StringPtr operationName,
      kj::Date startTime = kj::systemPreciseCalendarClock().now());
  // Create a new child span.
  //
  // `operationName` should be a string literal with infinite lifetime.

  void setOperationName(kj::StringPtr operationName);
  // Change the operation name from what was specified at span creation.
  //
  // `operationName` should be a string literal with infinite lifetime.

  using TagValue = Span::TagValue;
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
  struct State {
    kj::Own<SpanObserver> observer;
    // The observer, or null if not being observed.

    Span span;
    // The under-construction span.

    State(kj::Own<SpanObserver> observer, kj::StringPtr operationName,
          kj::Date startTime)
        : observer(kj::mv(observer)), span(operationName, startTime) {}
  };
  kj::Maybe<State> state;

  friend class SpanParent;
};

class SpanObserver: public kj::Refcounted {
  // Abstract interface for observing trace spans reported by the runtime. Different
  // implementations might support different tracing back-ends, e.g. Trace Workers, Jaeger, or
  // whatever infrastrure you prefer to use for this.
  //
  // A new SpanObserver is created at the start of each Span. The observer is used to report the
  // span data at the end of the span, as well as to construct child observers.

public:
  virtual kj::Own<SpanObserver> newChild() = 0;
  // Allocate a new child span.
  //
  // Note that children can be created long after a span has completed.

  virtual void report(const Span& span) = 0;
  // Report the span data. Called at the end of the span.
  //
  // This should always be called exactly once per observer.
};

inline SpanParent::SpanParent(SpanBuilder& builder)
    : observer(builder.state.map(
          [](SpanBuilder::State& state) { return kj::addRef(*state.observer); })) {}

inline SpanParent SpanParent::addRef() {
  return SpanParent(mapAddRef(observer));
}

inline SpanBuilder SpanParent::newChild(kj::StringPtr operationName, kj::Date startTime) {
  return SpanBuilder(observer.map([](kj::Own<SpanObserver>& obs) { return obs->newChild(); }),
                     operationName, startTime);
}

inline SpanBuilder SpanBuilder::newChild(kj::StringPtr operationName, kj::Date startTime) {
  return SpanBuilder(state.map([](State& state) { return state.observer->newChild(); }),
                     operationName, startTime);
}

} // namespace workerd
