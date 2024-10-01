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

// A Trace Tag is a key-value pair that can be attached to multiple types of objects
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
struct Tag {
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

// Metadata describing the onset of a trace session.
struct Onset final {
  explicit Onset() = default;
  Onset(kj::Maybe<uint32_t> accountId,
      kj::Maybe<kj::String> stableId,
      kj::Maybe<kj::String> scriptName,
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

  kj::Maybe<uint32_t> accountId = kj::none;
  kj::Maybe<kj::String> stableId = kj::none;
  kj::Maybe<kj::String> scriptName = kj::none;
  kj::Maybe<kj::Own<ScriptVersion::Reader>> scriptVersion = kj::none;
  kj::Maybe<kj::String> dispatchNamespace = kj::none;
  kj::Maybe<kj::String> scriptId = kj::none;
  kj::Array<kj::String> scriptTags = nullptr;
  kj::Maybe<kj::String> entrypoint = kj::none;
  ExecutionModel executionModel;
  Tags tags = nullptr;

  void copyTo(rpc::Trace::Onset::Builder builder) const;
  Onset clone() const;
};

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

struct ActorFlushInfo final {
  explicit ActorFlushInfo(Tags tags = nullptr);
  ActorFlushInfo(rpc::Trace::ActorFlushInfo::Reader reader);
  ActorFlushInfo(ActorFlushInfo&&) = default;
  ActorFlushInfo& operator=(ActorFlushInfo&&) = default;
  KJ_DISALLOW_COPY(ActorFlushInfo);

  using CommonTags = rpc::Trace::ActorFlushInfo::Common;

  Tags tags;

  void copyTo(rpc::Trace::ActorFlushInfo::Builder builder) const;
  ActorFlushInfo clone() const;
};

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

struct CustomEventInfo {
  explicit CustomEventInfo() {};
  CustomEventInfo(rpc::Trace::CustomEventInfo::Reader reader) {};
  CustomEventInfo(CustomEventInfo&&) = default;
  CustomEventInfo& operator=(CustomEventInfo&&) = default;
  KJ_DISALLOW_COPY(CustomEventInfo);

  CustomEventInfo clone() const {
    return CustomEventInfo();
  }
};

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

using EventInfo = kj::OneOf<FetchEventInfo,
    JsRpcEventInfo,
    ScheduledEventInfo,
    AlarmEventInfo,
    QueueEventInfo,
    EmailEventInfo,
    TraceEventInfo,
    HibernatableWebSocketEventInfo,
    CustomEventInfo>;

// Used to describe the final outcome of the trace.
struct Outcome final {
  Outcome() = default;
  explicit Outcome(EventOutcome outcome, kj::Duration cpuTime, kj::Duration wallTime);
  Outcome(rpc::Trace::Outcome::Reader reader);
  Outcome(Outcome&&) = default;
  Outcome& operator=(Outcome&&) = default;
  KJ_DISALLOW_COPY(Outcome);

  EventOutcome outcome = EventOutcome::UNKNOWN;
  kj::Duration cpuTime;
  kj::Duration wallTime;

  void copyTo(rpc::Trace::Outcome::Builder builder) const;
  Outcome clone() const;
};

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

struct LogV2 final {
  explicit LogV2(
      kj::Date timestamp, LogLevel logLevel, kj::Array<kj::byte> data, Tags tags = nullptr);
  LogV2(rpc::Trace::LogV2::Reader reader);
  LogV2(LogV2&&) = default;
  LogV2& operator=(LogV2&&) = default;
  KJ_DISALLOW_COPY(LogV2);

  // Only as accurate as Worker's Date.now(), for Spectre mitigation.
  kj::Date timestamp;

  LogLevel logLevel;
  kj::Array<kj::byte> data;
  Tags tags = nullptr;

  void copyTo(rpc::Trace::LogV2::Builder builder) const;
  LogV2 clone() const;
};

struct Exception final {
  struct Detail {
    kj::Maybe<kj::Own<Exception>> cause = kj::none;
    kj::Array<kj::Own<Exception>> errors = nullptr;
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

struct SubrequestOutcome final {
  using Info = kj::OneOf<FetchResponseInfo, Tags>;
  explicit SubrequestOutcome(
      uint32_t id, kj::Maybe<Info> info, rpc::Trace::Span::SpanOutcome outcome);
  SubrequestOutcome(rpc::Trace::SubrequestOutcome::Reader reader);
  SubrequestOutcome(SubrequestOutcome&&) = default;
  SubrequestOutcome& operator=(SubrequestOutcome&&) = default;
  KJ_DISALLOW_COPY(SubrequestOutcome);

  uint32_t id;
  kj::Maybe<Info> info;
  rpc::Trace::Span::SpanOutcome outcome;

  void copyTo(rpc::Trace::SubrequestOutcome::Builder builder) const;
  SubrequestOutcome clone() const;
};

struct SpanEvent final {
  using Outcome = rpc::Trace::Span::SpanOutcome;
  using Info = kj::OneOf<FetchResponseInfo, ActorFlushInfo, Tags>;
  explicit SpanEvent(uint32_t id,
      uint32_t parentId,
      rpc::Trace::Span::SpanOutcome outcome,
      bool transactional = false,
      kj::Maybe<Info> maybeInfo = kj::none,
      Tags tags = nullptr);
  SpanEvent(rpc::Trace::Span::Reader reader);
  SpanEvent(SpanEvent&&) = default;
  SpanEvent& operator=(SpanEvent&&) = default;
  KJ_DISALLOW_COPY(SpanEvent);

  uint32_t id;
  uint32_t parent;
  rpc::Trace::Span::SpanOutcome outcome;
  bool transactional;
  kj::Maybe<Info> info;
  Tags tags;

  void copyTo(rpc::Trace::Span::Builder builder) const;
  SpanEvent clone() const;
};

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

struct Metric final {
  using Key = kj::OneOf<kj::String, uint32_t>;
  using Value = kj::OneOf<double, int64_t, uint64_t>;
  using Type = rpc::Trace::Metric::Type;

  explicit Metric(Type type, Key key, Value value);

  template <IsEnum K>
  explicit Metric(Type type, K key, Value value)
      : Metric(type, static_cast<uint32_t>(key), kj::mv(value)) {}

  Metric(rpc::Trace::Metric::Reader reader);
  Metric(Metric&&) = default;
  Metric& operator=(Metric&&) = default;
  KJ_DISALLOW_COPY(Metric);

  Type type;
  Key key;
  Value value;

  bool keyMatches(kj::OneOf<kj::StringPtr, uint32_t> key);

  template <IsEnum Key>
  inline bool keyMatches(Key key) {
    return keyMatches(static_cast<uint32_t>(key));
  }

  void copyTo(rpc::Trace::Metric::Builder builder) const;
  Metric clone() const;
};
using Metrics = kj::Array<Metric>;

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

using EventDetail = kj::OneOf<LogV2,
    Exception,
    DiagnosticChannelEvent,
    Mark,
    Metrics,
    Subrequest,
    SubrequestOutcome,
    Tags>;

// ======================================================================================
// Represents a trace span. `Span` objects are delivered to `SpanObserver`s for recording. To
// create a `Span`, use a `SpanBuilder`. (Used in the legacy trace api)
struct Span {
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

// ======================================================================================
// The base class for both the original legacy Trace (defined in trace-legacy.h)
// and the new StreamingTrace (defined in trace-streaming.h)
class TraceBase {
public:
  virtual void addException(trace::Exception&& exception) = 0;
  virtual void addDiagnosticChannelEvent(trace::DiagnosticChannelEvent&& event) = 0;
  virtual void addMark(trace::Mark&& mark) = 0;
  virtual void addMetrics(trace::Metrics&& metrics) = 0;
  virtual void addSubrequest(trace::Subrequest&& subrequest) = 0;
  virtual void addSubrequestOutcome(trace::SubrequestOutcome&& outcome) = 0;
  virtual void addCustom(Tags&& tags) = 0;
};

}  // namespace trace
}  // namespace workerd
