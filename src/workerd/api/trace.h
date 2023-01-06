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

class TraceEvent final: public ExtendableEvent {
public:
  explicit TraceEvent(kj::ArrayPtr<kj::Own<Trace>> traces);

  static jsg::Ref<TraceEvent> constructor(kj::String type) = delete;
  // TODO(soon): constructor?

  // TODO(perf): more efficient to build/return cached array object?  Or iterator?
  kj::Array<jsg::Ref<TraceItem>> getTraces();

  JSG_RESOURCE_TYPE(TraceEvent) {
    JSG_INHERIT(ExtendableEvent);

    JSG_READONLY_INSTANCE_PROPERTY(traces, getTraces);
  }

private:
  kj::Array<jsg::Ref<TraceItem>> traces;

  void visitForGc(jsg::GcVisitor& visitor) {
    for (auto& t: traces) {
      visitor.visit(t);
    }
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

  explicit TraceItem(kj::Own<Trace> trace);

  typedef kj::OneOf<jsg::Ref<FetchEventInfo>, jsg::Ref<ScheduledEventInfo>,
      jsg::Ref<AlarmEventInfo>, jsg::Ref<QueueEventInfo>,
      jsg::Ref<EmailEventInfo>, jsg::Ref<CustomEventInfo>> EventInfo;
  kj::Maybe<EventInfo> getEvent();
  // TODO(someday): support more event types (trace, email) via kj::OneOf.
  kj::Maybe<double> getEventTimestamp();

  kj::Array<jsg::Ref<TraceLog>> getLogs();
  kj::Array<jsg::Ref<TraceException>> getExceptions();
  kj::Maybe<kj::StringPtr> getScriptName();
  jsg::Optional<kj::StringPtr> getDispatchNamespace();
  jsg::Optional<kj::Array<kj::StringPtr>> getScriptTags();
  kj::StringPtr getOutcome();

  uint getCpuTime();
  uint getWallTime();

  JSG_RESOURCE_TYPE(TraceItem) {
    JSG_READONLY_INSTANCE_PROPERTY(event, getEvent);
    JSG_READONLY_INSTANCE_PROPERTY(eventTimestamp, getEventTimestamp);
    JSG_READONLY_INSTANCE_PROPERTY(logs, getLogs);
    JSG_READONLY_INSTANCE_PROPERTY(exceptions, getExceptions);
    JSG_READONLY_INSTANCE_PROPERTY(scriptName, getScriptName);
    JSG_READONLY_INSTANCE_PROPERTY(dispatchNamespace, getDispatchNamespace);
    JSG_READONLY_INSTANCE_PROPERTY(scriptTags, getScriptTags);
    JSG_READONLY_INSTANCE_PROPERTY(outcome, getOutcome);
  }

private:
  kj::Own<Trace> trace;
};

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

  explicit FetchEventInfo(kj::Own<Trace> trace, const Trace::FetchEventInfo& eventInfo,
                          kj::Maybe<const Trace::FetchResponseInfo&> responseInfo);

  jsg::Ref<Request> getRequest();
  jsg::Optional<jsg::Ref<Response>> getResponse();

  // TODO(cleanup) Use struct types more?
  JSG_RESOURCE_TYPE(FetchEventInfo) {
    JSG_READONLY_INSTANCE_PROPERTY(response, getResponse);
    JSG_READONLY_INSTANCE_PROPERTY(request, getRequest);
  }

private:
  kj::Own<Trace> trace;
  const Trace::FetchEventInfo& eventInfo;
  kj::Maybe<const Trace::FetchResponseInfo&> responseInfo;
};

class TraceItem::FetchEventInfo::Request final: public jsg::Object {
public:
  explicit Request(kj::Own<Trace> trace, const Trace::FetchEventInfo& eventInfo);

  jsg::Optional<v8::Local<v8::Object>> getCf(v8::Isolate* isolate);
  jsg::Dict<jsg::ByteString, jsg::ByteString> getHeaders();
  kj::StringPtr getMethod();
  kj::String getUrl();

  jsg::Ref<Request> getUnredacted();

  JSG_RESOURCE_TYPE(Request) {
    JSG_READONLY_INSTANCE_PROPERTY(cf, getCf);
    JSG_READONLY_INSTANCE_PROPERTY(headers, getHeaders);
    JSG_READONLY_INSTANCE_PROPERTY(method, getMethod);
    JSG_READONLY_INSTANCE_PROPERTY(url, getUrl);

    JSG_METHOD(getUnredacted);
  }

private:
  kj::Own<Trace> trace;
  const Trace::FetchEventInfo& eventInfo;
  bool redacted = true;
};

class TraceItem::FetchEventInfo::Response final: public jsg::Object {
public:
  explicit Response(kj::Own<Trace> trace, const Trace::FetchResponseInfo& responseInfo);

  uint16_t getStatus();

  JSG_RESOURCE_TYPE(Response) {
    JSG_READONLY_INSTANCE_PROPERTY(status, getStatus);
  }

private:
  kj::Own<Trace> trace;
  const Trace::FetchResponseInfo& responseInfo;
};

class TraceItem::ScheduledEventInfo final: public jsg::Object {
public:
  explicit ScheduledEventInfo(kj::Own<Trace> trace, const Trace::ScheduledEventInfo& eventInfo);

  double getScheduledTime();
  kj::StringPtr getCron();

  JSG_RESOURCE_TYPE(ScheduledEventInfo) {
    JSG_READONLY_INSTANCE_PROPERTY(scheduledTime, getScheduledTime);
    JSG_READONLY_INSTANCE_PROPERTY(cron, getCron);
  }

private:
  kj::Own<Trace> trace;
  const Trace::ScheduledEventInfo& eventInfo;
};

class TraceItem::AlarmEventInfo final: public jsg::Object {
public:
  explicit AlarmEventInfo(kj::Own<Trace> trace, const Trace::AlarmEventInfo& eventInfo);

  kj::Date getScheduledTime();

  JSG_RESOURCE_TYPE(AlarmEventInfo) {
    JSG_READONLY_INSTANCE_PROPERTY(scheduledTime, getScheduledTime);
  }

private:
  kj::Own<Trace> trace;
  const Trace::AlarmEventInfo& eventInfo;
};

class TraceItem::QueueEventInfo final: public jsg::Object {
public:
  explicit QueueEventInfo(kj::Own<Trace> trace, const Trace::QueueEventInfo& eventInfo);

  kj::StringPtr getQueueName();
  uint32_t getBatchSize();
  // TODO(now): Add something about the timestamp(s) of the newest/oldest message(s) in the batch?

  JSG_RESOURCE_TYPE(QueueEventInfo) {
    JSG_READONLY_INSTANCE_PROPERTY(queue, getQueueName);
    JSG_READONLY_INSTANCE_PROPERTY(batchSize, getBatchSize);
  }

private:
  kj::Own<Trace> trace;
  const Trace::QueueEventInfo& eventInfo;
};

class TraceItem::EmailEventInfo final: public jsg::Object {
public:
  explicit EmailEventInfo(kj::Own<Trace> trace, const Trace::EmailEventInfo& eventInfo);

  kj::StringPtr getMailFrom();
  kj::StringPtr getRcptTo();
  uint32_t getRawSize();

  JSG_RESOURCE_TYPE(EmailEventInfo) {
    JSG_READONLY_PROTOTYPE_PROPERTY(mailFrom, getMailFrom);
    JSG_READONLY_PROTOTYPE_PROPERTY(rcptTo, getRcptTo);
    JSG_READONLY_PROTOTYPE_PROPERTY(rawSize, getRawSize);
  }

private:
  kj::Own<Trace> trace;
  const Trace::EmailEventInfo& eventInfo;
};

class TraceItem::CustomEventInfo final: public jsg::Object {
public:
  explicit CustomEventInfo(kj::Own<Trace> trace, const Trace::CustomEventInfo& eventInfo);

  JSG_RESOURCE_TYPE(CustomEventInfo) {}

private:
  kj::Own<Trace> trace;
  const Trace::CustomEventInfo& eventInfo;
};

class TraceLog final: public jsg::Object {
public:
  TraceLog(kj::Own<Trace> trace, const Trace::Log& log);

  double getTimestamp();
  kj::StringPtr getLevel();
  v8::Local<v8::Object> getMessage(v8::Isolate* isolate);

  JSG_RESOURCE_TYPE(TraceLog) {
    JSG_READONLY_INSTANCE_PROPERTY(timestamp, getTimestamp);
    JSG_READONLY_INSTANCE_PROPERTY(level, getLevel);
    JSG_READONLY_INSTANCE_PROPERTY(message, getMessage);
  }

private:
  kj::Own<Trace> trace;
  const Trace::Log& log;
};

class TraceException final: public jsg::Object {
public:
  TraceException(kj::Own<Trace> trace, const Trace::Exception& exception);

  double getTimestamp();
  kj::StringPtr getName();
  kj::StringPtr getMessage();

  JSG_RESOURCE_TYPE(TraceException) {
    JSG_READONLY_INSTANCE_PROPERTY(timestamp, getTimestamp);
    JSG_READONLY_INSTANCE_PROPERTY(message, getMessage);
    JSG_READONLY_INSTANCE_PROPERTY(name, getName);
  }

private:
  kj::Own<Trace> trace;
  const Trace::Exception& exception;
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
  jsg::Ref<TraceMetrics> fromTrace(jsg::Ref<TraceItem>);

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
  api::TraceEvent,                            \
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
  api::TraceMetrics,                          \
  api::UnsafeTraceMetrics
// The list of trace.h types that are added to worker.c++'s JSG_DECLARE_ISOLATE_TYPE

}  // namespace workerd::api
