// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

/**
 * Bindings for trace worker (ie. to support running wrangler tail).
 */

#include <workerd/api/basics.h>
#include <workerd/io/io-context.h>
#include <workerd/io/trace.h>
#include <workerd/jsg/jsg.h>

#include <kj/async.h>

namespace workerd::api {

class TraceItem;
class TraceException;
class TraceLog;
class TraceDiagnosticChannelEvent;

class TailEvent final: public ExtendableEvent {
 public:
  explicit TailEvent(jsg::Lock& js, kj::StringPtr type, kj::ArrayPtr<kj::Own<Trace>> events);

  static jsg::Ref<TailEvent> constructor(kj::String type) = delete;
  // TODO(soon): constructor?

  kj::Array<jsg::Ref<TraceItem>> getEvents();

  JSG_RESOURCE_TYPE(TailEvent) {
    JSG_INHERIT(ExtendableEvent);

    JSG_LAZY_READONLY_INSTANCE_PROPERTY(events, getEvents);
    // Deprecated. Please, use `events` instead.
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(traces, getEvents);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

 private:
  kj::Array<jsg::Ref<TraceItem>> events;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visitAll(events);
  }
};

struct ScriptVersion {
  explicit ScriptVersion(workerd::ScriptVersion::Reader version);
  ScriptVersion(const ScriptVersion&);

  jsg::Optional<kj::String> id;
  jsg::Optional<kj::String> tag;
  jsg::Optional<kj::String> message;

  JSG_STRUCT(id, tag, message);

  JSG_MEMORY_INFO(ScriptVersion) {
    tracker.trackField("id", id);
    tracker.trackField("tag", tag);
    tracker.trackField("message", message);
  }
};

class TraceItem final: public jsg::Object {
 public:
  class FetchEventInfo;
  class JsRpcEventInfo;
  class ScheduledEventInfo;
  class AlarmEventInfo;
  class QueueEventInfo;
  class EmailEventInfo;
  class TailEventInfo;
  class HibernatableWebSocketEventInfo;
  class CustomEventInfo;

  explicit TraceItem(jsg::Lock& js, const Trace& trace);

  typedef kj::OneOf<jsg::Ref<FetchEventInfo>,
      jsg::Ref<JsRpcEventInfo>,
      jsg::Ref<ScheduledEventInfo>,
      jsg::Ref<AlarmEventInfo>,
      jsg::Ref<QueueEventInfo>,
      jsg::Ref<EmailEventInfo>,
      jsg::Ref<TailEventInfo>,
      jsg::Ref<CustomEventInfo>,
      jsg::Ref<HibernatableWebSocketEventInfo>>
      EventInfo;
  kj::Maybe<EventInfo> getEvent(jsg::Lock& js);
  kj::Maybe<double> getEventTimestamp();

  kj::ArrayPtr<jsg::Ref<TraceLog>> getLogs();
  kj::ArrayPtr<jsg::Ref<TraceException>> getExceptions();
  kj::ArrayPtr<jsg::Ref<TraceDiagnosticChannelEvent>> getDiagnosticChannelEvents();
  kj::Maybe<kj::StringPtr> getScriptName();
  jsg::Optional<kj::StringPtr> getEntrypoint();
  jsg::Optional<ScriptVersion> getScriptVersion();
  jsg::Optional<kj::StringPtr> getDispatchNamespace();
  jsg::Optional<kj::Array<kj::StringPtr>> getScriptTags();
  kj::StringPtr getExecutionModel();
  kj::StringPtr getOutcome();

  uint getCpuTime();
  uint getWallTime();
  bool getTruncated();

  JSG_RESOURCE_TYPE(TraceItem) {
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(event, getEvent);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(eventTimestamp, getEventTimestamp);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(logs, getLogs);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(exceptions, getExceptions);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(diagnosticsChannelEvents, getDiagnosticChannelEvents);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(scriptName, getScriptName);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(entrypoint, getEntrypoint);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(scriptVersion, getScriptVersion);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(dispatchNamespace, getDispatchNamespace);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(scriptTags, getScriptTags);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(outcome, getOutcome);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(executionModel, getExecutionModel);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(truncated, getTruncated);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

 private:
  kj::Maybe<EventInfo> eventInfo;
  kj::Maybe<double> eventTimestamp;
  kj::Array<jsg::Ref<TraceLog>> logs;
  kj::Array<jsg::Ref<TraceException>> exceptions;
  kj::Array<jsg::Ref<TraceDiagnosticChannelEvent>> diagnosticChannelEvents;
  kj::Maybe<kj::String> scriptName;
  kj::Maybe<kj::String> entrypoint;
  kj::Maybe<ScriptVersion> scriptVersion;
  kj::Maybe<kj::String> dispatchNamespace;
  jsg::Optional<kj::Array<kj::String>> scriptTags;
  kj::String executionModel;
  kj::String outcome;
  uint cpuTime;
  uint wallTime;
  bool truncated;
};

// When adding a new TraceItem eventInfo type, it is important not to
// try keeping a reference to the Trace and tracing::*EventInfo inputs.
// They are kj heap objects that have a lifespan that is managed independently
// of the TraceItem object. Each of the implementations here extract the
// necessary detail on creation and use LAZY instance properties to minimize
// copying and allocation necessary when accessing these values.
// TODO(cleanup): Later we can further optimize by creating the JS objects
// immediately on creation.

// While this class is named FetchEventInfo, it encapsulates both the actual
// FetchEventInfo as well as the FetchResponseInfo, which is an (optional)
// sibling field (see worker.capnp). The internal FetchEventInfo (and
// EventInfo in general) only represents the original event, not any
// subsequent results such as the HTTP response. Internally, FetchEventInfo is
// populated as soon as a request comes in, whereas the FetchResponseInfo is
// only set once the request has finished entirely (along with the outcome,
// see TraceItem::getOutcome).
class TraceItem::FetchEventInfo final: public jsg::Object {
 public:
  class Request;
  class Response;

  explicit FetchEventInfo(jsg::Lock& js,
      const Trace& trace,
      const tracing::FetchEventInfo& eventInfo,
      kj::Maybe<const tracing::FetchResponseInfo&> responseInfo);

  jsg::Ref<Request> getRequest();
  jsg::Optional<jsg::Ref<Response>> getResponse();

  // TODO(cleanup) Use struct types more?
  JSG_RESOURCE_TYPE(FetchEventInfo) {
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(response, getResponse);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(request, getRequest);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

 private:
  jsg::Ref<Request> request;
  jsg::Optional<jsg::Ref<Response>> response;
};

class TraceItem::FetchEventInfo::Request final: public jsg::Object {
 public:
  struct Detail: public kj::Refcounted {
    jsg::Optional<jsg::V8Ref<v8::Object>> cf;
    kj::Array<tracing::FetchEventInfo::Header> headers;
    kj::String method;
    kj::String url;

    Detail(jsg::Optional<jsg::V8Ref<v8::Object>> cf,
        kj::Array<tracing::FetchEventInfo::Header> headers,
        kj::String method,
        kj::String url);

    JSG_MEMORY_INFO(Detail) {
      tracker.trackField("cf", cf);
      for (const auto& header: headers) {
        tracker.trackField(nullptr, header);
      }
      tracker.trackField("method", method);
      tracker.trackField("url", url);
    }
  };

  explicit Request(jsg::Lock& js, const Trace& trace, const tracing::FetchEventInfo& eventInfo);

  // Creates a possibly unredacted instance that shared a ref of the Detail
  explicit Request(Detail& detail, bool redacted = true);

  jsg::Optional<jsg::V8Ref<v8::Object>> getCf(jsg::Lock& js);
  jsg::Dict<jsg::ByteString, jsg::ByteString> getHeaders();
  kj::StringPtr getMethod();
  kj::String getUrl();

  jsg::Ref<Request> getUnredacted();

  JSG_RESOURCE_TYPE(Request) {
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(cf, getCf);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(headers, getHeaders);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(method, getMethod);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(url, getUrl);

    JSG_METHOD(getUnredacted);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("detail", detail);
  }

 private:
  bool redacted = true;
  kj::Own<Detail> detail;
};

class TraceItem::FetchEventInfo::Response final: public jsg::Object {
 public:
  explicit Response(const Trace& trace, const tracing::FetchResponseInfo& responseInfo);

  uint16_t getStatus();

  JSG_RESOURCE_TYPE(Response) {
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(status, getStatus);
  }

 private:
  uint16_t status;
};

class TraceItem::JsRpcEventInfo final: public jsg::Object {
 public:
  explicit JsRpcEventInfo(const Trace& trace, const tracing::JsRpcEventInfo& eventInfo);

  // We call this `rpcMethod` to make clear this is an RPC event, since some tail workers rely
  // on duck-typing EventInfo based on the properties present. (`methodName` might be ambiguous
  // since HTTP also has methods.)
  //
  // TODO(someday): Clearly there should be a better way to distinguish event types?
  kj::StringPtr getRpcMethod();

  JSG_RESOURCE_TYPE(JsRpcEventInfo) {
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(rpcMethod, getRpcMethod);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("rpcMethod", rpcMethod);
  }

 private:
  kj::String rpcMethod;
};

class TraceItem::ScheduledEventInfo final: public jsg::Object {
 public:
  explicit ScheduledEventInfo(const Trace& trace, const tracing::ScheduledEventInfo& eventInfo);

  double getScheduledTime();
  kj::StringPtr getCron();

  JSG_RESOURCE_TYPE(ScheduledEventInfo) {
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(scheduledTime, getScheduledTime);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(cron, getCron);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("cron", cron);
  }

 private:
  double scheduledTime;
  kj::String cron;
};

class TraceItem::AlarmEventInfo final: public jsg::Object {
 public:
  explicit AlarmEventInfo(const Trace& trace, const tracing::AlarmEventInfo& eventInfo);

  kj::Date getScheduledTime();

  JSG_RESOURCE_TYPE(AlarmEventInfo) {
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(scheduledTime, getScheduledTime);
  }

 private:
  kj::Date scheduledTime;
};

class TraceItem::QueueEventInfo final: public jsg::Object {
 public:
  explicit QueueEventInfo(const Trace& trace, const tracing::QueueEventInfo& eventInfo);

  kj::StringPtr getQueueName();
  uint32_t getBatchSize();
  // TODO(now): Add something about the timestamp(s) of the newest/oldest message(s) in the batch?

  JSG_RESOURCE_TYPE(QueueEventInfo) {
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(queue, getQueueName);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(batchSize, getBatchSize);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("queueName", queueName);
  }

 private:
  kj::String queueName;
  uint32_t batchSize;
};

class TraceItem::EmailEventInfo final: public jsg::Object {
 public:
  explicit EmailEventInfo(const Trace& trace, const tracing::EmailEventInfo& eventInfo);

  kj::StringPtr getMailFrom();
  kj::StringPtr getRcptTo();
  uint32_t getRawSize();

  JSG_RESOURCE_TYPE(EmailEventInfo) {
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(mailFrom, getMailFrom);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(rcptTo, getRcptTo);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(rawSize, getRawSize);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("mailFrom", mailFrom);
    tracker.trackField("rcptTo", rcptTo);
  }

 private:
  kj::String mailFrom;
  kj::String rcptTo;
  uint32_t rawSize;
};

class TraceItem::TailEventInfo final: public jsg::Object {
 public:
  class TailItem;

  explicit TailEventInfo(const Trace& trace, const tracing::TraceEventInfo& eventInfo);

  kj::Array<jsg::Ref<TailItem>> getConsumedEvents();

  JSG_RESOURCE_TYPE(TailEventInfo) {
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(consumedEvents, getConsumedEvents);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

 private:
  kj::Array<jsg::Ref<TailItem>> consumedEvents;
};

class TraceItem::TailEventInfo::TailItem final: public jsg::Object {
 public:
  explicit TailItem(const tracing::TraceEventInfo::TraceItem& traceItem);

  kj::Maybe<kj::StringPtr> getScriptName();

  JSG_RESOURCE_TYPE(TailItem) {
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(scriptName, getScriptName);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("scriptName", scriptName);
  }

 private:
  kj::Maybe<kj::String> scriptName;
};

class TraceItem::HibernatableWebSocketEventInfo final: public jsg::Object {
 public:
  class Message;
  class Close;
  class Error;

  explicit HibernatableWebSocketEventInfo(
      const Trace& trace, const tracing::HibernatableWebSocketEventInfo::Message& eventInfo);
  explicit HibernatableWebSocketEventInfo(
      const Trace& trace, const tracing::HibernatableWebSocketEventInfo::Close& eventInfo);
  explicit HibernatableWebSocketEventInfo(
      const Trace& trace, const tracing::HibernatableWebSocketEventInfo::Error& eventInfo);

  using Type = kj::OneOf<jsg::Ref<Message>, jsg::Ref<Close>, jsg::Ref<Error>>;

  Type getEvent();

  JSG_RESOURCE_TYPE(HibernatableWebSocketEventInfo) {
    JSG_READONLY_INSTANCE_PROPERTY(getWebSocketEvent, getEvent);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

 private:
  Type eventType;
};

class TraceItem::HibernatableWebSocketEventInfo::Message final: public jsg::Object {
 public:
  explicit Message(
      const Trace& trace, const tracing::HibernatableWebSocketEventInfo::Message& eventInfo)
      : eventInfo(eventInfo) {}

  static constexpr kj::StringPtr webSocketEventType = "message"_kj;
  kj::StringPtr getWebSocketEventType() {
    return webSocketEventType;
  }

  JSG_RESOURCE_TYPE(Message) {
    JSG_READONLY_INSTANCE_PROPERTY(webSocketEventType, getWebSocketEventType);
  }

 private:
  const tracing::HibernatableWebSocketEventInfo::Message& eventInfo;
};

class TraceItem::HibernatableWebSocketEventInfo::Close final: public jsg::Object {
 public:
  explicit Close(
      const Trace& trace, const tracing::HibernatableWebSocketEventInfo::Close& eventInfo)
      : eventInfo(eventInfo) {}

  static constexpr kj::StringPtr webSocketEventType = "close"_kj;
  kj::StringPtr getWebSocketEventType() {
    return webSocketEventType;
  }

  uint16_t getCode();
  bool getWasClean();

  JSG_RESOURCE_TYPE(Close) {
    JSG_READONLY_INSTANCE_PROPERTY(webSocketEventType, getWebSocketEventType);
    JSG_READONLY_INSTANCE_PROPERTY(code, getCode);
    JSG_READONLY_INSTANCE_PROPERTY(wasClean, getWasClean);
  }

 private:
  const tracing::HibernatableWebSocketEventInfo::Close& eventInfo;
};

class TraceItem::HibernatableWebSocketEventInfo::Error final: public jsg::Object {
 public:
  explicit Error(
      const Trace& trace, const tracing::HibernatableWebSocketEventInfo::Error& eventInfo)
      : eventInfo(eventInfo) {}

  static constexpr kj::StringPtr webSocketEventType = "error"_kj;
  kj::StringPtr getWebSocketEventType() {
    return webSocketEventType;
  }

  JSG_RESOURCE_TYPE(Error) {
    JSG_READONLY_INSTANCE_PROPERTY(webSocketEventType, getWebSocketEventType);
  }

 private:
  const tracing::HibernatableWebSocketEventInfo::Error& eventInfo;
};

class TraceItem::CustomEventInfo final: public jsg::Object {
 public:
  explicit CustomEventInfo(const Trace& trace, const tracing::CustomEventInfo& eventInfo);

  JSG_RESOURCE_TYPE(CustomEventInfo) {}

 private:
  const tracing::CustomEventInfo& eventInfo;
};

class TraceDiagnosticChannelEvent final: public jsg::Object {
 public:
  explicit TraceDiagnosticChannelEvent(
      const Trace& trace, const tracing::DiagnosticChannelEvent& eventInfo);

  double getTimestamp();
  kj::StringPtr getChannel();
  jsg::JsValue getMessage(jsg::Lock& js);

  JSG_RESOURCE_TYPE(TraceDiagnosticChannelEvent) {
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(timestamp, getTimestamp);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(channel, getChannel);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(message, getMessage);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("channel", channel);
    tracker.trackFieldWithSize("message", message.size());
  }

 private:
  double timestamp;
  kj::String channel;
  kj::Array<kj::byte> message;
};

class TraceLog final: public jsg::Object {
 public:
  TraceLog(jsg::Lock& js, const Trace& trace, const tracing::Log& log);

  double getTimestamp();
  kj::StringPtr getLevel();
  jsg::V8Ref<v8::Object> getMessage(jsg::Lock& js);

  JSG_RESOURCE_TYPE(TraceLog) {
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(timestamp, getTimestamp);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(level, getLevel);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(message, getMessage);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("level", level);
    tracker.trackField("message", message);
  }

 private:
  double timestamp;
  kj::String level;
  jsg::V8Ref<v8::Object> message;
};

class TraceException final: public jsg::Object {
 public:
  TraceException(const Trace& trace, const tracing::Exception& exception);

  double getTimestamp();
  kj::StringPtr getName();
  kj::StringPtr getMessage();
  jsg::Optional<kj::StringPtr> getStack(jsg::Lock& js);

  JSG_RESOURCE_TYPE(TraceException) {
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(timestamp, getTimestamp);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(message, getMessage);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(name, getName);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(stack, getStack);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("name", name);
    tracker.trackField("message", message);
  }

 private:
  double timestamp;
  kj::String name;
  kj::String message;
  kj::Maybe<kj::String> stack;
};

class TraceMetrics final: public jsg::Object {
 public:
  explicit TraceMetrics(uint cpuTime, uint wallTime);

  uint getCPUTime() {
    return cpuTime;
  };
  uint getWallTime() {
    return wallTime;
  };

  JSG_RESOURCE_TYPE(TraceMetrics) {
    JSG_READONLY_INSTANCE_PROPERTY(cpuTime, getCPUTime);
    JSG_READONLY_INSTANCE_PROPERTY(wallTime, getWallTime);

    JSG_TS_ROOT();
  }

 private:
  uint cpuTime;
  uint wallTime;
};

class UnsafeTraceMetrics final: public jsg::Object {
 public:
  jsg::Ref<TraceMetrics> fromTrace(jsg::Ref<TraceItem> item);

  JSG_RESOURCE_TYPE(UnsafeTraceMetrics) {
    JSG_METHOD(fromTrace);

    JSG_TS_ROOT();
  }
};

class TraceCustomEventImpl final: public WorkerInterface::CustomEvent {
 public:
  TraceCustomEventImpl(uint16_t typeId, kj::Array<kj::Own<Trace>> traces)
      : typeId(typeId),
        traces(kj::mv(traces)) {}

  kj::Promise<Result> run(kj::Own<IoContext::IncomingRequest> incomingRequest,
      kj::Maybe<kj::StringPtr> entrypointName,
      Frankenvalue props,
      kj::TaskSet& waitUntilTasks) override;

  kj::Promise<Result> sendRpc(capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
      capnp::ByteStreamFactory& byteStreamFactory,
      rpc::EventDispatcher::Client dispatcher) override;

  uint16_t getType() override {
    return typeId;
  }

  kj::Promise<Result> notSupported() override {
    KJ_UNIMPLEMENTED("trace event not supported");
  }

  static constexpr uint16_t TYPE = 2;

 private:
  uint16_t typeId;
  kj::Array<kj::Own<workerd::Trace>> traces;
};

#define EW_TRACE_ISOLATE_TYPES                                                                     \
  api::ScriptVersion, api::TailEvent, api::TraceItem, api::TraceItem::AlarmEventInfo,              \
      api::TraceItem::CustomEventInfo, api::TraceItem::ScheduledEventInfo,                         \
      api::TraceItem::QueueEventInfo, api::TraceItem::EmailEventInfo,                              \
      api::TraceItem::TailEventInfo, api::TraceItem::TailEventInfo::TailItem,                      \
      api::TraceItem::FetchEventInfo, api::TraceItem::FetchEventInfo::Request,                     \
      api::TraceItem::FetchEventInfo::Response, api::TraceItem::JsRpcEventInfo,                    \
      api::TraceItem::HibernatableWebSocketEventInfo,                                              \
      api::TraceItem::HibernatableWebSocketEventInfo::Message,                                     \
      api::TraceItem::HibernatableWebSocketEventInfo::Close,                                       \
      api::TraceItem::HibernatableWebSocketEventInfo::Error, api::TraceLog, api::TraceException,   \
      api::TraceDiagnosticChannelEvent, api::TraceMetrics, api::UnsafeTraceMetrics
// The list of trace.h types that are added to worker.c++'s JSG_DECLARE_ISOLATE_TYPE

}  // namespace workerd::api
