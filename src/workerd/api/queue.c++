// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "queue.h"
#include "util.h"

#include <workerd/jsg/buffersource.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/ser.h>
#include <workerd/util/mimetype.h>
#include <workerd/api/global-scope.h>
#include <kj/encoding.h>

namespace workerd::api {

kj::StringPtr validateContentType(kj::StringPtr contentType) {
  auto lowerCase = toLower(contentType);
  if (lowerCase == IncomingQueueMessage::ContentType::TEXT) {
    return IncomingQueueMessage::ContentType::TEXT;
  } else if (lowerCase == IncomingQueueMessage::ContentType::BYTES) {
    return IncomingQueueMessage::ContentType::BYTES;
  } else if (lowerCase == IncomingQueueMessage::ContentType::JSON) {
    return IncomingQueueMessage::ContentType::JSON;
  } else if (lowerCase == IncomingQueueMessage::ContentType::V8) {
    return IncomingQueueMessage::ContentType::V8;
  } else {
    JSG_FAIL_REQUIRE(TypeError, kj::str("Unsupported queue message content type: ", contentType));
  }
}

struct Serialized {
  kj::Maybe<kj::OneOf<kj::String, kj::Array<kj::byte>, jsg::BufferSource, jsg::BackingStore>> own;
  // Holds onto the owner of a given array of serialized data.
  kj::ArrayPtr<kj::byte> data;
  // A pointer into that data that can be directly written into an outgoing queue send, regardless
  // of its holder.
};

Serialized serializeV8(jsg::Lock& js, v8::Local<v8::Value> body) {
  // Use a specific serialization version to avoid sending messages using a new version before all
  // runtimes at the edge know how to read it.
  jsg::Serializer serializer(js.v8Isolate, jsg::Serializer::Options {
    .version = 15,
    .omitHeader = false,
  });
  serializer.write(body);
  kj::Array<kj::byte> bytes = serializer.release().data;
  Serialized result;
  result.data = bytes;
  result.own = kj::mv(bytes);
  return kj::mv(result);
}

// Control whether the serialize() method makes a deep copy of provided ArrayBuffer types or if it
// just returns a shallow reference that is only valid until the given method returns.
enum class SerializeArrayBufferBehavior {
  DEEP_COPY,
  SHALLOW_REFERENCE,
};

Serialized serialize(
    jsg::Lock& js,
    v8::Local<v8::Value> body,
    kj::StringPtr contentType,
    SerializeArrayBufferBehavior bufferBehavior) {
  if (contentType == IncomingQueueMessage::ContentType::TEXT) {
    JSG_REQUIRE(
      body->IsString(),
      TypeError,
      kj::str(
        "Content Type \"",
        IncomingQueueMessage::ContentType::TEXT,
        "\" requires a value of type string, but received: ",
        body->TypeOf(js.v8Isolate)
      )
    );

    kj::String s = js.toString(body);
    Serialized result;
    result.data = s.asBytes();
    result.own = kj::mv(s);
    return kj::mv(result);
  } else if (contentType == IncomingQueueMessage::ContentType::BYTES) {
    JSG_REQUIRE(
      body->IsArrayBufferView(),
      TypeError,
      kj::str(
        "Content Type \"",
        IncomingQueueMessage::ContentType::BYTES,
        "\" requires a value of type ArrayBufferView, but received: ",
        body->TypeOf(js.v8Isolate)
      )
    );

    jsg::BufferSource source(js, body);
    if (bufferBehavior == SerializeArrayBufferBehavior::SHALLOW_REFERENCE) {
      // If we know the data will be consumed synchronously, we can avoid copying it.
      Serialized result;
      result.data = source.asArrayPtr();
      result.own = kj::mv(source);
      return kj::mv(result);
    } else if (source.canDetach(js)) {
      // Prefer detaching the input ArrayBuffer whenever possible to avoid needing to copy it.
      auto backingSource = source.detach(js);
      Serialized result;
      result.data = backingSource.asArrayPtr();
      result.own = kj::mv(backingSource);
      return kj::mv(result);
    } else {
      kj::Array<kj::byte> bytes = kj::heapArray(source.asArrayPtr());
      Serialized result;
      result.data = bytes;
      result.own = kj::mv(bytes);
      return kj::mv(result);
    }
  } else if (contentType == IncomingQueueMessage::ContentType::JSON) {
    kj::String s = js.serializeJson(body);
    Serialized result;
    result.data = s.asBytes();
    result.own = kj::mv(s);
    return kj::mv(result);
  } else if (contentType == IncomingQueueMessage::ContentType::V8) {
    return serializeV8(js, body);
  } else {
    JSG_FAIL_REQUIRE(TypeError, kj::str("Unsupported queue message content type: ", contentType));
  }
}

kj::Promise<void> WorkerQueue::send(
    jsg::Lock& js, v8::Local<v8::Value> body, jsg::Optional<SendOptions> options) {
  auto& context = IoContext::current();

  JSG_REQUIRE(!body->IsUndefined(), TypeError, "Message body cannot be undefined");

  kj::Maybe<kj::StringPtr> contentType;
  KJ_IF_MAYBE(opts, options) {
    KJ_IF_MAYBE(type, opts->contentType) {
      contentType = validateContentType(*type);
    }
  }

  auto headers = kj::HttpHeaders(context.getHeaderTable());
  headers.set(kj::HttpHeaderId::CONTENT_TYPE, MimeType::OCTET_STREAM.toString());

  Serialized serialized;
  KJ_IF_MAYBE(type, contentType) {
    headers.add("X-Msg-Fmt", *type);
    serialized = serialize(js, body, *type, SerializeArrayBufferBehavior::DEEP_COPY);
  } else {
    // TODO(cleanup) send message format header (v8) by default
    serialized = serializeV8(js, body);
  }

  // The stage that we're sending a subrequest to provides a base URL that includes a scheme, the
  // queue broker's domain, and the start of the URL path including the account ID and queue ID. All
  // we have to do is provide the end of the path (which is "/message") to send a single message.

  auto client = context.getHttpClient(subrequestChannel, true, nullptr, "queue_send"_kjc);
  auto req = client->request(kj::HttpMethod::POST,
                             "https://fake-host/message"_kjc,
                             headers, serialized.data.size());

  auto pending = context.registerPendingEvent();
  co_await req.body->write(serialized.data.begin(), serialized.data.size());
  auto response = co_await req.response;

  JSG_REQUIRE(response.statusCode == 200, Error,
              kj::str("Queue send failed: ", response.statusText));

  // Read and discard response body, otherwise we might burn the HTTP connection.
  co_await response.body->readAllBytes().ignoreResult();
};

struct SerializedWithContentType {
  Serialized body;
  kj::Maybe<kj::StringPtr> contentType;
};

kj::Promise<void> WorkerQueue::sendBatch(
    jsg::Lock& js, jsg::Sequence<MessageSendRequest> batch) {
  auto& context = IoContext::current();

  JSG_REQUIRE(batch.size() > 0, TypeError, "sendBatch() requires at least one message");

  size_t totalSize = 0;
  size_t largestMessage = 0;
  auto messageCount = batch.size();
  auto builder = kj::heapArrayBuilder<SerializedWithContentType>(messageCount);
  for (auto& message: batch) {
    JSG_REQUIRE(!message.body.getHandle(js)->IsUndefined(), TypeError,
        "Message body cannot be undefined");

    SerializedWithContentType item;
    KJ_IF_MAYBE(contentType, message.contentType) {
      item.contentType = validateContentType(*contentType);
      item.body = serialize(js, message.body.getHandle(js), *contentType,
          SerializeArrayBufferBehavior::SHALLOW_REFERENCE);
    } else {
      item.body = serializeV8(js, message.body.getHandle(js));
    }

    builder.add(kj::mv(item));
    totalSize += builder.back().body.data.size();
    largestMessage = kj::max(largestMessage, builder.back().body.data.size());
  }
  auto serializedBodies = builder.finish();

  // Construct the request body by concatenating the messages together into a JSON message.
  // Done manually to minimize copies, although it'd be nice to make this safer.
  // (totalSize + 2) / 3 * 4 is equivalent to ceil(totalSize / 3) * 4 for base64 encoding overhead.
  auto estimatedSize = (totalSize + 2) / 3 * 4 + messageCount * 32 + 32;
  kj::Vector<char> bodyBuilder(estimatedSize);
  bodyBuilder.addAll("{\"messages\":["_kj);
  for (size_t i = 0; i < messageCount; ++i) {
    bodyBuilder.addAll("{\"body\":\""_kj);
    // TODO(perf): We should be able to encode the data directly into bodyBuilder's buffer to
    // eliminate a lot of data copying (whereas now encodeBase64 allocates a new buffer of its own
    // to hold its result, which we then have to copy into bodyBuilder).
    bodyBuilder.addAll(kj::encodeBase64(serializedBodies[i].body.data));
    bodyBuilder.add('"');

    KJ_IF_MAYBE(contentType, serializedBodies[i].contentType) {
      bodyBuilder.addAll(",\"contentType\":\""_kj);
      bodyBuilder.addAll(*contentType);
      bodyBuilder.add('"');
    }

    bodyBuilder.addAll("}"_kj);
    if (i < messageCount - 1) {
      bodyBuilder.add(',');
    }
  }
  bodyBuilder.addAll("]}"_kj);
  bodyBuilder.add('\0');
  KJ_DASSERT(bodyBuilder.size() <= estimatedSize);
  kj::String body(bodyBuilder.releaseAsArray());
  KJ_DASSERT(js.parseJson(body).getHandle(js)->IsObject());

  auto client = context.getHttpClient(subrequestChannel, true, nullptr, "queue_send"_kjc);

  // We add info about the size of the batch to the headers so that the queue implementation can
  // decide whether it's too large.
  // TODO(someday): Enforce the size limits here instead for very slightly better performance.
  auto headers = kj::HttpHeaders(context.getHeaderTable());
  headers.add("CF-Queue-Batch-Count"_kj, kj::str(messageCount));
  headers.add("CF-Queue-Batch-Bytes"_kj, kj::str(totalSize));
  headers.add("CF-Queue-Largest-Msg"_kj, kj::str(largestMessage));
  headers.set(kj::HttpHeaderId::CONTENT_TYPE, MimeType::JSON.toString());

  // The stage that we're sending a subrequest to provides a base URL that includes a scheme, the
  // queue broker's domain, and the start of the URL path including the account ID and queue ID. All
  // we have to do is provide the end of the path (which is "/batch") to send a message batch.

  auto req = client->request(kj::HttpMethod::POST,
                             "https://fake-host/batch"_kjc,
                             headers, body.size());

  auto pendingEvent = context.registerPendingEvent();
  co_await req.body->write(body.begin(), body.size());
  auto response = co_await req.response;

  JSG_REQUIRE(response.statusCode == 200, Error,
              kj::str("Queue sendBatch failed: ", response.statusText));

  // Read and discard response body, otherwise we might burn the HTTP connection.
  co_await response.body->readAllBytes().ignoreResult();
};

jsg::Value deserialize(jsg::Lock& js, kj::Array<kj::byte> body, kj::Maybe<kj::StringPtr> contentType) {
  auto type = contentType.orDefault(IncomingQueueMessage::ContentType::V8);

  if (type == IncomingQueueMessage::ContentType::TEXT) {
    return jsg::Value(js.v8Isolate, jsg::v8Str(js.v8Isolate, body.asChars()));
  } else if (type == IncomingQueueMessage::ContentType::BYTES) {
    return jsg::Value(js.v8Isolate, js.wrapBytes(kj::mv(body)));
  } else if (type == IncomingQueueMessage::ContentType::JSON) {
    return js.parseJson(jsg::v8Str(js.v8Isolate, body.asChars()));
  } else if (type == IncomingQueueMessage::ContentType::V8) {
    return jsg::Value(js.v8Isolate, jsg::Deserializer(js.v8Isolate, body.asPtr()).readValue());
  } else {
    JSG_FAIL_REQUIRE(TypeError, kj::str("Unsupported queue message content type: ", type));
  }
}

jsg::Value deserialize(jsg::Lock& js, rpc::QueueMessage::Reader message) {
  kj::StringPtr type = message.getContentType();
  if (type == "") {
    // default to v8 format
    type = IncomingQueueMessage::ContentType::V8;
  }

  if (type == IncomingQueueMessage::ContentType::TEXT) {
    return jsg::Value(js.v8Isolate, jsg::v8Str(js.v8Isolate, message.getData().asChars()));
  } else if (type == IncomingQueueMessage::ContentType::BYTES) {
    kj::Array<kj::byte> bytes = kj::heapArray(message.getData().asBytes());
    return jsg::Value(js.v8Isolate, js.wrapBytes(kj::mv(bytes)));
  } else if (type == IncomingQueueMessage::ContentType::JSON) {
    return js.parseJson(jsg::v8Str(js.v8Isolate, message.getData().asChars()));
  } else if (type == IncomingQueueMessage::ContentType::V8) {
    return jsg::Value(js.v8Isolate, jsg::Deserializer(js.v8Isolate, message.getData()).readValue());
  } else {
    JSG_FAIL_REQUIRE(TypeError, kj::str("Unsupported queue message content type: ", type));
  }
}

QueueMessage::QueueMessage(
    jsg::Lock& js, rpc::QueueMessage::Reader message, IoPtr<QueueEventResult> result)
    : id(kj::str(message.getId())),
      timestamp(message.getTimestampNs() * kj::NANOSECONDS + kj::UNIX_EPOCH),
      body(deserialize(js, message)),
      result(result) {}
// Note that we must make deep copies of all data here since the incoming Reader may be
// deallocated while JS's GC wrappers still exist.

QueueMessage::QueueMessage(
    jsg::Lock& js, IncomingQueueMessage message, IoPtr<QueueEventResult> result)
    : id(kj::mv(message.id)),
      timestamp(message.timestamp),
      body(deserialize(js, kj::mv(message.body), message.contentType)),
      result(result) {}

jsg::Value QueueMessage::getBody(jsg::Lock& js) {
  return body.addRef(js);
}

void QueueMessage::retry() {
  if (result->retryAll) {
    return;
  }

  if (result->ackAll) {
    auto msg = kj::str(
        "Received a call to retry() on message ", id, " after ackAll() was already called. "
        "Calling retry() on a message after calling ackAll() has no effect.");
    IoContext::current().logWarning(msg);
    return;
  }

  if (result->explicitAcks.contains(id)) {
    auto msg = kj::str(
        "Received a call to retry() on message ", id, " after ack() was already called. "
        "Calling retry() on a message after calling ack() has no effect.");
    IoContext::current().logWarning(msg);
    return;
  }
  result->explicitRetries.findOrCreate(id, [this]() { return kj::heapString(id); } );
}

void QueueMessage::ack() {
  if (result->ackAll) {
    return;
  }

  if (result->retryAll) {
    auto msg = kj::str(
        "Received a call to ack() on message ", id, " after retryAll() was already called. "
        "Calling ack() on a message after calling retryAll() has no effect.");
    IoContext::current().logWarning(msg);
    return;
  }

  if (result->explicitRetries.contains(id)) {
    auto msg = kj::str(
        "Received a call to ack() on message ", id, " after retry() was already called. "
        "Calling ack() on a message after calling retry() has no effect.");
    IoContext::current().logWarning(msg);
    return;
  }
  result->explicitAcks.findOrCreate(id, [this]() { return kj::heapString(id); } );
}

QueueEvent::QueueEvent(jsg::Lock& js, rpc::EventDispatcher::QueueParams::Reader params, IoPtr<QueueEventResult> result)
    : ExtendableEvent("queue"), queueName(kj::heapString(params.getQueueName())), result(result) {
  // Note that we must make deep copies of all data here since the incoming Reader may be
  // deallocated while JS's GC wrappers still exist.
  auto incoming = params.getMessages();
  auto messagesBuilder = kj::heapArrayBuilder<jsg::Ref<QueueMessage>>(incoming.size());
  for (auto i: kj::indices(incoming)) {
    messagesBuilder.add(jsg::alloc<QueueMessage>(js, incoming[i], result));
  }
  messages = messagesBuilder.finish();
}

QueueEvent::QueueEvent(jsg::Lock& js, Params params, IoPtr<QueueEventResult> result)
    : ExtendableEvent("queue"), queueName(kj::mv(params.queueName)), result(result)  {
  auto messagesBuilder = kj::heapArrayBuilder<jsg::Ref<QueueMessage>>(params.messages.size());
  for (auto i: kj::indices(params.messages)) {
    messagesBuilder.add(jsg::alloc<QueueMessage>(js, kj::mv(params.messages[i]), result));
  }
  messages = messagesBuilder.finish();
}

void QueueEvent::retryAll() {
  if (result->ackAll) {
    IoContext::current().logWarning(
        "Received a call to retryAll() after ackAll() was already called. "
        "Calling retryAll() after calling ackAll() has no effect.");
    return;
  }
  result->retryAll = true;
}

void QueueEvent::ackAll() {
  if (result->retryAll) {
    IoContext::current().logWarning(
        "Received a call to ackAll() after retryAll() was already called. "
        "Calling ackAll() after calling retryAll() has no effect.");
    return;
  }
  result->ackAll = true;
}

namespace {
jsg::Ref<QueueEvent> startQueueEvent(
    EventTarget& globalEventTarget,
    kj::OneOf<rpc::EventDispatcher::QueueParams::Reader, QueueEvent::Params> params,
    IoPtr<QueueEventResult> result,
    Worker::Lock& lock, kj::Maybe<ExportedHandler&> exportedHandler,
    const jsg::TypeHandler<QueueExportedHandler>& handlerHandler) {
  jsg::Lock& js = lock;
  // Start a queue event (called from C++, not JS). Similar to startScheduled(), the caller must
  // wait for waitUntil()s to produce the final QueueResult.
  jsg::Ref<QueueEvent> event(nullptr);
  KJ_SWITCH_ONEOF(params) {
    KJ_CASE_ONEOF(p, rpc::EventDispatcher::QueueParams::Reader) {
      event = jsg::alloc<QueueEvent>(js, p, result);
    }
    KJ_CASE_ONEOF(p, QueueEvent::Params) {
      event = jsg::alloc<QueueEvent>(js, kj::mv(p), result);
    }
  }

  KJ_IF_MAYBE(h, exportedHandler) {
    auto queueHandler = KJ_ASSERT_NONNULL(handlerHandler.tryUnwrap(
        lock, h->self.getHandle(lock.getIsolate())));
    KJ_IF_MAYBE(f, queueHandler.queue) {
      auto promise = (*f)(lock, jsg::alloc<QueueController>(event.addRef()),
                          h->env.addRef(js), h->getCtx());
      event->waitUntil(kj::mv(promise));
    } else {
      lock.logWarningOnce(
          "Received a QueueEvent but we lack a handler for QueueEvents. "
          "Did you remember to export a queue() function?");
      JSG_FAIL_REQUIRE(Error, "Handler does not export a queue() function.");
    }
  } else {
    if (globalEventTarget.getHandlerCount("queue") == 0) {
      lock.logWarningOnce(
          "Received a QueueEvent but we lack an event listener for queue events. "
          "Did you remember to call addEventListener(\"queue\", ...)?");
      JSG_FAIL_REQUIRE(Error, "No event listener registered for queue messages.");
    }
    globalEventTarget.dispatchEventImpl(lock, event.addRef());
  }

  return event.addRef();
}
}

kj::Promise<WorkerInterface::CustomEvent::Result> QueueCustomEventImpl::run(
    kj::Own<IoContext_IncomingRequest> incomingRequest,
    kj::Maybe<kj::StringPtr> entrypointName) {
  incomingRequest->delivered();
  auto& context = incomingRequest->getContext();

  kj::String queueName;
  uint32_t batchSize;
  KJ_SWITCH_ONEOF(params) {
    KJ_CASE_ONEOF(p, rpc::EventDispatcher::QueueParams::Reader) {
      queueName = kj::heapString(p.getQueueName());
      batchSize = p.getMessages().size();
    }
    KJ_CASE_ONEOF(p, QueueEvent::Params) {
      queueName = kj::heapString(p.queueName);
      batchSize = p.messages.size();
    }
  }

  KJ_IF_MAYBE(t, incomingRequest->getWorkerTracer()) {
    t->setEventInfo(context.now(), Trace::QueueEventInfo(kj::mv(queueName), batchSize));
  }

  // Create a custom refcounted type for holding the queueEvent so that we can pass it to the
  // waitUntil'ed callback safely without worrying about whether this coroutine gets canceled.
  struct QueueEventHolder : public kj::Refcounted {
    jsg::Ref<QueueEvent> event = nullptr;
  };
  auto queueEventHolder = kj::refcounted<QueueEventHolder>();

  // It's a little ugly, but the usage of waitUntil (and finishScheduled) down below are here so
  // that users can write queue handlers in the old addEventListener("queue", ...) syntax (where we
  // can't just wait on their addEventListener handler to resolve because it can't be async).
  context.addWaitUntil(context.run(
      [this, entrypointName=entrypointName, &context, queueEvent = kj::addRef(*queueEventHolder),
       &metrics = incomingRequest->getMetrics()]
      (Worker::Lock& lock) mutable {
    jsg::AsyncContextFrame::StorageScope traceScope = context.makeAsyncTraceScope(lock);

    auto& typeHandler = lock.getWorker().getIsolate().getApiIsolate().getQueueTypeHandler(lock);
    queueEvent->event = startQueueEvent(lock.getGlobalScope(), kj::mv(params), context.addObject(result), lock,
        lock.getExportedHandler(entrypointName, context.getActor()), typeHandler);
  }));

  // TODO(soon): There's a good chance we'll want a different wall-clock timeout for queue handlers
  // than for scheduled workers, but it's not at all clear yet to me what it should be, so just
  // reuse the scheduled worker logic and timeout for now.
  auto completed = co_await incomingRequest->finishScheduled();

  co_return WorkerInterface::CustomEvent::Result {
    .outcome = completed ? context.waitUntilStatus() : EventOutcome::EXCEEDED_CPU,
  };
}

kj::Promise<WorkerInterface::CustomEvent::Result> QueueCustomEventImpl::sendRpc(
    capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
    capnp::ByteStreamFactory& byteStreamFactory,
    kj::TaskSet& waitUntilTasks,
    rpc::EventDispatcher::Client dispatcher) {
  auto req = dispatcher.castAs<rpc::EventDispatcher>().queueRequest();
  KJ_SWITCH_ONEOF(params) {
    KJ_CASE_ONEOF(p, rpc::EventDispatcher::QueueParams::Reader) {
      req.setQueueName(p.getQueueName());
      req.setMessages(p.getMessages());
    }
    KJ_CASE_ONEOF(p, QueueEvent::Params) {
      req.setQueueName(p.queueName);
      auto messages = req.initMessages(p.messages.size());
      for (auto i: kj::indices(p.messages)) {
        messages[i].setId(p.messages[i].id);
        messages[i].setTimestampNs((p.messages[i].timestamp - kj::UNIX_EPOCH) / kj::NANOSECONDS);
        messages[i].setData(p.messages[i].body);
        KJ_IF_MAYBE(contentType, p.messages[i].contentType) {
          messages[i].setContentType(*contentType);
        }
      }
    }
  }

  return req.send().then([this](auto resp) {
    auto respResult = resp.getResult();
    this->result.retryAll = respResult.getRetryAll();
    this->result.ackAll = respResult.getAckAll();
    this->result.explicitRetries.clear();
    for (const auto& msgId : respResult.getExplicitRetries()) {
      this->result.explicitRetries.insert(kj::heapString(msgId));
    }
    this->result.explicitAcks.clear();
    for (const auto& msgId : respResult.getExplicitAcks()) {
      this->result.explicitAcks.insert(kj::heapString(msgId));
    }
    return WorkerInterface::CustomEvent::Result {
      .outcome = respResult.getOutcome(),
    };
  });
}

kj::Array<kj::String> QueueCustomEventImpl::getExplicitRetries() const {
  auto retryArray = kj::heapArrayBuilder<kj::String>(result.explicitRetries.size());
  for (const auto& msgId : result.explicitRetries) {
    retryArray.add(kj::heapString(msgId));
  }
  return retryArray.finish();
}

kj::Array<kj::String> QueueCustomEventImpl::getExplicitAcks() const {
  auto ackArray = kj::heapArrayBuilder<kj::String>(result.explicitAcks.size());
  for (const auto& msgId : result.explicitAcks) {
    ackArray.add(kj::heapString(msgId));
  }
  return ackArray.finish();
}

}  // namespace workerd::api
