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
      ExecutionModel executionModel);
  Onset(rpc::Trace::Onset::Reader reader);
  Onset(Onset&&) = default;
  Onset& operator=(Onset&&) = default;
  KJ_DISALLOW_COPY(Onset);

  kj::Maybe<kj::String> scriptName = kj::none;
  kj::Maybe<kj::Own<ScriptVersion::Reader>> scriptVersion = kj::none;
  kj::Maybe<kj::String> dispatchNamespace = kj::none;
  kj::Maybe<kj::String> scriptId = kj::none;
  kj::Array<kj::String> scriptTags = nullptr;
  kj::Maybe<kj::String> entrypoint = kj::none;
  ExecutionModel executionModel;
  kj::Maybe<EventInfo> info = kj::none;

  void copyTo(rpc::Trace::Onset::Builder builder) const;
  Onset clone() const;
};

// The SpanClose struct identifies the *close* of a span in the stream. A span is
// a logical grouping of events. Spans can be nested.
struct SpanClose final {
  // TODO(streaming-trace): Currently there's only one kind of info here but we likely
  // will add more in the future that would make it necessary to use a kj::OneOf.
  explicit SpanClose(EventOutcome outcome, kj::Maybe<FetchResponseInfo> maybeInfo);
  SpanClose(rpc::Trace::SpanClose::Reader reader);
  SpanClose(SpanClose&&) = default;
  SpanClose& operator=(SpanClose&&) = default;
  KJ_DISALLOW_COPY(SpanClose);

  EventOutcome outcome;
  kj::Maybe<FetchResponseInfo> info;

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
// as an alternative to plain text
// TODO(soon): Coallesce Log and LogV2
struct LogV2 final {
  explicit LogV2(LogLevel logLevel, kj::Array<kj::byte> message);
  LogV2(rpc::Trace::LogV2::Reader reader);
  LogV2(LogV2&&) = default;
  LogV2& operator=(LogV2&&) = default;
  KJ_DISALLOW_COPY(LogV2);

  LogLevel logLevel;
  kj::Array<kj::byte> message;

  void copyTo(rpc::Trace::LogV2::Builder builder) const;
  LogV2 clone() const;
};

// Describes metadata of an Exception that is added to the trace.
struct Exception final {
  explicit Exception(
      kj::Date timestamp, kj::String name, kj::String message, kj::Maybe<kj::String> stack);
  Exception(rpc::Trace::Exception::Reader reader);
  Exception(Exception&&) = default;
  Exception& operator=(Exception&&) = default;
  KJ_DISALLOW_COPY(Exception);

  // Only as accurate as Worker's Date.now(), for Spectre mitigation.
  kj::Date timestamp;

  kj::String name;
  kj::String message;
  kj::Maybe<kj::String> stack;

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

  void copyTo(rpc::Trace::Exception::Builder builder) const;
  Exception clone() const;
};

// Describes the start (and kind) of a subrequest initiated during the trace.
// For instance, a fetch request or a JS RPC call.
struct Subrequest final {
  using Info = kj::OneOf<FetchEventInfo, JsRpcEventInfo>;
  explicit Subrequest(kj::Maybe<Info> info = kj::none);
  Subrequest(rpc::Trace::Subrequest::Reader reader);
  Subrequest(Subrequest&&) = default;
  Subrequest& operator=(Subrequest&&) = default;
  KJ_DISALLOW_COPY(Subrequest);

  kj::Maybe<Info> info;

  void copyTo(rpc::Trace::Subrequest::Builder builder) const;
  Subrequest clone() const;
};

// A Metric is a key-value pair that can be emitted as part of the trace. Metrics
// include things like CPU time, Wall time, isolate heap memory usage, etc. The
// structure is left intentionally flexible to allow for a wide range of possible
// metrics to be emitted. This is a new feature in the streaming trace model.
struct Metric final {
  using Type = rpc::Trace::Metric::Type;

  explicit Metric(Type type, kj::String key, double value);
  explicit Metric(Type type, kj::StringPtr key, double value): Metric(type, kj::str(key), value) {}
  Metric(rpc::Trace::Metric::Reader reader);
  Metric(Metric&&) = default;
  Metric& operator=(Metric&&) = default;
  KJ_DISALLOW_COPY(Metric);

  Type type;
  kj::String key;
  double value;

  void copyTo(rpc::Trace::Metric::Builder builder) const;
  Metric clone() const;

  static inline Metric forWallTime(kj::Duration duration) {
    return Metric(Type::COUNTER, kj::str("wallTime"), duration / kj::MILLISECONDS);
  }

  static Metric forCpuTime(kj::Duration duration) {
    return Metric(Type::COUNTER, kj::str("cpuTime"), duration / kj::MILLISECONDS);
  }
};
using Metrics = kj::Array<Metric>;

}  // namespace trace
}  // namespace workerd
