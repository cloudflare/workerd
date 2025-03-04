// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/outcome.capnp.h>
#include <workerd/io/trace.capnp.h>
#include <workerd/io/worker-interface.capnp.h>
#include <workerd/jsg/memory.h>
#include <workerd/util/own-util.h>

#include <kj/async.h>
#include <kj/map.h>
#include <kj/one-of.h>
#include <kj/refcount.h>
#include <kj/string.h>
#include <kj/time.h>
#include <kj/vector.h>

#include <concepts>
#include <initializer_list>

namespace kj {
enum class HttpMethod;
class EntropySource;
}  // namespace kj

namespace workerd {

using kj::byte;
using kj::uint;

typedef rpc::Trace::Log::Level LogLevel;
typedef rpc::Trace::ExecutionModel ExecutionModel;

class Trace;

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

  // Creates a random Trace Id, optionally using a given entropy source. If an
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

// A 64-bit span identifier.
class SpanId final {
 public:
  // A null span ID. This is only acceptable for use in tests.
  constexpr SpanId(decltype(nullptr)): id(0) {}

  constexpr SpanId(uint64_t id): id(id) {}
  constexpr SpanId(const SpanId& other) = default;
  constexpr SpanId& operator=(const SpanId& other) = default;
  constexpr SpanId(SpanId&& other): id(other.id) {
    other.id = 0;
  }
  constexpr SpanId& operator=(SpanId&& other) {
    id = other.id;
    other.id = 0;
    return *this;
  }
  constexpr operator bool() const {
    return id != 0;
  }
  constexpr bool operator==(const SpanId& other) const {
    return id == other.id;
  }
  constexpr bool operator==(decltype(nullptr)) const {
    return id == 0;
  }

  inline operator kj::String() const {
    return toGoString();
  }

  inline operator uint64_t() const {
    return id;
  }

  kj::String toGoString() const;

  static const SpanId nullId;

  constexpr uint64_t getId() const {
    return id;
  }

  static SpanId fromEntropy(kj::Maybe<kj::EntropySource&> entropy = kj::none);

 private:
  uint64_t id;
};
constexpr SpanId SpanId::nullId = nullptr;

// The InvocationSpanContext is a tuple of a trace id, invocation id, and span id.
// The trace id represents a top-level request and should be shared across all
// invocation spans and events within those spans. The invocation id identifies
// a specific worker invocation. The span id identifies a specific span within an
// invocation. Every invocation of every worker should have an InvocationSpanContext.
// That may or may not have a trigger InvocationSpanContext.
class InvocationSpanContext final {
 public:
  // The constructor is public only so kj::rc can see it and create a new instance.
  // User code should use the static factory methods or the newChild method.
  InvocationSpanContext(kj::Badge<InvocationSpanContext>,
      kj::Maybe<kj::EntropySource&> entropySource,
      TraceId traceId,
      TraceId invocationId,
      SpanId spanId,
      kj::Maybe<const InvocationSpanContext&> parentSpanContext = kj::none);

  KJ_DISALLOW_COPY(InvocationSpanContext);

  InvocationSpanContext(InvocationSpanContext&& other) = default;
  InvocationSpanContext& operator=(InvocationSpanContext&& other) = default;

  inline bool operator==(const InvocationSpanContext& other) const {
    return traceId == other.traceId && invocationId == other.invocationId && spanId == other.spanId;
  }

  inline const TraceId& getTraceId() const {
    return traceId;
  }

  inline const TraceId& getInvocationId() const {
    return invocationId;
  }

  inline const SpanId& getSpanId() const {
    return spanId;
  }

  inline kj::Maybe<const InvocationSpanContext&> getParent() const {
    KJ_IF_SOME(p, parentSpanContext) {
      return *p;
    }
    return kj::none;
  }

  // Creates a new child span. If the current context does not have an entropy
  // source this will assert. If isTrigger() is true then it will not have an
  // entropy source.
  InvocationSpanContext newChild() const;

  // An InvocationSpanContext is a trigger context if it has no entropy source.
  // This generally means the SpanContext was create from a capnp message and
  // represents an InvocationSpanContext that was propagated from a parent
  // or triggering context.
  bool isTrigger() const {
    return entropySource == kj::none;
  }

  // Creates a new InvocationSpanContext. If the triggerContext is given, then its
  // traceId is used as the traceId for the newly created context. Otherwise a new
  // traceId is generated. The invocationId is always generated new and the spanId
  // will be 0 with no parent span.
  static InvocationSpanContext newForInvocation(
      kj::Maybe<const InvocationSpanContext&> triggerContext = kj::none,
      kj::Maybe<kj::EntropySource&> entropySource = kj::none);

  // Creates a new InvocationSpanContext from a capnp message. The returned
  // InvocationSpanContext will not be capable of creating child spans and
  // is considered only a "trigger" span.
  static kj::Maybe<InvocationSpanContext> fromCapnp(rpc::InvocationSpanContext::Reader reader);
  void toCapnp(rpc::InvocationSpanContext::Builder writer) const;
  InvocationSpanContext clone() const;

 private:
  // If there is no entropy source, then child spans cannot be created from
  // this InvocationSpanContext.
  kj::Maybe<kj::EntropySource&> entropySource;
  TraceId traceId;
  TraceId invocationId;
  SpanId spanId;

  // The parentSpanContext can be either a direct parent or a trigger
  // context. If it is a trigger context, then it should have the same
  // traceId but a different invocationId (unless predictable mode for
  // testing is enabled). The isTrigger() should also return true.
  kj::Maybe<kj::Own<InvocationSpanContext>> parentSpanContext;
};

kj::String KJ_STRINGIFY(const SpanId& id);
kj::String KJ_STRINGIFY(const TraceId& id);
kj::String KJ_STRINGIFY(const InvocationSpanContext& context);

// The various structs defined below are used in both legacy tail workers
// and streaming tail workers to report tail events.

// Describes a fetch request
struct FetchEventInfo final {
  struct Header;

  explicit FetchEventInfo(
      kj::HttpMethod method, kj::String url, kj::String cfJson, kj::Array<Header> headers);
  FetchEventInfo(rpc::Trace::FetchEventInfo::Reader reader);
  FetchEventInfo(FetchEventInfo&&) = default;
  FetchEventInfo& operator=(FetchEventInfo&&) = default;
  KJ_DISALLOW_COPY(FetchEventInfo);

  struct Header final {
    explicit Header(kj::String name, kj::String value);
    Header(rpc::Trace::FetchEventInfo::Header::Reader reader);
    Header(Header&&) = default;
    Header& operator=(Header&&) = default;
    KJ_DISALLOW_COPY(Header);

    kj::String name;
    kj::String value;

    void copyTo(rpc::Trace::FetchEventInfo::Header::Builder builder);
    Header clone();

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
  FetchEventInfo clone();
};

// Describes a jsrpc request
struct JsRpcEventInfo final {
  explicit JsRpcEventInfo(kj::String methodName);
  JsRpcEventInfo(rpc::Trace::JsRpcEventInfo::Reader reader);
  JsRpcEventInfo(JsRpcEventInfo&&) = default;
  JsRpcEventInfo& operator=(JsRpcEventInfo&&) = default;
  KJ_DISALLOW_COPY(JsRpcEventInfo);

  kj::String methodName;

  void copyTo(rpc::Trace::JsRpcEventInfo::Builder builder);
  JsRpcEventInfo clone();
};

// Describes a scheduled request
struct ScheduledEventInfo final {
  explicit ScheduledEventInfo(double scheduledTime, kj::String cron);
  ScheduledEventInfo(rpc::Trace::ScheduledEventInfo::Reader reader);
  ScheduledEventInfo(ScheduledEventInfo&&) = default;
  ScheduledEventInfo& operator=(ScheduledEventInfo&&) = default;
  KJ_DISALLOW_COPY(ScheduledEventInfo);

  double scheduledTime;
  kj::String cron;

  void copyTo(rpc::Trace::ScheduledEventInfo::Builder builder);
  ScheduledEventInfo clone();
};

// Describes a Durable Object alarm request
struct AlarmEventInfo final {
  explicit AlarmEventInfo(kj::Date scheduledTime);
  AlarmEventInfo(rpc::Trace::AlarmEventInfo::Reader reader);
  AlarmEventInfo(AlarmEventInfo&&) = default;
  AlarmEventInfo& operator=(AlarmEventInfo&&) = default;
  KJ_DISALLOW_COPY(AlarmEventInfo);

  kj::Date scheduledTime;

  void copyTo(rpc::Trace::AlarmEventInfo::Builder builder);
  AlarmEventInfo clone();
};

// Describes a queue worker request
struct QueueEventInfo final {
  explicit QueueEventInfo(kj::String queueName, uint32_t batchSize);
  QueueEventInfo(rpc::Trace::QueueEventInfo::Reader reader);
  QueueEventInfo(QueueEventInfo&&) = default;
  QueueEventInfo& operator=(QueueEventInfo&&) = default;
  KJ_DISALLOW_COPY(QueueEventInfo);

  kj::String queueName;
  uint32_t batchSize;

  void copyTo(rpc::Trace::QueueEventInfo::Builder builder);
  QueueEventInfo clone();
};

// Describes an email request
struct EmailEventInfo final {
  explicit EmailEventInfo(kj::String mailFrom, kj::String rcptTo, uint32_t rawSize);
  EmailEventInfo(rpc::Trace::EmailEventInfo::Reader reader);
  EmailEventInfo(EmailEventInfo&&) = default;
  EmailEventInfo& operator=(EmailEventInfo&&) = default;
  KJ_DISALLOW_COPY(EmailEventInfo);

  kj::String mailFrom;
  kj::String rcptTo;
  uint32_t rawSize;

  void copyTo(rpc::Trace::EmailEventInfo::Builder builder);
  EmailEventInfo clone();
};

// Describes a legacy tail worker request
struct TraceEventInfo final {
  struct TraceItem;

  explicit TraceEventInfo(kj::ArrayPtr<kj::Own<Trace>> traces);
  TraceEventInfo(kj::Array<TraceItem> traces): traces(kj::mv(traces)) {}
  TraceEventInfo(rpc::Trace::TraceEventInfo::Reader reader);
  TraceEventInfo(TraceEventInfo&&) = default;
  TraceEventInfo& operator=(TraceEventInfo&&) = default;
  KJ_DISALLOW_COPY(TraceEventInfo);

  struct TraceItem final {
    explicit TraceItem(kj::Maybe<kj::String> scriptName);
    TraceItem(rpc::Trace::TraceEventInfo::TraceItem::Reader reader);
    TraceItem(TraceItem&&) = default;
    TraceItem& operator=(TraceItem&&) = default;
    KJ_DISALLOW_COPY(TraceItem);

    kj::Maybe<kj::String> scriptName;

    void copyTo(rpc::Trace::TraceEventInfo::TraceItem::Builder builder);
    TraceItem clone();
  };

  kj::Vector<TraceItem> traces;

  void copyTo(rpc::Trace::TraceEventInfo::Builder builder);
  TraceEventInfo clone();
};

// Describes a hibernatable web socket event
struct HibernatableWebSocketEventInfo final {
  struct Message final {};
  struct Close final {
    uint16_t code;
    bool wasClean;
  };
  struct Error final {};

  using Type = kj::OneOf<Message, Close, Error>;

  explicit HibernatableWebSocketEventInfo(Type type);
  HibernatableWebSocketEventInfo(rpc::Trace::HibernatableWebSocketEventInfo::Reader reader);
  HibernatableWebSocketEventInfo(HibernatableWebSocketEventInfo&&) = default;
  HibernatableWebSocketEventInfo& operator=(HibernatableWebSocketEventInfo&&) = default;
  KJ_DISALLOW_COPY(HibernatableWebSocketEventInfo);

  Type type;

  void copyTo(rpc::Trace::HibernatableWebSocketEventInfo::Builder builder);
  HibernatableWebSocketEventInfo clone();
  static Type readFrom(rpc::Trace::HibernatableWebSocketEventInfo::Reader reader);
};

// Describes a custom event
struct CustomEventInfo final {
  explicit CustomEventInfo() {};
  CustomEventInfo(rpc::Trace::CustomEventInfo::Reader reader) {};
};

// Describes a fetch response
struct FetchResponseInfo final {
  explicit FetchResponseInfo(uint16_t statusCode);
  FetchResponseInfo(rpc::Trace::FetchResponseInfo::Reader reader);
  FetchResponseInfo(FetchResponseInfo&&) = default;
  FetchResponseInfo& operator=(FetchResponseInfo&&) = default;
  KJ_DISALLOW_COPY(FetchResponseInfo);

  uint16_t statusCode;

  void copyTo(rpc::Trace::FetchResponseInfo::Builder builder);
  FetchResponseInfo clone();
};

// Describes an event published using the node:diagnostics_channel API
struct DiagnosticChannelEvent final {
  explicit DiagnosticChannelEvent(
      kj::Date timestamp, kj::String channel, kj::Array<kj::byte> message);
  DiagnosticChannelEvent(rpc::Trace::DiagnosticChannelEvent::Reader reader);
  DiagnosticChannelEvent(DiagnosticChannelEvent&&) = default;
  KJ_DISALLOW_COPY(DiagnosticChannelEvent);

  kj::Date timestamp;
  kj::String channel;
  kj::Array<kj::byte> message;

  void copyTo(rpc::Trace::DiagnosticChannelEvent::Builder builder);
  DiagnosticChannelEvent clone();
};

// Describes a log event
struct Log final {
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
  Log clone();
};

// Describes an exception event
struct Exception final {
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
  Exception clone();
};

// Used to indicate that a previously hibernated tail stream is being resumed.
struct Resume final {
  explicit Resume(kj::Maybe<kj::Array<kj::byte>> attachment);
  Resume(rpc::Trace::Resume::Reader reader);
  Resume(Resume&&) = default;
  KJ_DISALLOW_COPY(Resume);
  ~Resume() noexcept(false) = default;

  kj::Maybe<kj::Array<kj::byte>> attachment;

  void copyTo(rpc::Trace::Resume::Builder builder);
  Resume clone();
};

// Used to indicate that a tail stream is being hibernated.
struct Hibernate final {
  explicit Hibernate();
  Hibernate(rpc::Trace::Hibernate::Reader reader);
  Hibernate(Hibernate&&) = default;
  KJ_DISALLOW_COPY(Hibernate);
  ~Hibernate() noexcept(false) = default;

  void copyTo(rpc::Trace::Hibernate::Builder builder);
  Hibernate clone();
};

// EventInfo types are used to describe the onset of an invocation. The FetchEventInfo
// can also be used to describe the start of a fetch subrequest.
using EventInfo = kj::OneOf<FetchEventInfo,
    JsRpcEventInfo,
    ScheduledEventInfo,
    AlarmEventInfo,
    QueueEventInfo,
    EmailEventInfo,
    TraceEventInfo,
    HibernatableWebSocketEventInfo,
    Resume,
    CustomEventInfo>;

template <typename T>
concept AttributeValue = kj::isSameType<kj::String, T>() || kj::isSameType<bool, T>() ||
    kj::isSameType<double, T>() || kj::isSameType<int32_t, T>();

// An Attribute mark is used to add detail to a span over its lifetime.
// The Attribute struct can also be used to provide arbitrary additional
// properties for some other structs.
// Modeled after https://opentelemetry.io/docs/concepts/signals/traces/#attributes
struct Attribute final {
  using Value = kj::OneOf<kj::String, bool, double, int32_t>;
  using Values = kj::Array<Value>;

  explicit Attribute(kj::String name, Value&& value);
  explicit Attribute(kj::String name, Values&& values);

  template <AttributeValue V>
  explicit Attribute(kj::String name, V v): Attribute(kj::mv(name), Value(kj::mv(v))) {}

  template <AttributeValue V>
  explicit Attribute(kj::String name, kj::Array<V> vals)
      : Attribute(kj::mv(name), KJ_MAP(v, vals) { return Value(kj::mv(v)); }) {}

  template <AttributeValue V>
  explicit Attribute(kj::String name, std::initializer_list<V> list)
      : Attribute(kj::mv(name), kj::heapArray<V>(list)) {}

  Attribute(rpc::Trace::Attribute::Reader reader);
  Attribute(Attribute&&) = default;
  Attribute& operator=(Attribute&&) = default;
  KJ_DISALLOW_COPY(Attribute);

  kj::String name;
  Values value;

  void copyTo(rpc::Trace::Attribute::Builder builder);
  Attribute clone();
};
using CustomInfo = kj::Array<Attribute>;

// A Return mark is used to mark the point at which a span operation returned
// a value. For instance, when a fetch subrequest response is received, or when
// the fetch handler returns a Response. Importantly, it does not signal that the
// span has closed, which may not happen for some period of time after the return
// mark is recorded (e.g. due to things like waitUntils or waiting to fully ready
// the response body payload, etc).
struct Return final {
  using Info = kj::OneOf<FetchResponseInfo, CustomInfo>;

  explicit Return(kj::Maybe<Info> info = kj::none);
  Return(rpc::Trace::Return::Reader reader);
  Return(Return&&) = default;
  Return& operator=(Return&&) = default;
  KJ_DISALLOW_COPY(Return);

  kj::Maybe<Info> info = kj::none;

  void copyTo(rpc::Trace::Return::Builder builder);
  Return clone();
};

// A Link mark is used to establish a link from one span to another.
// The optional label can be used to identify the link.
struct Link final {
  explicit Link(const InvocationSpanContext& other, kj::Maybe<kj::String> label = kj::none);
  explicit Link(kj::Maybe<kj::String> label, TraceId traceId, TraceId invocationId, SpanId spanId);
  Link(rpc::Trace::Link::Reader reader);
  Link(Link&&) = default;
  Link& operator=(Link&&) = default;
  KJ_DISALLOW_COPY(Link);

  kj::Maybe<kj::String> label;
  TraceId traceId;
  TraceId invocationId;
  SpanId spanId;

  void copyTo(rpc::Trace::Link::Builder builder);
  Link clone();
};

using Mark = kj::OneOf<DiagnosticChannelEvent, Exception, Log, Return, Link, kj::Array<Attribute>>;

// Marks the opening of a child span within the streaming tail session.
struct SpanOpen final {
  // If the span represents a subrequest, then the info describes the
  // details of that subrequest.
  using Info = kj::OneOf<FetchEventInfo, JsRpcEventInfo, CustomInfo>;

  explicit SpanOpen(
      kj::Maybe<kj::String> operationName = kj::none, kj::Maybe<Info> info = kj::none);
  SpanOpen(rpc::Trace::SpanOpen::Reader reader);
  SpanOpen(SpanOpen&&) = default;
  SpanOpen& operator=(SpanOpen&&) = default;
  KJ_DISALLOW_COPY(SpanOpen);

  kj::Maybe<kj::String> operationName = kj::none;
  kj::Maybe<Info> info = kj::none;

  void copyTo(rpc::Trace::SpanOpen::Builder builder);
  SpanOpen clone();
};

// Marks the closing of a child span within the streaming tail session.
// Once emitted, no further mark events should occur within the closed
// span.
struct SpanClose final {
  explicit SpanClose(EventOutcome outcome = EventOutcome::OK);
  SpanClose(rpc::Trace::SpanClose::Reader reader);
  SpanClose(SpanClose&&) = default;
  SpanClose& operator=(SpanClose&&) = default;
  KJ_DISALLOW_COPY(SpanClose);

  EventOutcome outcome = EventOutcome::OK;

  void copyTo(rpc::Trace::SpanClose::Builder builder);
  SpanClose clone();
};

// The Onset and Outcome event types are special forms of SpanOpen and
// SpanClose that explicitly mark the start and end of the root span.
// A streaming tail session will always begin with an Onset event, and
// always end with an Outcome event.
struct Onset final {
  using Info = EventInfo;

  // Information about the worker that is being tailed.
  struct WorkerInfo final {
    ExecutionModel executionModel = ExecutionModel::STATELESS;
    kj::Maybe<kj::String> scriptName;
    kj::Maybe<kj::Own<ScriptVersion::Reader>> scriptVersion;
    kj::Maybe<kj::String> dispatchNamespace;
    kj::Maybe<kj::Array<kj::String>> scriptTags;
    kj::Maybe<kj::String> entrypoint;

    WorkerInfo clone() const;
  };

  struct TriggerContext final {
    TraceId traceId;
    TraceId invocationId;
    SpanId spanId;

    TriggerContext(TraceId traceId, TraceId invocationId, SpanId spanId)
        : traceId(kj::mv(traceId)),
          invocationId(kj::mv(invocationId)),
          spanId(kj::mv(spanId)) {}

    TriggerContext(const InvocationSpanContext& ctx)
        : TriggerContext(ctx.getTraceId(), ctx.getInvocationId(), ctx.getSpanId()) {}
  };

  explicit Onset(
      Info&& info, WorkerInfo&& workerInfo, kj::Maybe<TriggerContext> maybeTrigger = kj::none);

  Onset(rpc::Trace::Onset::Reader reader);
  Onset(Onset&&) = default;
  Onset& operator=(Onset&&) = default;
  KJ_DISALLOW_COPY(Onset);

  Info info;
  WorkerInfo workerInfo;
  kj::Maybe<TriggerContext> trigger;

  void copyTo(rpc::Trace::Onset::Builder builder);
  Onset clone();
};

struct Outcome final {
  explicit Outcome(EventOutcome outcome, kj::Duration cpuTime, kj::Duration wallTime);
  Outcome(rpc::Trace::Outcome::Reader reader);
  Outcome(Outcome&&) = default;
  Outcome& operator=(Outcome&&) = default;
  KJ_DISALLOW_COPY(Outcome);

  EventOutcome outcome = EventOutcome::OK;
  kj::Duration cpuTime;
  kj::Duration wallTime;

  void copyTo(rpc::Trace::Outcome::Builder builder);
  Outcome clone();
};

// A streaming tail worker receives a series of Tail Events. Tail events always
// occur within an InvocationSpanContext. The first TailEvent delivered to a
// streaming tail session is always an Onset. The final TailEvent delivered is
// always an Outcome. Between those can be any number of SpanOpen, SpanClose,
// and Mark events. Every SpanOpen *must* be associated with a SpanClose unless
// the stream was abruptly terminated.
struct TailEvent final {
  using Event = kj::OneOf<Onset, Outcome, Hibernate, SpanOpen, SpanClose, Mark>;

  explicit TailEvent(
      const InvocationSpanContext& context, kj::Date timestamp, kj::uint sequence, Event&& event);
  TailEvent(TraceId traceId,
      TraceId invocationId,
      SpanId spanId,
      kj::Date timestamp,
      kj::uint sequence,
      Event&& event);
  TailEvent(rpc::Trace::TailEvent::Reader reader);
  TailEvent(TailEvent&&) = default;
  TailEvent& operator=(TailEvent&&) = default;
  KJ_DISALLOW_COPY(TailEvent);

  // The invocation span context this event is associated with.
  TraceId traceId;
  TraceId invocationId;
  SpanId spanId;

  kj::Date timestamp;  // Unix epoch, Spectre-mitigated resolution
  kj::uint sequence;

  Event event;

  void copyTo(rpc::Trace::TailEvent::Builder builder);
  TailEvent clone();
};
}  // namespace tracing

enum class PipelineLogLevel {
  // WARNING: This must be kept in sync with PipelineDef::LogLevel (which is not in the OSS
  //   release).
  NONE,
  FULL
};

struct Span;
struct CompleteSpan;

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

  // Empty for toplevel worker.
  kj::Maybe<kj::String> stableId;

  // We treat the origin value as "unset".
  kj::Date eventTimestamp = kj::UNIX_EPOCH;

  kj::Maybe<tracing::EventInfo> eventInfo;

  // TODO(someday): Work out what sort of information we may want to convey about the parent
  // trace, if any.

  kj::Maybe<kj::String> scriptName;
  kj::Maybe<kj::Own<ScriptVersion::Reader>> scriptVersion;
  kj::Maybe<kj::String> dispatchNamespace;
  kj::Maybe<kj::String> scriptId;
  kj::Array<kj::String> scriptTags;
  kj::Maybe<kj::String> entrypoint;

  kj::Vector<tracing::Log> logs;
  kj::Vector<CompleteSpan> spans;
  // A request's trace can have multiple exceptions due to separate request/waitUntil tasks.
  kj::Vector<tracing::Exception> exceptions;

  kj::Vector<tracing::DiagnosticChannelEvent> diagnosticChannelEvents;

  ExecutionModel executionModel;
  EventOutcome outcome = EventOutcome::UNKNOWN;

  kj::Maybe<tracing::FetchResponseInfo> fetchResponseInfo;

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

// Utility functions for handling span tags.
void serializeTagValue(rpc::TagValue::Builder builder, const Span::TagValue& value);
Span::TagValue deserializeTagValue(rpc::TagValue::Reader value);

// Stringify and clone for span tags, getting this to work with KJ_STRINGIFY() appears exceedingly
// difficult.
kj::String spanTagStr(const Span::TagValue& tag);
Span::TagValue spanTagClone(const Span::TagValue& tag);

struct CompleteSpan {
  // Represents a completed span within user tracing.
  uint64_t spanId;
  uint64_t parentSpanId;

  kj::ConstString operationName;
  kj::Date startTime;
  kj::Date endTime;
  Span::TagMap tags;

  CompleteSpan(rpc::UserSpanData::Reader reader);
  void copyTo(rpc::UserSpanData::Builder builder);
  explicit CompleteSpan(kj::ConstString operationName, kj::Date startTime)
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
