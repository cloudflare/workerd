#pragma once

#include <workerd/io/outcome.capnp.h>
#include <workerd/io/worker-interface.capnp.h>
#include <workerd/jsg/memory.h>

#include <kj/one-of.h>
#include <kj/string.h>
#include <kj/time.h>

namespace kj {
enum class HttpMethod;
class EntropySource;
}  // namespace kj

namespace workerd {

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

// Metadata describing the onset of a trace session.
struct OnsetInfo final {
  kj::Maybe<kj::String> ownerId = kj::none;
  kj::Maybe<kj::String> stableId = kj::none;
  kj::Maybe<kj::String> scriptName = kj::none;
  kj::Maybe<kj::Own<ScriptVersion::Reader>> scriptVersion = kj::none;
  kj::Maybe<kj::String> dispatchNamespace = kj::none;
  kj::Maybe<kj::String> scriptId = kj::none;
  kj::Array<kj::String> scriptTags = nullptr;
  kj::Maybe<kj::String> entrypoint = kj::none;
  ExecutionModel ExecutionModel;
};

struct OutcomeInfo final {
  EventOutcome outcome = EventOutcome::UNKNOWN;
  kj::Duration cpuTime;
  kj::Duration wallTime;
};

// Metadata describing the start of a received fetch request.
struct FetchEventInfo final {
  struct Header;

  explicit FetchEventInfo(
      kj::HttpMethod method, kj::String url, kj::String cfJson, kj::Array<Header> headers);
  FetchEventInfo(rpc::Trace::FetchEventInfo::Reader reader);

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
};

struct FetchResponseInfo final {
  explicit FetchResponseInfo(uint16_t statusCode);
  FetchResponseInfo(rpc::Trace::FetchResponseInfo::Reader reader);

  uint16_t statusCode;

  void copyTo(rpc::Trace::FetchResponseInfo::Builder builder);
};

struct JsRpcEventInfo final {
  explicit JsRpcEventInfo(kj::String methodName);
  JsRpcEventInfo(rpc::Trace::JsRpcEventInfo::Reader reader);

  kj::String methodName;

  void copyTo(rpc::Trace::JsRpcEventInfo::Builder builder);
};

struct ScheduledEventInfo final {
  explicit ScheduledEventInfo(double scheduledTime, kj::String cron);
  ScheduledEventInfo(rpc::Trace::ScheduledEventInfo::Reader reader);

  double scheduledTime;
  kj::String cron;

  void copyTo(rpc::Trace::ScheduledEventInfo::Builder builder);
};

struct AlarmEventInfo final {
  explicit AlarmEventInfo(kj::Date scheduledTime);
  AlarmEventInfo(rpc::Trace::AlarmEventInfo::Reader reader);

  kj::Date scheduledTime;

  void copyTo(rpc::Trace::AlarmEventInfo::Builder builder);
};

struct QueueEventInfo final {
  explicit QueueEventInfo(kj::String queueName, uint32_t batchSize);
  QueueEventInfo(rpc::Trace::QueueEventInfo::Reader reader);

  kj::String queueName;
  uint32_t batchSize;

  void copyTo(rpc::Trace::QueueEventInfo::Builder builder);
};

struct EmailEventInfo final {
  explicit EmailEventInfo(kj::String mailFrom, kj::String rcptTo, uint32_t rawSize);
  EmailEventInfo(rpc::Trace::EmailEventInfo::Reader reader);

  kj::String mailFrom;
  kj::String rcptTo;
  uint32_t rawSize;

  void copyTo(rpc::Trace::EmailEventInfo::Builder builder);
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

  Type type;

  void copyTo(rpc::Trace::HibernatableWebSocketEventInfo::Builder builder);
  static Type readFrom(rpc::Trace::HibernatableWebSocketEventInfo::Reader reader);
};

struct CustomEventInfo {
  explicit CustomEventInfo() {};
  CustomEventInfo(rpc::Trace::CustomEventInfo::Reader reader) {};
};

struct TraceEventInfo final {
  struct TraceItem;

  explicit TraceEventInfo(kj::ArrayPtr<kj::Own<Trace>> traces);
  TraceEventInfo(rpc::Trace::TraceEventInfo::Reader reader);

  struct TraceItem {
    explicit TraceItem(kj::Maybe<kj::String> scriptName);
    TraceItem(rpc::Trace::TraceEventInfo::TraceItem::Reader reader);

    kj::Maybe<kj::String> scriptName;

    void copyTo(rpc::Trace::TraceEventInfo::TraceItem::Builder builder);
  };

  kj::Vector<TraceItem> traces;

  void copyTo(rpc::Trace::TraceEventInfo::Builder builder);
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
};

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
};

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
};

}  // namespace trace
}  // namespace workerd
