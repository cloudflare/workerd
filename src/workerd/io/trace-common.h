// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <workerd/io/outcome.capnp.h>
#include <workerd/io/worker-interface.capnp.h>
#include <workerd/jsg/memory.h>

#include <kj/map.h>
#include <kj/one-of.h>
#include <kj/string.h>
#include <kj/time.h>

namespace kj {
enum class HttpMethod;
}  // namespace kj

namespace workerd {

// trace-commmon.h defines the common data structures used by both the legacy and streaming trace
// models. These are primarily structs adapting the capnp schema definitions of the types.

using LogLevel = rpc::Trace::Log::Level;
using ExecutionModel = rpc::Trace::ExecutionModel;

enum class PipelineLogLevel {
  // WARNING: This must be kept in sync with PipelineDef::LogLevel (which is not in the OSS
  //   release).
  NONE,
  FULL
};

// A legacy in-memory accumulated Trace
class Trace;

namespace trace {

template <typename T>
concept IsEnum = std::is_enum<T>::value;

// A trace::Tag is a key-value pair that can be attached to multiple types of objects
// as an extension field. They can be used to provide additional context, extend
// types, etc.
//
// The tag key can be either a string or a numeric index. When a numeric index is
// used, the tag represents some internally defined field in the runtime that should
// be defined in a capnp schema somewhere. Workerd does not define any of these keys.
//
// The tag value can be one of either a boolean, signed or unsigned 64-bit integer,
// a double, a string, or an arbitrary byte array. When a byte array is used, the
// specific format is specific to the tag key. No general assumptions can be made
// about the value without knowledge of the specific key.
struct Tag final {
  using TagValue = kj::OneOf<bool, int64_t, uint64_t, double, kj::String, kj::Array<kj::byte>>;
  using TagKey = kj::OneOf<kj::String, uint32_t>;
  TagKey key;
  TagValue value;
  explicit Tag(TagKey key, TagValue value);

  template <IsEnum Key>
  explicit Tag(Key key, TagValue value): Tag(static_cast<uint32_t>(key), kj::mv(value)) {}

  Tag(rpc::Trace::Tag::Reader reader);

  bool keyMatches(kj::OneOf<kj::StringPtr, uint32_t> key);

  template <IsEnum Key>
  inline bool keyMatches(Key key) {
    return keyMatches(static_cast<uint32_t>(key));
  }

  void copyTo(rpc::Trace::Tag::Builder builder) const;
  Tag clone() const;
};
using Tags = kj::Array<Tag>;

// Metadata describing the start of a fetch request.
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

    kj::String name;
    kj::String value;

    void copyTo(rpc::Trace::FetchEventInfo::Header::Builder builder) const;

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

  void copyTo(rpc::Trace::FetchEventInfo::Builder builder) const;

  FetchEventInfo clone() const;
};

// The FetchResponseInfo struct is used to attach additional detail about the conclusion
// of fetch request. It is to be included in a closing Span event. The FetchResponseInfo
// is not an event on it's own.
struct FetchResponseInfo final {
  explicit FetchResponseInfo(uint16_t statusCode);
  FetchResponseInfo(rpc::Trace::FetchResponseInfo::Reader reader);
  FetchResponseInfo(FetchResponseInfo&&) = default;
  FetchResponseInfo& operator=(FetchResponseInfo&&) = default;
  KJ_DISALLOW_COPY(FetchResponseInfo);

  uint16_t statusCode;

  void copyTo(rpc::Trace::FetchResponseInfo::Builder builder) const;
  FetchResponseInfo clone() const;
};

// Metadata describing the start of a JS RPC event
struct JsRpcEventInfo final {
  explicit JsRpcEventInfo(kj::String methodName);
  JsRpcEventInfo(rpc::Trace::JsRpcEventInfo::Reader reader);
  JsRpcEventInfo(JsRpcEventInfo&&) = default;
  JsRpcEventInfo& operator=(JsRpcEventInfo&&) = default;
  KJ_DISALLOW_COPY(JsRpcEventInfo);

  kj::String methodName;

  void copyTo(rpc::Trace::JsRpcEventInfo::Builder builder) const;
  JsRpcEventInfo clone() const;
};

// Metadata describing the start of a scheduled worker event (cron workers).
struct ScheduledEventInfo final {
  explicit ScheduledEventInfo(double scheduledTime, kj::String cron);
  ScheduledEventInfo(rpc::Trace::ScheduledEventInfo::Reader reader);
  ScheduledEventInfo(ScheduledEventInfo&&) = default;
  ScheduledEventInfo& operator=(ScheduledEventInfo&&) = default;
  KJ_DISALLOW_COPY(ScheduledEventInfo);

  double scheduledTime;
  kj::String cron;

  void copyTo(rpc::Trace::ScheduledEventInfo::Builder builder) const;
  ScheduledEventInfo clone() const;
};

// Metadata describing the start of an alarm event.
struct AlarmEventInfo final {
  explicit AlarmEventInfo(kj::Date scheduledTime);
  AlarmEventInfo(rpc::Trace::AlarmEventInfo::Reader reader);
  AlarmEventInfo(AlarmEventInfo&&) = default;
  AlarmEventInfo& operator=(AlarmEventInfo&&) = default;
  KJ_DISALLOW_COPY(AlarmEventInfo);

  kj::Date scheduledTime;

  void copyTo(rpc::Trace::AlarmEventInfo::Builder builder) const;
  AlarmEventInfo clone() const;
};

// Metadata describing the start of a queue event.
struct QueueEventInfo final {
  explicit QueueEventInfo(kj::String queueName, uint32_t batchSize);
  QueueEventInfo(rpc::Trace::QueueEventInfo::Reader reader);
  QueueEventInfo(QueueEventInfo&&) = default;
  QueueEventInfo& operator=(QueueEventInfo&&) = default;
  KJ_DISALLOW_COPY(QueueEventInfo);

  kj::String queueName;
  uint32_t batchSize;

  void copyTo(rpc::Trace::QueueEventInfo::Builder builder) const;
  QueueEventInfo clone() const;
};

// Metadata describing the start of an email event.
struct EmailEventInfo final {
  explicit EmailEventInfo(kj::String mailFrom, kj::String rcptTo, uint32_t rawSize);
  EmailEventInfo(rpc::Trace::EmailEventInfo::Reader reader);
  EmailEventInfo(EmailEventInfo&&) = default;
  EmailEventInfo& operator=(EmailEventInfo&&) = default;
  KJ_DISALLOW_COPY(EmailEventInfo);

  kj::String mailFrom;
  kj::String rcptTo;
  uint32_t rawSize;

  void copyTo(rpc::Trace::EmailEventInfo::Builder builder) const;
  EmailEventInfo clone() const;
};

// Metadata describing the start of a hibernatable web socket event.
struct HibernatableWebSocketEventInfo final {
  struct Message {};
  struct Close {
    uint16_t code;
    bool wasClean;
  };
  struct Error {};

  using Type = kj::OneOf<Message, Close, Error>;

  explicit HibernatableWebSocketEventInfo(Type type);
  HibernatableWebSocketEventInfo(rpc::Trace::HibernatableWebSocketEventInfo::Reader reader);
  HibernatableWebSocketEventInfo(HibernatableWebSocketEventInfo&&) = default;
  HibernatableWebSocketEventInfo& operator=(HibernatableWebSocketEventInfo&&) = default;
  KJ_DISALLOW_COPY(HibernatableWebSocketEventInfo);

  Type type;

  void copyTo(rpc::Trace::HibernatableWebSocketEventInfo::Builder builder) const;
  static Type readFrom(rpc::Trace::HibernatableWebSocketEventInfo::Reader reader);
  HibernatableWebSocketEventInfo clone() const;
};

// Metadata describing the start of a custom event (currently unused) but here
// to support legacy trace use cases.
struct CustomEventInfo final {
  explicit CustomEventInfo() {};
  CustomEventInfo(rpc::Trace::CustomEventInfo::Reader reader) {};
  CustomEventInfo(CustomEventInfo&&) = default;
  CustomEventInfo& operator=(CustomEventInfo&&) = default;
  KJ_DISALLOW_COPY(CustomEventInfo);

  CustomEventInfo clone() const {
    return CustomEventInfo();
  }
};

// Metadata describing the start of a legacy tail event (that is, the event
// that delivers collected traces to a legacy tail worker)
struct TraceEventInfo final {
  struct TraceItem;

  explicit TraceEventInfo(kj::ArrayPtr<kj::Own<Trace>> traces);
  explicit TraceEventInfo(kj::Array<TraceItem> traces);
  TraceEventInfo(rpc::Trace::TraceEventInfo::Reader reader);
  TraceEventInfo(TraceEventInfo&&) = default;
  TraceEventInfo& operator=(TraceEventInfo&&) = default;
  KJ_DISALLOW_COPY(TraceEventInfo);

  struct TraceItem {
    explicit TraceItem(kj::Maybe<kj::String> scriptName);
    TraceItem(rpc::Trace::TraceEventInfo::TraceItem::Reader reader);
    TraceItem(TraceItem&&) = default;
    TraceItem& operator=(TraceItem&&) = default;
    KJ_DISALLOW_COPY(TraceItem);

    kj::Maybe<kj::String> scriptName;

    void copyTo(rpc::Trace::TraceEventInfo::TraceItem::Builder builder) const;
    TraceItem clone() const;
  };

  kj::Vector<TraceItem> traces;

  void copyTo(rpc::Trace::TraceEventInfo::Builder builder) const;
  TraceEventInfo clone() const;
};

// The set of structs that make up the EventInfo union all represent trigger
// events for worker requests... That is, for instance, when a worker receives
// a fetch request, a FetchEventInfo will be emitted; if the worker receives a
// JS RPC request, a JsRpcEventInfo will be emitted; and so on.
using EventInfo = kj::OneOf<FetchEventInfo,
    JsRpcEventInfo,
    ScheduledEventInfo,
    AlarmEventInfo,
    QueueEventInfo,
    EmailEventInfo,
    TraceEventInfo,
    HibernatableWebSocketEventInfo,
    CustomEventInfo>;

// Metadata describing the onset of a trace session. The first event in any trace session
// will always be an Onset event. It details information about which worker, script, etc
// is being traced. Every trace session will have exactly one Onset event.
struct Onset final {
  explicit Onset() = default;
  Onset(kj::Maybe<kj::String> scriptName,
      kj::Maybe<kj::Own<ScriptVersion::Reader>> scriptVersion,
      kj::Maybe<kj::String> dispatchNamespace,
      kj::Maybe<kj::String> scriptId,
      kj::Array<kj::String> scriptTags,
      kj::Maybe<kj::String> entrypoint,
      ExecutionModel executionModel,
      Tags tags = nullptr);
  Onset(rpc::Trace::Onset::Reader reader);
  Onset(Onset&&) = default;
  Onset& operator=(Onset&&) = default;
  KJ_DISALLOW_COPY(Onset);

  // Note that all of these fields could be represented by tags but are kept
  // as separate fields for legacy reasons.
  kj::Maybe<kj::String> scriptName = kj::none;
  kj::Maybe<kj::Own<ScriptVersion::Reader>> scriptVersion = kj::none;
  kj::Maybe<kj::String> dispatchNamespace = kj::none;
  kj::Maybe<kj::String> scriptId = kj::none;
  kj::Array<kj::String> scriptTags = nullptr;
  kj::Maybe<kj::String> entrypoint = kj::none;
  ExecutionModel executionModel;
  kj::Maybe<EventInfo> info = kj::none;
  Tags tags = nullptr;

  void copyTo(rpc::Trace::Onset::Builder builder) const;
  Onset clone() const;
};

// Used to describe the final outcome of the trace. Every trace session should
// have at most a single Outcome event that is the final event in the stream.
struct Outcome final {
  using Info = kj::OneOf<FetchResponseInfo, Tags>;
  Outcome() = default;
  explicit Outcome(EventOutcome outcome, kj::Maybe<Info> info = kj::none);
  Outcome(rpc::Trace::Outcome::Reader reader);
  Outcome(Outcome&&) = default;
  Outcome& operator=(Outcome&&) = default;
  KJ_DISALLOW_COPY(Outcome);

  EventOutcome outcome = EventOutcome::UNKNOWN;

  // The closing outcome event should include an info field that matches the kind of
  // event that started the span. For instance, if the span was started with a
  // FetchEventInfo event, then the outcome should be closed with a FetchResponseInfo.
  kj::Maybe<Info> info;

  void copyTo(rpc::Trace::Outcome::Builder builder) const;
  Outcome clone() const;
};

// The SpanClose struct identifies the *close* of a span in the stream. A span is
// a logical grouping of events. Spans can be nested.
struct SpanClose final {
  using Outcome = rpc::Trace::SpanClose::SpanOutcome;
  explicit SpanClose(Outcome outcome, Tags tags = nullptr);
  SpanClose(rpc::Trace::SpanClose::Reader reader);
  SpanClose(SpanClose&&) = default;
  SpanClose& operator=(SpanClose&&) = default;
  KJ_DISALLOW_COPY(SpanClose);

  Outcome outcome;
  Tags tags;

  void copyTo(rpc::Trace::SpanClose::Builder builder) const;
  SpanClose clone() const;
};

// Describes an event caused by use of the Node.js diagnostics-channel API.
struct DiagnosticChannelEvent final {
  explicit DiagnosticChannelEvent(
      kj::Date timestamp, kj::String channel, kj::Array<kj::byte> message);
  DiagnosticChannelEvent(rpc::Trace::DiagnosticChannelEvent::Reader reader);
  DiagnosticChannelEvent(DiagnosticChannelEvent&&) = default;
  DiagnosticChannelEvent& operator=(DiagnosticChannelEvent&&) = default;
  KJ_DISALLOW_COPY(DiagnosticChannelEvent);

  kj::Date timestamp;
  kj::String channel;
  kj::Array<kj::byte> message;

  void copyTo(rpc::Trace::DiagnosticChannelEvent::Builder builder) const;
  DiagnosticChannelEvent clone() const;
};

// The Log struct is used by the legacy tracing (v1 tail workers) model and
// is unchanged for treaming traces. This is the struct that is used, for
// instance, to carry console.log outputs into the trace.
struct Log final {
  explicit Log(kj::Date timestamp, LogLevel logLevel, kj::String message);
  Log(rpc::Trace::Log::Reader reader);
  Log(Log&&) = default;
  KJ_DISALLOW_COPY(Log);

  // Only as accurate as Worker's Date.now(), for Spectre mitigation.
  kj::Date timestamp;

  LogLevel logLevel;
  // TODO(soon): Just string for now.  Eventually, capture serialized JS objects.
  kj::String message;

  void copyTo(rpc::Trace::Log::Builder builder) const;
  Log clone() const;
};

// The LogV2 struct is used by the streaming trace model. It serves the same
// purpose as the Log struct above but allows for v8 serialized binary data
// as an alternative to plain text and allows for additional metadata tags
// to be added. It is separated out into a new struct in order to prevent
// introducing any backwards compat issues with the original legacy trace
// implementation.
// TODO(soon): Coallesce Log and LogV2
struct LogV2 final {
  explicit LogV2(kj::Date timestamp,
      LogLevel logLevel,
      kj::OneOf<kj::Array<kj::byte>, kj::String> message,
      Tags tags = nullptr,
      bool truncated = false);
  LogV2(rpc::Trace::LogV2::Reader reader);
  LogV2(LogV2&&) = default;
  LogV2& operator=(LogV2&&) = default;
  KJ_DISALLOW_COPY(LogV2);

  // Only as accurate as Worker's Date.now(), for Spectre mitigation.
  kj::Date timestamp;

  LogLevel logLevel;
  kj::OneOf<kj::Array<kj::byte>, kj::String> message;
  Tags tags = nullptr;
  bool truncated = false;

  void copyTo(rpc::Trace::LogV2::Builder builder) const;
  LogV2 clone() const;
};

// Describes metadata of an Exception that is added to the trace.
struct Exception final {
  // The Detail here is additional, optional information about the exception.
  // Intende to provide additional context when appropriate. Not every error
  // will have a Detail.
  struct Detail {
    // If the JS Error object has a cause property, then it will be serialized
    // into it's own Exception instance and attached here.
    kj::Maybe<kj::Own<Exception>> cause = kj::none;
    // If the JS Error object is an AggregateError or SuppressedError, or if
    // it has an errors property like those standard types, then those errors
    // will be serialized into their own Exception instances and attached here.
    kj::Array<kj::Own<Exception>> errors = nullptr;

    // These flags match the additional workers-specific properties that we
    // add to errors. See makeInternalError in jsg/util.c++.
    bool remote = false;
    bool retryable = false;
    bool overloaded = false;
    bool durableObjectReset = false;
    Tags tags = nullptr;

    Detail clone() const;
  };

  explicit Exception(kj::Date timestamp,
      kj::String name,
      kj::String message,
      kj::Maybe<kj::String> stack,
      kj::Maybe<Detail> detail = kj::none);
  Exception(rpc::Trace::Exception::Reader reader);
  Exception(Exception&&) = default;
  Exception& operator=(Exception&&) = default;
  KJ_DISALLOW_COPY(Exception);

  // Only as accurate as Worker's Date.now(), for Spectre mitigation.
  kj::Date timestamp;

  kj::String name;
  kj::String message;
  kj::Maybe<kj::String> stack;
  Detail detail;

  void copyTo(rpc::Trace::Exception::Builder builder) const;
  Exception clone() const;
};

// Describes the start (and kind) of a subrequest initiated during the trace.
// For instance, a fetch request or a JS RPC call.
struct Subrequest final {
  using Info = kj::OneOf<FetchEventInfo, JsRpcEventInfo>;
  explicit Subrequest(uint32_t id, kj::Maybe<Info> info);
  Subrequest(rpc::Trace::Subrequest::Reader reader);
  Subrequest(Subrequest&&) = default;
  Subrequest& operator=(Subrequest&&) = default;
  KJ_DISALLOW_COPY(Subrequest);

  uint32_t id;
  kj::Maybe<Info> info;

  void copyTo(rpc::Trace::Subrequest::Builder builder) const;
  Subrequest clone() const;
};

// Describes the results of a subrequest initiated during the trace. The id
// must match a previously emitted Subrequest event, and the info (if provided)
// should correlate to the kind of subrequest that was made.
struct SubrequestOutcome final {
  using Info = kj::OneOf<FetchResponseInfo, Tags>;
  explicit SubrequestOutcome(uint32_t id, kj::Maybe<Info> info, SpanClose::Outcome outcome);
  SubrequestOutcome(rpc::Trace::SubrequestOutcome::Reader reader);
  SubrequestOutcome(SubrequestOutcome&&) = default;
  SubrequestOutcome& operator=(SubrequestOutcome&&) = default;
  KJ_DISALLOW_COPY(SubrequestOutcome);

  uint32_t id;
  kj::Maybe<Info> info;

  // A subrequest is a technically a special form of span, and therefore can
  // have a Span outcome.
  SpanClose::Outcome outcome;

  void copyTo(rpc::Trace::SubrequestOutcome::Builder builder) const;
  SubrequestOutcome clone() const;
};

// A Mark is a simple event that can be used to mark a significant point in the
// trace. This currently has no analog in the current tail workers model.
struct Mark final {
  explicit Mark(kj::String name);
  Mark(rpc::Trace::Mark::Reader reader);
  Mark(Mark&&) = default;
  Mark& operator=(Mark&&) = default;
  KJ_DISALLOW_COPY(Mark);

  kj::String name;

  void copyTo(rpc::Trace::Mark::Builder builder) const;
  Mark clone() const;
};

// A Metric is a key-value pair that can be emitted as part of the trace. Metrics
// include things like CPU time, Wall time, isolate heap memory usage, etc. The
// structure is left intentionally flexible to allow for a wide range of possible
// metrics to be emitted. This is a new feature in the streaming trace model.
struct Metric final {
  using Key = kj::OneOf<kj::String, uint32_t>;
  using Type = rpc::Trace::Metric::Type;

  enum class Common {
    CPU_TIME,
    WALL_TIME,
  };

  explicit Metric(Type type, Key key, double value);

  template <IsEnum K>
  explicit Metric(Type type, K key, double value)
      : Metric(type, static_cast<uint32_t>(key), value) {}

  Metric(rpc::Trace::Metric::Reader reader);
  Metric(Metric&&) = default;
  Metric& operator=(Metric&&) = default;
  KJ_DISALLOW_COPY(Metric);

  Type type;
  Key key;
  double value;

  bool keyMatches(kj::OneOf<kj::StringPtr, uint32_t> key);

  template <IsEnum Key>
  inline bool keyMatches(Key key) {
    return keyMatches(static_cast<uint32_t>(key));
  }

  void copyTo(rpc::Trace::Metric::Builder builder) const;
  Metric clone() const;

  static inline Metric forWallTime(kj::Duration duration) {
    return Metric(Type::COUNTER, Common::WALL_TIME, duration / kj::MILLISECONDS);
  }

  static Metric forCpuTime(kj::Duration duration) {
    return Metric(Type::COUNTER, Common::CPU_TIME, duration / kj::MILLISECONDS);
  }
};
using Metrics = kj::Array<Metric>;

// A Dropped event can be used to indicate that a specific range of events were
// dropped from the stream. This would typically be used in cases where the process
// may be too overloaded to successfully deliver all events to the tail worker.
// The start/end values are inclusive and indicate the range of events (by sequence
// number) that were dropped.
struct Dropped final {
  explicit Dropped(uint32_t start, uint32_t end);
  Dropped(rpc::Trace::Dropped::Reader reader);
  Dropped(Dropped&&) = default;
  Dropped& operator=(Dropped&&) = default;
  KJ_DISALLOW_COPY(Dropped);

  uint32_t start;
  uint32_t end;

  void copyTo(rpc::Trace::Dropped::Builder builder) const;
  Dropped clone() const;
};

// ======================================================================================
// The base class for both the original legacy Trace (defined in trace-legacy.h)
// and the new StreamingTrace (defined in trace-streaming.h)
class TraceBase {
public:
  virtual void addException(trace::Exception&& exception) = 0;
  virtual void addDiagnosticChannelEvent(trace::DiagnosticChannelEvent&& event) = 0;
  virtual void addMark(kj::StringPtr mark) = 0;
  virtual void addMetrics(trace::Metrics&& metrics) = 0;
  virtual void addSubrequest(trace::Subrequest&& subrequest) = 0;
  virtual void addSubrequestOutcome(trace::SubrequestOutcome&& outcome) = 0;
  virtual void addCustom(Tags&& tags) = 0;
};

}  // namespace trace
}  // namespace workerd
