#pragma once

#include <kj/common.h>

#include <workerd/jsg/jsg.h>

namespace workerd::api {

using kj::uint;

// Binding types

class WorkerQueue: public jsg::Object {
  // A capability to a Worker Queue.

public:
  WorkerQueue(uint subrequestChannel)
    : subrequestChannel(subrequestChannel) {}
  // `subrequestChannel` is what to pass to IoContext::getHttpClient() to get an HttpClient
  // representing this queue.

  struct SendOptions {
    // TODO(soon): Support metadata.

    jsg::Optional<bool> tmp;
    // TODO(soon): Remove this -- it's only here to make JSG_STRUCT work since it doesn't if a
    // struct doesn't have any fields.

    JSG_STRUCT(tmp);
    JSG_STRUCT_TS_OVERRIDE(QueueSendOptions {
      tmp: never;
    });
    // NOTE: Any new fields added here should also be added to MessageSendRequest below.
  };

  struct MessageSendRequest {
    jsg::Value body;

    JSG_STRUCT(body);
    JSG_STRUCT_TS_OVERRIDE(MessageSendRequest<Body = unknown> {
      body: Body;
    });
    // NOTE: Any new fields added to SendOptions must also be added here.
  };

  kj::Promise<void> send(
      v8::Local<v8::Value> body, jsg::Optional<SendOptions> options, v8::Isolate* isolate);

  kj::Promise<void> sendBatch(jsg::Sequence<MessageSendRequest> batch, v8::Isolate* isolate);

  JSG_RESOURCE_TYPE(WorkerQueue) {
    JSG_METHOD(send);
    JSG_METHOD(sendBatch);

    JSG_TS_ROOT();
    JSG_TS_OVERRIDE(Queue<Body> {
      send(message: Body): Promise<void>;
      sendBatch(messages: Iterable<MessageSendRequest<Body>>): Promise<void>;
    });
  }

private:
  uint subrequestChannel;
};

#define EW_QUEUE_ISOLATE_TYPES \
  api::WorkerQueue,                     \
  api::WorkerQueue::SendOptions,        \
  api::WorkerQueue::MessageSendRequest

}  // namespace edgeworker::api
