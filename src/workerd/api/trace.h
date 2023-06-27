// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

/**
 * Bindings for trace worker (ie. to support running wrangler tail).
 */

#include <workerd/api/basics.h>
#include <workerd/jsg/jsg.h>
#include <workerd/io/trace.h>
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

private:
  kj::Array<jsg::Ref<TraceItem>> events;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visitAll(events);
  }
};

class TraceItem final: public jsg::Object {
public:
  class FetchEventInfo;
  class ScheduledEventInfo;
  class AlarmEventInfo;
  class QueueEventInfo;
  class EmailEventInfo;
  class CustomEventInfo;

  explicit TraceItem(jsg::Lock& js, const Trace& trace);

  typedef kj::OneOf<jsg::Ref<FetchEventInfo>,
                    jsg::Ref<ScheduledEventInfo>,
                    jsg::Ref<AlarmEventInfo>,
                    jsg::Ref<QueueEventInfo>,
                    jsg::Ref<EmailEventInfo>,
                    jsg::Ref<CustomEventInfo>> EventInfo;
  kj::Maybe<EventInfo> getEvent(jsg::Lock& js);
  kj::Maybe<double> getEventTimestamp();

  kj::ArrayPtr<jsg::Ref<TraceLog>> getLogs();
  kj::ArrayPtr<jsg::Ref<TraceException>> getExceptions();
  kj::ArrayPtr<jsg::Ref<TraceDiagnosticChannelEvent>> getDiagnosticChannelEvents();
  kj::Maybe<kj::StringPtr> getScriptName();
  jsg::Optional<kj::StringPtr> getDispatchNamespace();
  jsg::Optional<kj::Array<kj::StringPtr>> getScriptTags();
  kj::StringPtr getOutcome();

  uint getCpuTime();
  uint getWallTime();

  JSG_RESOURCE_TYPE(TraceItem) {
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(event, getEvent);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(eventTimestamp, getEventTimestamp);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(logs, getLogs);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(exceptions, getExceptions);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(diagnosticsChannelEvents, getDiagnosticChannelEvents);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(scriptName, getScriptName);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(dispatchNamespace, getDispatchNamespace);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(scriptTags, getScriptTags);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(outcome, getOutcome);
  }

private:
  kj::Maybe<EventInfo> eventInfo;
  kj::Maybe<double> eventTimestamp;
  kj::Array<jsg::Ref<TraceLog>> logs;
  kj::Array<jsg::Ref<TraceException>> exceptions;
  kj::Array<jsg::Ref<TraceDiagnosticChannelEvent>> diagnosticChannelEvents;
  kj::Maybe<kj::String> scriptName;
  kj::Maybe<kj::String> dispatchNamespace;
  jsg::Optional<kj::Array<kj::String>> scriptTags;
  kj::String outcome;
  uint cpuTime;
  uint wallTime;
};

// When adding a new TraceItem eventInfo type, it is important not to
// try keeping a reference to the Trace and Trace::*EventInfo inputs.
// They are kj heap objects that have a lifespan that is managed independently
// of the TraceItem object. Each of the implementations here extract the
// necessary detail on creation and use LAZY instance properties to minimize
// copying and allocation necessary when accessing these values.
// TODO(cleanup): Later we can further optimize by creating the JS objects
// immediately on creation.

class TraceItem::FetchEventInfo final: public jsg::Object {
  // While this class is named FetchEventInfo, it encapsulates both the actual
  // FetchEventInfo as well as the FetchResponseInfo, which is an (optional)
  // sibling field (see worker.capnp). The internal FetchEventInfo (and
  // EventInfo in general) only represents the original event, not any
  // subsequent results such as the HTTP response. Internally, FetchEventInfo is
  // populated as soon as a request comes in, whereas the FetchResponseInfo is
  // only set once the request has finished entirely (along with the outcome,
  // see TraceItem::getOutcome).

public:
  class Request;
  class Response;

  explicit FetchEventInfo(jsg::Lock& js,
                          const Trace& trace,
                          const Trace::FetchEventInfo& eventInfo,
                          kj::Maybe<const Trace::FetchResponseInfo&> responseInfo);

  jsg::Ref<Request> getRequest();
  jsg::Optional<jsg::Ref<Response>> getResponse();

  // TODO(cleanup) Use struct types more?
  JSG_RESOURCE_TYPE(FetchEventInfo) {
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(response, getResponse);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(request, getRequest);
  }

private:
  jsg::Ref<Request> request;
  jsg::Optional<jsg::Ref<Response>> response;
};

class TraceItem::FetchEventInfo::Request final: public jsg::Object {
public:
  struct Detail : public kj::Refcounted {
    jsg::Optional<jsg::V8Ref<v8::Object>> cf;
    kj::Array<Trace::FetchEventInfo::Header> headers;
    kj::String method;
    kj::String url;

    Detail(jsg::Optional<jsg::V8Ref<v8::Object>> cf,
           kj::Array<Trace::FetchEventInfo::Header> headers,
           kj::String method,
           kj::String url);
  };

  explicit Request(jsg::Lock& js, const Trace& trace, const Trace::FetchEventInfo& eventInfo);

  explicit Request(Detail& detail, bool redacted = true);
  // Creates a possibly unredacted instance that shared a ref of the Detail

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

private:
  bool redacted = true;
  kj::Own<Detail> detail;
};

class TraceItem::FetchEventInfo::Response final: public jsg::Object {
public:
  explicit Response(const Trace& trace, const Trace::FetchResponseInfo& responseInfo);

  uint16_t getStatus();

  JSG_RESOURCE_TYPE(Response) {
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(status, getStatus);
  }

private:
  uint16_t status;
};

class TraceItem::ScheduledEventInfo final: public jsg::Object {
public:
  explicit ScheduledEventInfo(const Trace& trace, const Trace::ScheduledEventInfo& eventInfo);

  double getScheduledTime();
  kj::StringPtr getCron();

  JSG_RESOURCE_TYPE(ScheduledEventInfo) {
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(scheduledTime, getScheduledTime);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(cron, getCron);
  }

private:
  double scheduledTime;
  kj::String cron;
};

class TraceItem::AlarmEventInfo final: public jsg::Object {
public:
  explicit AlarmEventInfo(const Trace& trace, const Trace::AlarmEventInfo& eventInfo);

  kj::Date getScheduledTime();

  JSG_RESOURCE_TYPE(AlarmEventInfo) {
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(scheduledTime, getScheduledTime);
  }

private:
  kj::Date scheduledTime;
};

class TraceItem::QueueEventInfo final: public jsg::Object {
public:
  explicit QueueEventInfo(const Trace& trace, const Trace::QueueEventInfo& eventInfo);

  kj::StringPtr getQueueName();
  uint32_t getBatchSize();
  // TODO(now): Add something about the timestamp(s) of the newest/oldest message(s) in the batch?

  JSG_RESOURCE_TYPE(QueueEventInfo) {
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(queue, getQueueName);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(batchSize, getBatchSize);
  }

private:
  kj::String queueName;
  uint32_t batchSize;
};

class TraceItem::EmailEventInfo final: public jsg::Object {
public:
  explicit EmailEventInfo(const Trace& trace, const Trace::EmailEventInfo& eventInfo);

  kj::StringPtr getMailFrom();
  kj::StringPtr getRcptTo();
  uint32_t getRawSize();

  JSG_RESOURCE_TYPE(EmailEventInfo) {
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(mailFrom, getMailFrom);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(rcptTo, getRcptTo);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(rawSize, getRawSize);
  }

private:
  kj::String mailFrom;
  kj::String rcptTo;
  uint32_t rawSize;
};

class TraceItem::CustomEventInfo final: public jsg::Object {
public:
  explicit CustomEventInfo(const Trace& trace, const Trace::CustomEventInfo& eventInfo);

  JSG_RESOURCE_TYPE(CustomEventInfo) {}

private:
  const Trace::CustomEventInfo& eventInfo;
};

class TraceDiagnosticChannelEvent final: public jsg::Object {
public:
  explicit TraceDiagnosticChannelEvent(
      const Trace& trace,
      const Trace::DiagnosticChannelEvent& eventInfo);

  double getTimestamp();
  kj::StringPtr getChannel();
  v8::Local<v8::Value> getMessage(jsg::Lock& js);

  JSG_RESOURCE_TYPE(TraceDiagnosticChannelEvent) {
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(timestamp, getTimestamp);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(channel, getChannel);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(message, getMessage);
  }

private:
  double timestamp;
  kj::String channel;
  kj::Array<kj::byte> message;
};

class TraceLog final: public jsg::Object {
public:
  TraceLog(jsg::Lock& js, const Trace& trace, const Trace::Log& log);

  double getTimestamp();
  kj::StringPtr getLevel();
  jsg::V8Ref<v8::Object> getMessage(jsg::Lock& js);

  JSG_RESOURCE_TYPE(TraceLog) {
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(timestamp, getTimestamp);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(level, getLevel);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(message, getMessage);
  }

private:
  double timestamp;
  kj::String level;
  jsg::V8Ref<v8::Object> message;
};

class TraceException final: public jsg::Object {
public:
  TraceException(const Trace& trace, const Trace::Exception& exception);

  double getTimestamp();
  kj::StringPtr getName();
  kj::StringPtr getMessage();

  JSG_RESOURCE_TYPE(TraceException) {
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(timestamp, getTimestamp);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(message, getMessage);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(name, getName);
  }

private:
  double timestamp;
  kj::String name;
  kj::String message;
};

class TraceMetrics final : public jsg::Object {
public:
  explicit TraceMetrics(uint cpuTime, uint wallTime);

  uint getCPUTime() { return cpuTime; };
  uint getWallTime() { return wallTime; };

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
  TraceCustomEventImpl(
      uint16_t typeId, kj::TaskSet& waitUntilTasks, kj::Array<kj::Own<Trace>> traces)
    : typeId(typeId), waitUntilTasks(waitUntilTasks), traces(kj::mv(traces)) {}

  kj::Promise<Result> run(
      kj::Own<IoContext::IncomingRequest> incomingRequest,
      kj::Maybe<kj::StringPtr> entrypointName) override;

  kj::Promise<Result> sendRpc(
      capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
      capnp::ByteStreamFactory& byteStreamFactory,
      kj::TaskSet& waitUntilTasks,
      rpc::EventDispatcher::Client dispatcher) override;

  uint16_t getType() override {
    return typeId;
  }

private:
  uint16_t typeId;
  kj::TaskSet& waitUntilTasks;
  kj::Array<kj::Own<workerd::Trace>> traces;
};

#define EW_TRACE_ISOLATE_TYPES                \
  api::TailEvent,                             \
  api::TraceItem,                             \
  api::TraceItem::AlarmEventInfo,             \
  api::TraceItem::CustomEventInfo,            \
  api::TraceItem::ScheduledEventInfo,         \
  api::TraceItem::QueueEventInfo,             \
  api::TraceItem::EmailEventInfo,             \
  api::TraceItem::FetchEventInfo,             \
  api::TraceItem::FetchEventInfo::Request,    \
  api::TraceItem::FetchEventInfo::Response,   \
  api::TraceLog,                              \
  api::TraceException,                        \
  api::TraceDiagnosticChannelEvent,           \
  api::TraceMetrics,                          \
  api::UnsafeTraceMetrics
// The list of trace.h types that are added to worker.c++'s JSG_DECLARE_ISOLATE_TYPE

}  // namespace workerd::api
