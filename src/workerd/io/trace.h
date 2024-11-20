// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/outcome.capnp.h>
#include <workerd/io/worker-interface.capnp.h>
#include <workerd/jsg/memory.h>
#include <workerd/util/own-util.h>
#include <workerd/util/weak-refs.h>

#include <kj/async.h>
#include <kj/map.h>
#include <kj/one-of.h>
#include <kj/refcount.h>
#include <kj/string.h>
#include <kj/time.h>
#include <kj/vector.h>

namespace kj {
enum class HttpMethod;
class EntropySource;
}  // namespace kj

namespace workerd {

using kj::byte;
using kj::uint;

typedef rpc::Trace::Log::Level LogLevel;
typedef rpc::Trace::ExecutionModel ExecutionModel;

namespace tracing {
// A 128-bit globally unique trace identifier. This will be used for both
// external and internal tracing. Specifically, for internal tracing, this
// is used to represent tracing IDs for jaeger traces. For external tracing,
// this is used for both the trace ID and invocation ID for tail workers.
class TraceId final {
 public:
  // A null trace ID. This is only acceptable for use in tests.
  constexpr TraceId(decltype(nullptr)): low(0), high(0) {}

  // A trace ID with the given low and high values.
  constexpr TraceId(uint64_t low, uint64_t high): low(low), high(high) {}

  constexpr TraceId(const TraceId& other) = default;
  constexpr TraceId& operator=(const TraceId& other) = default;

  constexpr TraceId(TraceId&& other): low(other.low), high(other.high) {
    other.low = 0;
    other.high = 0;
  }

  constexpr TraceId& operator=(TraceId&& other) {
    low = other.low;
    high = other.high;
    other.low = 0;
    other.high = 0;
    return *this;
  }

  constexpr TraceId& operator=(decltype(nullptr)) {
    low = 0;
    high = 0;
    return *this;
  }

  constexpr bool operator==(const TraceId& other) const {
    return low == other.low && high == other.high;
  }
  constexpr bool operator==(decltype(nullptr)) const {
    return low == 0 && high == 0;
  }
  constexpr operator bool() const {
    return low || high;
  }

  operator kj::String() const {
    return toGoString();
  }

  // Replicates Jaeger go library's string serialization.
  kj::String toGoString() const;

  // Replicates Jaeger go library's protobuf serialization.
  kj::Array<byte> toProtobuf() const;

  // Replicates W3C Serialization
  kj::String toW3C() const;

  // Creates a random Trace Id, optionally usig a given entropy source. If an
  // entropy source is not given, then we fallback to using BoringSSL's RAND_bytes.
  static TraceId fromEntropy(kj::Maybe<kj::EntropySource&> entropy = kj::none);

  // Replicates Jaeger go library's string serialization.
  static kj::Maybe<TraceId> fromGoString(kj::ArrayPtr<const char> s);

  // Replicates Jaeger go library's protobuf serialization.
  static kj::Maybe<TraceId> fromProtobuf(kj::ArrayPtr<const kj::byte> buf);

  // A null trace ID. This is really only acceptable for use in tests.
  static const TraceId nullId;

  inline uint64_t getLow() const {
    return low;
  }
  inline uint64_t getHigh() const {
    return high;
  }

  static TraceId fromCapnp(rpc::InvocationSpanContext::TraceId::Reader reader);
  void toCapnp(rpc::InvocationSpanContext::TraceId::Builder writer) const;

 private:
  uint64_t low = 0;
  uint64_t high = 0;
};
constexpr TraceId TraceId::nullId = nullptr;

// The InvocationSpanContext is a tuple of a trace id, invocation id, and span id.
// The trace id represents a top-level request and should be shared across all
// invocation spans and events within those spans. The invocation id identifies
// a specific worker invocation. The span id identifies a specific span within an
// invocation. Every invocation of every worker should have an InvocationSpanContext.
// That may or may not have a trigger InvocationSpanContext.
class InvocationSpanContext final: public kj::Refcounted,
                                   public kj::EnableAddRefToThis<InvocationSpanContext> {
 public:
  // Spans within a InvocationSpanContext are identified by a span id that is a
  // monotically increasing number. Every InvocationSpanContext has a root span
  // whose ID is zero. Every child span context created within that context will
  // have a span id that is one greater than the previously created one.
  class SpanIdCounter final: public kj::Refcounted {
   public:
    SpanIdCounter() = default;
    KJ_DISALLOW_COPY_AND_MOVE(SpanIdCounter);

    inline kj::uint next() {
#ifdef KJ_DEBUG
      static constexpr kj::uint kMax = kj::maxValue;
      KJ_ASSERT(id < kMax, "max number of spans exceeded");
#endif
      return id++;
    }

   private:
    kj::uint id = 1;
  };

  // The constructor is public only so kj::rc can see it and create a new instance.
  // User code should use the static factory methods or the newChild method.
  InvocationSpanContext(kj::Badge<InvocationSpanContext>,
      kj::Maybe<kj::Rc<SpanIdCounter>> counter,
      TraceId traceId,
      TraceId invocationId,
      kj::uint spanId = 0,
      kj::Maybe<kj::Rc<InvocationSpanContext>> parentSpanContext = kj::none);
  KJ_DISALLOW_COPY_AND_MOVE(InvocationSpanContext);

  inline const TraceId& getTraceId() const {
    return traceId;
  }

  inline const TraceId& getInvocationId() const {
    return invocationId;
  }

  inline const kj::uint getSpanId() const {
    return spanId;
  }

  inline const kj::Maybe<kj::Rc<InvocationSpanContext>>& getParent() const {
    return parentSpanContext;
  }

  // Creates a new child span. If the current context does not have a counter,
  // then this will assert. If isTrigger() is true then it will not have a
  // counter.
  kj::Rc<InvocationSpanContext> newChild();

  // An InvocationSpanContext is a trigger context if it has no counter. This
  // generally means the SpanContext was create from a capnp message and
  // represents an InvocationSpanContext that was propagated from a parent
  // or triggering context.
  bool isTrigger() const {
    return counter == kj::none;
  }

  // Creates a new InvocationSpanContext. If the triggerContext is given, then its
  // traceId is used as the traceId for the newly created context. Otherwise a new
  // traceId is generated. The invocationId is always generated new and the spanId
  // will be 0 with no parent span.
  static kj::Rc<InvocationSpanContext> newForInvocation(
      kj::Maybe<kj::Rc<InvocationSpanContext>&> triggerContext = kj::none,
      kj::Maybe<kj::EntropySource&> entropySource = kj::none);

  // Creates a new InvocationSpanContext from a capnp message. The returned
  // InvocationSpanContext will not be capable of creating child spans and
  // is considered only a "trigger" span.
  static kj::Maybe<kj::Rc<InvocationSpanContext>> fromCapnp(
      rpc::InvocationSpanContext::Reader reader);
  void toCapnp(rpc::InvocationSpanContext::Builder writer) const;

 private:
  // If there is no counter, then child spans cannot be created from
  // this InvocationSpanContext.
  kj::Maybe<kj::Rc<SpanIdCounter>> counter;
  const TraceId traceId;
  const TraceId invocationId;
  const kj::uint spanId;

  // The parentSpanContext can be either a direct parent or a trigger
  // context. If it is a trigger context, then it should have the same
  // traceId but a different invocationId (unless predictable mode for
  // testing is enabled). The isTrigger() should also return true.
  const kj::Maybe<kj::Rc<InvocationSpanContext>> parentSpanContext;
};

kj::String KJ_STRINGIFY(const TraceId& id);
kj::String KJ_STRINGIFY(const kj::Rc<InvocationSpanContext>& context);
}  // namespace tracing

enum class PipelineLogLevel {
  // WARNING: This must be kept in sync with PipelineDef::LogLevel (which is not in the OSS
  //   release).
  NONE,
  FULL
};

struct Span;

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
  explicit Trace(kj::Maybe<kj::String> stableId,
      kj::Maybe<kj::String> scriptName,
      kj::Maybe<kj::Own<ScriptVersion::Reader>> scriptVersion,
      kj::Maybe<kj::String> dispatchNamespace,
      kj::Maybe<kj::String> scriptId,
      kj::Array<kj::String> scriptTags,
      kj::Maybe<kj::String> entrypoint,
      ExecutionModel executionModel);
  Trace(rpc::Trace::Reader reader);
  ~Trace() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(Trace);

  class FetchEventInfo {
   public:
    class Header;

    explicit FetchEventInfo(
        kj::HttpMethod method, kj::String url, kj::String cfJson, kj::Array<Header> headers);
    FetchEventInfo(rpc::Trace::FetchEventInfo::Reader reader);

    class Header {
     public:
      explicit Header(kj::String name, kj::String value);
      Header(rpc::Trace::FetchEventInfo::Header::Reader reader);

      kj::String name;
      kj::String value;

      void copyTo(rpc::Trace::FetchEventInfo::Header::Builder builder);

      JSG_MEMORY_INFO(Header) {
        tracker.trackField("name", name);
        tracker.trackField("value", value);
      }
    };

    kj::HttpMethod method;
    kj::String url;
    // TODO(perf): It might be more efficient to store some sort of parsed JSON result instead?
    kj::String cfJson;
    kj::Array<Header> headers;

    void copyTo(rpc::Trace::FetchEventInfo::Builder builder);
  };

  class JsRpcEventInfo {
   public:
    explicit JsRpcEventInfo(kj::String methodName);
    JsRpcEventInfo(rpc::Trace::JsRpcEventInfo::Reader reader);

    kj::String methodName;

    void copyTo(rpc::Trace::JsRpcEventInfo::Builder builder);
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

  class TraceEventInfo {
   public:
    class TraceItem;

    explicit TraceEventInfo(kj::ArrayPtr<kj::Own<Trace>> traces);
    TraceEventInfo(rpc::Trace::TraceEventInfo::Reader reader);

    class TraceItem {
     public:
      explicit TraceItem(kj::Maybe<kj::String> scriptName);
      TraceItem(rpc::Trace::TraceEventInfo::TraceItem::Reader reader);

      kj::Maybe<kj::String> scriptName;

      void copyTo(rpc::Trace::TraceEventInfo::TraceItem::Builder builder);
    };

    kj::Vector<TraceItem> traces;

    void copyTo(rpc::Trace::TraceEventInfo::Builder builder);
  };

  class HibernatableWebSocketEventInfo {
   public:
    struct Message {};
    struct Close {
      uint16_t code;
      bool wasClean;
    };
    struct Error {};

    using Type = kj::OneOf<Message, Close, Error>;

    explicit HibernatableWebSocketEventInfo(Type type);
    HibernatableWebSocketEventInfo(rpc::Trace::HibernatableWebSocketEventInfo::Reader reader);

    Type type;

    void copyTo(rpc::Trace::HibernatableWebSocketEventInfo::Builder builder);
    static Type readFrom(rpc::Trace::HibernatableWebSocketEventInfo::Reader reader);
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

  typedef kj::OneOf<FetchEventInfo,
      JsRpcEventInfo,
      ScheduledEventInfo,
      AlarmEventInfo,
      QueueEventInfo,
      EmailEventInfo,
      TraceEventInfo,
      HibernatableWebSocketEventInfo,
      CustomEventInfo>
      EventInfo;
  kj::Maybe<EventInfo> eventInfo;
  // TODO(someday): Support more event types.
  // TODO(someday): Work out what sort of information we may want to convey about the parent
  // trace, if any.

  kj::Maybe<kj::String> scriptName;
  kj::Maybe<kj::Own<ScriptVersion::Reader>> scriptVersion;
  kj::Maybe<kj::String> dispatchNamespace;
  kj::Maybe<kj::String> scriptId;
  kj::Array<kj::String> scriptTags;
  kj::Maybe<kj::String> entrypoint;

  kj::Vector<Log> logs;
  // TODO(o11y): Convert this to actually store spans.
  kj::Vector<Log> spans;
  // A request's trace can have multiple exceptions due to separate request/waitUntil tasks.
  kj::Vector<Exception> exceptions;

  kj::Vector<DiagnosticChannelEvent> diagnosticChannelEvents;

  ExecutionModel executionModel;
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

// =======================================================================================

class WorkerTracer;

// A tracer which records traces for a set of stages. All traces for a pipeline's stages and
// possible subpipeline stages are recorded here, where they can be used to call a pipeline's
// trace worker.
class PipelineTracer final: public kj::Refcounted, public kj::EnableAddRefToThis<PipelineTracer> {
 public:
  // Creates a pipeline tracer (with a possible parent).
  explicit PipelineTracer() = default;
  ~PipelineTracer() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(PipelineTracer);

  // Returns a promise that fulfills when traces are complete.  Only one such promise can
  // exist at a time.
  kj::Promise<kj::Array<kj::Own<Trace>>> onComplete();

  // Makes a tracer for a worker stage.
  kj::Own<WorkerTracer> makeWorkerTracer(PipelineLogLevel pipelineLogLevel,
      ExecutionModel executionModel,
      kj::Maybe<kj::String> scriptId,
      kj::Maybe<kj::String> stableId,
      kj::Maybe<kj::String> scriptName,
      kj::Maybe<kj::Own<ScriptVersion::Reader>> scriptVersion,
      kj::Maybe<kj::String> dispatchNamespace,
      kj::Array<kj::String> scriptTags,
      kj::Maybe<kj::String> entrypoint);

  // Adds a trace from the contents of `reader` this is used in sharded workers to send traces back
  // to the host where tracing was initiated.
  void addTrace(rpc::Trace::Reader reader);

  // When collecting traces from multiple stages in a pipeline, this is called by the
  // tracer for a subordinate stage to add its collected traces to the parent pipeline.
  void addTracesFromChild(kj::ArrayPtr<kj::Own<Trace>> traces);

 private:
  kj::Vector<kj::Own<Trace>> traces;
  kj::Maybe<kj::Own<kj::PromiseFulfiller<kj::Array<kj::Own<Trace>>>>> completeFulfiller;

  friend class WorkerTracer;
};

// Records a worker stage's trace information into a Trace object.  When all references to the
// Tracer are released, its Trace is considered complete and ready for submission. If the Trace to
// write to isn't provided (that already exists in a PipelineTracer), the trace must by extracted
// via extractTrace.
class WorkerTracer final: public kj::Refcounted {
 public:
  explicit WorkerTracer(kj::Rc<PipelineTracer> parentPipeline,
      kj::Own<Trace> trace,
      PipelineLogLevel pipelineLogLevel);
  explicit WorkerTracer(PipelineLogLevel pipelineLogLevel, ExecutionModel executionModel);
  ~WorkerTracer() {
    self->invalidate();
  }
  KJ_DISALLOW_COPY_AND_MOVE(WorkerTracer);

  // Adds log line to trace.  For Spectre, timestamp should only be as accurate as JS Date.now().
  // The isSpan parameter allows for logging spans, which will be emitted after regular logs. There
  // can be at most MAX_USER_SPANS spans in a trace.
  void log(kj::Date timestamp, LogLevel logLevel, kj::String message, bool isSpan = false);
  // Add a span, which will be represented as a log.
  void addSpan(const Span& span, kj::String spanContext);

  // TODO(soon): Eventually:
  //void setMetrics(...) // Or get from MetricsCollector::Request directly?

  void addException(
      kj::Date timestamp, kj::String name, kj::String message, kj::Maybe<kj::String> stack);

  void addDiagnosticChannelEvent(
      kj::Date timestamp, kj::String channel, kj::Array<kj::byte> message);

  // Adds info about the event that triggered the trace.  Must not be called more than once.
  void setEventInfo(kj::Date timestamp, Trace::EventInfo&&);

  // Adds info about the response. Must not be called more than once, and only
  // after passing a FetchEventInfo to setEventInfo().
  void setFetchResponseInfo(Trace::FetchResponseInfo&&);

  void setOutcome(EventOutcome outcome);

  void setCPUTime(kj::Duration cpuTime);

  void setWallTime(kj::Duration wallTime);

  // Used only for a Trace in a process sandbox. Copies the content of this tracer's trace to the
  // builder.
  void extractTrace(rpc::Trace::Builder builder);

  // Sets the main trace of this Tracer to match the content of `reader`. This is used in the
  // parent process after receiving a trace from a process sandbox.
  void setTrace(rpc::Trace::Reader reader);

  kj::Own<WeakRef<WorkerTracer>> addWeakRef() {
    return self->addRef();
  }

 private:
  PipelineLogLevel pipelineLogLevel;
  kj::Own<Trace> trace;

  // own an instance of the pipeline to make sure it doesn't get destroyed
  // before we're finished tracing
  kj::Maybe<kj::Rc<PipelineTracer>> parentPipeline;
  // A weak reference for the internal span submitter. We use this so that the span submitter can
  // add spans while the tracer exists, but does not artifically prolong the lifetime of the tracer
  // which would interfere with span submission (traces get submitted when the worker returns its
  // response, but with e.g. waitUntil() the worker can still be performing tasks afterwards so the
  // span submitter may exist for longer than the tracer).
  kj::Own<WeakRef<WorkerTracer>> self;
};

// =======================================================================================

// Helper function used when setting "truncated_script_id" tags. Truncates the scriptId to 10
// characters.
inline kj::String truncateScriptId(kj::StringPtr id) {
  auto truncatedId = id.first(kj::min(id.size(), 10));
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
//   We might potentially want to give trace workers some access to span tracing as well, but with
//   that the trace worker and span interfaces should still be largely independent of each other;
//   separate span tracing into a separate header.

class SpanBuilder;
class SpanObserver;

struct Span {
  // Represents a trace span. `Span` objects are delivered to `SpanObserver`s for recording. To
  // create a `Span`, use a `SpanBuilder`.

 public:
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
  static constexpr auto MAX_LOGS = 1023;
  uint droppedLogs = 0;

  explicit Span(kj::ConstString operationName, kj::Date startTime)
      : operationName(kj::mv(operationName)),
        startTime(startTime),
        endTime(startTime) {}
};

// An opaque token which can be used to create child spans of some parent. This is typically
// passed down from a caller to a callee when the caller wants to allow the callee to create
// spans for itself that show up as children of the caller's span, but the caller does not
// want to give the callee any other ability to modify the parent span.
class SpanParent {
 public:
  SpanParent(SpanBuilder& builder);

  // Make a SpanParent that causes children not to be reported anywhere.
  SpanParent(decltype(nullptr)) {}

  SpanParent(kj::Maybe<kj::Own<SpanObserver>> observer): observer(kj::mv(observer)) {}

  SpanParent(SpanParent&& other) = default;
  SpanParent& operator=(SpanParent&& other) = default;
  KJ_DISALLOW_COPY(SpanParent);

  SpanParent addRef();

  // Create a new child span.
  //
  // `operationName` should be a string literal with infinite lifetime.
  [[nodiscard]] SpanBuilder newChild(
      kj::ConstString operationName, kj::Date startTime = kj::systemPreciseCalendarClock().now());

  // Useful to skip unnecessary code when not observed.
  bool isObserved() {
    return observer != kj::none;
  }

  // Get the underlying SpanObserver representing the parent span.
  //
  // This is needed in particular when making outbound network requests that must be annotated with
  // trace IDs in a way that is specific to the trace back-end being used. The caller must downcast
  // the `SpanObserver` to the expected observer type in order to extract the trace ID.
  kj::Maybe<SpanObserver&> getObserver() {
    return observer;
  }

 private:
  kj::Maybe<kj::Own<SpanObserver>> observer;
};

// Interface for writing a span. Essentially, this is a mutable interface to a `Span` object,
// given only to the code which is meant to create the span, whereas code that merely collects
// and reports spans gets the `Span` type.
//
// The reason we use a separate builder type rather than rely on constness is so that the methods
// can be no-ops when there is no observer, avoiding unnecessary allocations. To allow for this,
// SpanBuilder is designed to be write-only -- you cannot read back the content. Only the
// observer (if there is one) receives the content.
class SpanBuilder {
 public:
  // Create a new top-level span that will report to the given observer. If the observer is null,
  // no data is collected.
  //
  // `operationName` should be a string literal with infinite lifetime, or somehow otherwise be
  // attached to the observer observing this span.
  explicit SpanBuilder(kj::Maybe<kj::Own<SpanObserver>> observer,
      kj::ConstString operationName,
      kj::Date startTime = kj::systemPreciseCalendarClock().now()) {
    if (observer != kj::none) {
      this->observer = kj::mv(observer);
      span.emplace(kj::mv(operationName), startTime);
    }
  }

  // Make a SpanBuilder that ignores all calls. (Useful if you want to assign it later.)
  SpanBuilder(decltype(nullptr)) {}

  SpanBuilder(SpanBuilder&& other) = default;
  SpanBuilder& operator=(SpanBuilder&& other);  // ends the existing span and starts a new one
  KJ_DISALLOW_COPY(SpanBuilder);

  ~SpanBuilder() noexcept(false);

  // Finishes and submits the span. This is done implicitly by the destructor, but sometimes it's
  // useful to be able to submit early. The SpanBuilder ignores all further method calls after this
  // is invoked.
  void end();

  // Useful to skip unnecessary code when not observed.
  bool isObserved() {
    return observer != kj::none;
  }

  // Get the underlying SpanObserver representing the span.
  //
  // This is needed in particular when making outbound network requests that must be annotated with
  // trace IDs in a way that is specific to the trace back-end being used. The caller must downcast
  // the `SpanObserver` to the expected observer type in order to extract the trace ID.
  kj::Maybe<SpanObserver&> getObserver() {
    return observer;
  }

  // Create a new child span.
  //
  // `operationName` should be a string literal with infinite lifetime.
  [[nodiscard]] SpanBuilder newChild(
      kj::ConstString operationName, kj::Date startTime = kj::systemPreciseCalendarClock().now());

  // Change the operation name from what was specified at span creation.
  //
  // `operationName` should be a string literal with infinite lifetime.
  void setOperationName(kj::ConstString operationName);

  using TagValue = Span::TagValue;
  // `key` must point to memory that will remain valid all the way until this span's data is
  // serialized.
  void setTag(kj::ConstString key, TagValue value);

  // `key` must point to memory that will remain valid all the way until this span's data is
  // serialized.
  //
  // The differences between this and `setTag()` is that logs are timestamped and may have
  // duplicate keys.
  void addLog(kj::Date timestamp, kj::ConstString key, TagValue value);

 private:
  kj::Maybe<kj::Own<SpanObserver>> observer;
  // The under-construction span, or null if the span has ended.
  kj::Maybe<Span> span;

  friend class SpanParent;
};

// Abstract interface for observing trace spans reported by the runtime. Different
// implementations might support different tracing back-ends, e.g. Trace Workers, Jaeger, or
// whatever infrastructure you prefer to use for this.
//
// A new SpanObserver is created at the start of each Span. The observer is used to report the
// span data at the end of the span, as well as to construct child observers.
class SpanObserver: public kj::Refcounted {
 public:
  // Allocate a new child span.
  //
  // Note that children can be created long after a span has completed.
  [[nodiscard]] virtual kj::Own<SpanObserver> newChild() = 0;

  // Report the span data. Called at the end of the span.
  //
  // This should always be called exactly once per observer.
  virtual void report(const Span& span) = 0;
};

inline SpanParent::SpanParent(SpanBuilder& builder): observer(mapAddRef(builder.observer)) {}

inline SpanParent SpanParent::addRef() {
  return SpanParent(mapAddRef(observer));
}

inline SpanBuilder SpanParent::newChild(kj::ConstString operationName, kj::Date startTime) {
  return SpanBuilder(observer.map([](kj::Own<SpanObserver>& obs) { return obs->newChild(); }),
      kj::mv(operationName), startTime);
}

inline SpanBuilder SpanBuilder::newChild(kj::ConstString operationName, kj::Date startTime) {
  return SpanBuilder(observer.map([](kj::Own<SpanObserver>& obs) { return obs->newChild(); }),
      kj::mv(operationName), startTime);
}

// TraceContext to keep track of user tracing/existing tracing better
// TODO(o11y): When creating user child spans, verify that operationName is within a set of
// supported operations. This is important to avoid adding spans to the wrong tracing system.

// Interface to track trace context including both Jaeger and User spans.
// TODO(o11y): Consider fleshing this out to make it a proper class, support adding tags/child spans
// to both,... We expect that tracking user spans will not needed in all places where we have the
// existing spans, so synergies will likely be limited.
struct TraceContext {
  TraceContext(SpanBuilder span, SpanBuilder userSpan)
      : span(kj::mv(span)),
        userSpan(kj::mv(userSpan)) {}
  TraceContext(TraceContext&& other) = default;
  TraceContext& operator=(TraceContext&& other) = default;
  KJ_DISALLOW_COPY(TraceContext);

  SpanBuilder span;
  SpanBuilder userSpan;
};

// TraceContext variant tracking span parents instead. This is useful for code interacting with
// IoChannelFactory::SubrequestMetadata, which often needs to pass through both spans together
// without modifying them. In particular, add functions like newUserChild() here to make it easier
// to add a span for the right parent.
struct TraceParentContext {
  TraceParentContext(TraceContext& tracing)
      : parentSpan(tracing.span),
        userParentSpan(tracing.userSpan) {}
  TraceParentContext(SpanParent span, SpanParent userSpan)
      : parentSpan(kj::mv(span)),
        userParentSpan(kj::mv(userSpan)) {}
  TraceParentContext(TraceParentContext&& other) = default;
  TraceParentContext& operator=(TraceParentContext&& other) = default;
  KJ_DISALLOW_COPY(TraceParentContext);

  SpanParent parentSpan;
  SpanParent userParentSpan;
};

// RAII object that measures the time duration over its lifetime. It tags this duration onto a
// given request span using a specified tag name. Ideal for automatically tracking and logging
// execution times within a scoped block.
class ScopedDurationTagger {
 public:
  explicit ScopedDurationTagger(
      SpanBuilder& span, kj::ConstString key, const kj::MonotonicClock& timer);
  ~ScopedDurationTagger() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(ScopedDurationTagger);

 private:
  SpanBuilder& span;
  kj::ConstString key;
  const kj::MonotonicClock& timer;
  const kj::TimePoint startTime;
};

}  // namespace workerd
