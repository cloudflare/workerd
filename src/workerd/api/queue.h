// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/async.h>
#include <kj/common.h>

#include <workerd/api/basics.h>
#include <workerd/io/worker-interface.capnp.h>
#include <workerd/io/worker-interface.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api {

class ExecutionContext;

// Binding types

// A capability to a Worker Queue.
class WorkerQueue: public jsg::Object {
public:
  // `subrequestChannel` is what to pass to IoContext::getHttpClient() to get an HttpClient
  // representing this queue.
  WorkerQueue(uint subrequestChannel)
    : subrequestChannel(subrequestChannel) {}

  struct SendOptions {
    // TODO(soon): Support metadata.

    // contentType determines the serialization format of the message.
    jsg::Optional<kj::String> contentType;

    JSG_STRUCT(contentType);
    JSG_STRUCT_TS_OVERRIDE(QueueSendOptions {
      contentType?: QueueContentType;
    });
    // NOTE: Any new fields added here should also be added to MessageSendRequest below.
  };

  struct MessageSendRequest {
    jsg::JsRef<jsg::JsValue> body;

    // contentType determines the serialization format of the message.
    jsg::Optional<kj::String> contentType;

    JSG_STRUCT(body, contentType);
    JSG_STRUCT_TS_OVERRIDE(MessageSendRequest<Body = unknown> {
      body: Body;
      contentType?: QueueContentType;
    });
    // NOTE: Any new fields added to SendOptions must also be added here.
  };

  kj::Promise<void> send(jsg::Lock& js, jsg::JsValue body, jsg::Optional<SendOptions> options);

  kj::Promise<void> sendBatch(jsg::Lock& js, jsg::Sequence<MessageSendRequest> batch);

  JSG_RESOURCE_TYPE(WorkerQueue) {
    JSG_METHOD(send);
    JSG_METHOD(sendBatch);

    JSG_TS_ROOT();
    JSG_TS_OVERRIDE(Queue<Body = unknown> {
      send(message: Body, options?: QueueSendOptions): Promise<void>;
      sendBatch(messages: Iterable<MessageSendRequest<Body>>): Promise<void>;
    });
    JSG_TS_DEFINE(type QueueContentType = "text" | "bytes" | "json" | "v8");
  }

private:
  uint subrequestChannel;
};

// Event handler types

// Types for other workers passing messages into and responses out of a queue handler.

struct IncomingQueueMessage {
  kj::String id;
  kj::Date timestamp;
  kj::Array<kj::byte> body;
  kj::Maybe<kj::String> contentType;
  JSG_STRUCT(id, timestamp, body, contentType);

  struct ContentType {
    static constexpr kj::StringPtr TEXT = "text"_kj;
    static constexpr kj::StringPtr BYTES = "bytes"_kj;
    static constexpr kj::StringPtr JSON = "json"_kj;
    static constexpr kj::StringPtr V8 = "v8"_kj;
  };
};

struct QueueResponse {
  uint16_t outcome;
  bool retryAll;
  bool ackAll;
  kj::Array<kj::String> explicitRetries;
  kj::Array<kj::String> explicitAcks;
  JSG_STRUCT(outcome, retryAll, ackAll, explicitRetries, explicitAcks);
};

// Internal-only representation used to accumulate the results of a queue event.

struct QueueEventResult {
  bool retryAll = false;
  bool ackAll = false;
  kj::HashSet<kj::String> explicitRetries;
  kj::HashSet<kj::String> explicitAcks;
};

class QueueMessage final: public jsg::Object {
public:
  QueueMessage(jsg::Lock& js, rpc::QueueMessage::Reader message, IoPtr<QueueEventResult> result);
  QueueMessage(jsg::Lock& js, IncomingQueueMessage message, IoPtr<QueueEventResult> result);

  kj::StringPtr getId() { return id; }
  kj::Date getTimestamp() { return timestamp; }
  jsg::JsValue getBody(jsg::Lock& js);

  void retry();
  void ack();

  // TODO(soon): Add metadata support.

  JSG_RESOURCE_TYPE(QueueMessage) {
    JSG_READONLY_INSTANCE_PROPERTY(id, getId);
    JSG_READONLY_INSTANCE_PROPERTY(timestamp, getTimestamp);
    JSG_READONLY_INSTANCE_PROPERTY(body, getBody);
    JSG_METHOD(retry);
    JSG_METHOD(ack);

    JSG_TS_OVERRIDE(Message<Body = unknown> {
        readonly body: Body;
    });
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("id", id);
    tracker.trackField("body", body);
    tracker.trackFieldWithSize("IoPtr<QueueEventResult>", sizeof(IoPtr<QueueEventResult>));
  }

private:
  kj::String id;
  kj::Date timestamp;
  jsg::JsRef<jsg::JsValue> body;
  IoPtr<QueueEventResult> result;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(body);
  }
};

class QueueEvent final: public ExtendableEvent {
public:
  // TODO(cleanup): Should we get around the need for this alternative param type by just having the
  // service worker caller provide us with capnp-serialized params?
  struct Params {
    kj::String queueName;
    kj::Array<IncomingQueueMessage> messages;
  };

  explicit QueueEvent(jsg::Lock& js, rpc::EventDispatcher::QueueParams::Reader params, IoPtr<QueueEventResult> result);
  explicit QueueEvent(jsg::Lock& js, Params params, IoPtr<QueueEventResult> result);

  static jsg::Ref<QueueEvent> constructor(kj::String type) = delete;

  kj::ArrayPtr<jsg::Ref<QueueMessage>> getMessages() { return messages; }
  kj::StringPtr getQueueName() { return queueName; }

  void retryAll();
  void ackAll();

  JSG_RESOURCE_TYPE(QueueEvent) {
    JSG_INHERIT(ExtendableEvent);

    JSG_LAZY_READONLY_INSTANCE_PROPERTY(messages, getMessages);
    JSG_READONLY_INSTANCE_PROPERTY(queue, getQueueName);

    JSG_METHOD(retryAll);
    JSG_METHOD(ackAll);

    JSG_TS_ROOT();
    JSG_TS_OVERRIDE(QueueEvent<Body = unknown> {
        readonly messages: readonly Message<Body>[];
    });
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    for (auto& message: messages) {
      tracker.trackField("message", message);
    }
    tracker.trackField("queueName", queueName);
    tracker.trackFieldWithSize("IoPtr<QueueEventResult>", sizeof(IoPtr<QueueEventResult>));
  }

private:
  // TODO(perf): Should we store these in a v8 array directly rather than this intermediate kj
  // array to avoid one intermediate copy?
  kj::Array<jsg::Ref<QueueMessage>> messages;
  kj::String queueName;
  IoPtr<QueueEventResult> result;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visitAll(messages);
  }
};

// Type used when calling a module-exported queue event handler.
class QueueController final: public jsg::Object {
public:
  QueueController(jsg::Ref<QueueEvent> event)
      : event(kj::mv(event)) {}

  kj::ArrayPtr<jsg::Ref<QueueMessage>> getMessages() { return event->getMessages(); }
  kj::StringPtr getQueueName() { return event->getQueueName(); }
  void retryAll() { event->retryAll(); }
  void ackAll() { event->ackAll(); }

  JSG_RESOURCE_TYPE(QueueController) {
    JSG_READONLY_INSTANCE_PROPERTY(messages, getMessages);
    JSG_READONLY_INSTANCE_PROPERTY(queue, getQueueName);

    JSG_METHOD(retryAll);
    JSG_METHOD(ackAll);

    JSG_TS_ROOT();
    JSG_TS_OVERRIDE(MessageBatch<Body = unknown> {
      readonly messages: readonly Message<Body>[];
    });
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("event", event);
  }

private:
  jsg::Ref<QueueEvent> event;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(event);
  }
};

// Extension of ExportedHandler covering queue handlers.
struct QueueExportedHandler {
  typedef kj::Promise<void> QueueHandler(jsg::Ref<QueueController> controller,
                                         jsg::JsRef<jsg::JsValue> env,
                                         jsg::Optional<jsg::Ref<ExecutionContext>> ctx);
  jsg::LenientOptional<jsg::Function<QueueHandler>> queue;

  JSG_STRUCT(queue);
};

class QueueCustomEventImpl final: public WorkerInterface::CustomEvent, public kj::Refcounted {
public:
  QueueCustomEventImpl(
      kj::OneOf<QueueEvent::Params, rpc::EventDispatcher::QueueParams::Reader> params)
      : params(kj::mv(params)) {}

  kj::Promise<Result> run(
      kj::Own<IoContext_IncomingRequest> incomingRequest,
      kj::Maybe<kj::StringPtr> entrypointName) override;

  kj::Promise<Result> sendRpc(
      capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
      capnp::ByteStreamFactory& byteStreamFactory,
      kj::TaskSet& waitUntilTasks,
      rpc::EventDispatcher::Client dispatcher) override;

  static const uint16_t EVENT_TYPE = 5;
  uint16_t getType() override {
    return EVENT_TYPE;
  }

  bool getRetryAll() const { return result.retryAll; }
  bool getAckAll() const { return result.ackAll; }
  kj::Array<kj::String> getExplicitRetries() const;
  kj::Array<kj::String> getExplicitAcks() const;

private:
  kj::OneOf<rpc::EventDispatcher::QueueParams::Reader, QueueEvent::Params> params;
  QueueEventResult result;
};

#define EW_QUEUE_ISOLATE_TYPES \
  api::WorkerQueue,                     \
  api::WorkerQueue::SendOptions,        \
  api::WorkerQueue::MessageSendRequest, \
  api::IncomingQueueMessage,            \
  api::QueueResponse,                   \
  api::QueueMessage,                    \
  api::QueueEvent,                      \
  api::QueueController,                 \
  api::QueueExportedHandler

}  // namespace workerd::api
