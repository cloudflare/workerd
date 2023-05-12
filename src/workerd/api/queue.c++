#include "queue.h"

#include <workerd/jsg/ser.h>
#include <workerd/api/global-scope.h>
#include <kj/encoding.h>

namespace workerd::api {

kj::Promise<void> WorkerQueue::send(
    v8::Local<v8::Value> body, jsg::Optional<SendOptions> options, v8::Isolate* isolate) {
  auto& context = IoContext::current();

  JSG_REQUIRE(!body->IsUndefined(), TypeError, "Message body cannot be undefined");

  // Use a specific serialization version to avoid sending messages using a new version before all
  // runtimes at the edge know how to read it.
  jsg::Serializer serializer(isolate, jsg::Serializer::Options {
    .version = 15,
    .omitHeader = false,
  });
  serializer.write(body);
  kj::Array<kj::byte> serialized = serializer.release().data;

  auto client = context.getHttpClient(subrequestChannel, true, nullptr, "queue_send"_kj);

  // The stage that we're sending a subrequest to provides a base URL that includes a scheme, the
  // queue broker's domain, and the start of the URL path including the account ID and queue ID. All
  // we have to do is provide the end of the path (which is "/message") to send a single message.
  kj::StringPtr url = "https://fake-host/message"_kj;
  auto headers = kj::HttpHeaders(context.getHeaderTable());
  headers.set(kj::HttpHeaderId::CONTENT_TYPE, "application/octet-stream");
  auto req = client->request(kj::HttpMethod::POST, url, headers, serialized.size());

  return req.body->write(serialized.begin(), serialized.size())
      .attach(kj::mv(serialized), kj::mv(req.body), context.registerPendingEvent())
      .then([resp = kj::mv(req.response), &context]() mutable {
    return resp.then([](kj::HttpClient::Response&& response) mutable {
      if (response.statusCode != 200) {
        // Manually construct exception so that we can include the status text.
        kj::throwFatalException(kj::Exception(kj::Exception::Type::FAILED, __FILE__, __LINE__,
              kj::str(JSG_EXCEPTION(Error) ": Queue send failed: ", response.statusText)));
      }

      // Read and discard response body, otherwise we might burn the HTTP connection.
      return response.body->readAllBytes().attach(kj::mv(response.body)).ignoreResult();
    }).attach(context.registerPendingEvent());
  }).attach(kj::mv(client));
};

kj::Promise<void> WorkerQueue::sendBatch(
    jsg::Sequence<MessageSendRequest> batch, v8::Isolate* isolate) {
  auto& context = IoContext::current();

  JSG_REQUIRE(batch.size() > 0, TypeError, "sendBatch() requires at least one message");

  size_t totalSize = 0;
  size_t largestMessage = 0;
  auto messageCount = batch.size();
  auto builder = kj::heapArrayBuilder<kj::Array<byte>>(messageCount);
  for (auto& message: batch) {
    JSG_REQUIRE(!message.body.getHandle(isolate)->IsUndefined(), TypeError,
        "Message body cannot be undefined");

    // Use a specific serialization version to avoid sending messages using a new version before all
    // runtimes at the edge know how to read it.
    // TODO(perf): Would we be better off just serializing all the messages together in one big
    // buffer rather than separately?
    jsg::Serializer serializer(isolate, jsg::Serializer::Options {
      .version = 15,
      .omitHeader = false,
    });
    serializer.write(kj::mv(message.body));
    builder.add(serializer.release().data);
    totalSize += builder.back().size();
    largestMessage = kj::max(largestMessage, builder.back().size());
  }
  auto serializedBodies = builder.finish();

  // Construct the request body by concatenating the messages together into a JSON message.
  // Done manually to minimize copies, although it'd be nice to make this safer.
  // (totalSize + 2) / 3 * 4 is equivalent to ceil(totalSize / 3) * 4 for base64 encoding overhead.
  auto estimatedSize = (totalSize + 2) / 3 * 4 + messageCount * 16 + 32;
  kj::Vector<char> bodyBuilder(estimatedSize);
  bodyBuilder.addAll("{\"messages\":["_kj);
  for (size_t i = 0; i < messageCount; ++i) {
    bodyBuilder.addAll("{\"body\":\""_kj);
    // TODO(perf): We should be able to encode the data directly into bodyBuilder's buffer to
    // eliminate a lot of data copying (whereas now encodeBase64 allocates a new buffer of its own
    // to hold its result, which we then have to copy into bodyBuilder).
    bodyBuilder.addAll(kj::encodeBase64(serializedBodies[i]));
    bodyBuilder.addAll("\"}"_kj);
    if (i < messageCount - 1) {
      bodyBuilder.add(',');
    }
  }
  bodyBuilder.addAll("]}"_kj);
  bodyBuilder.add('\0');
  KJ_DASSERT(bodyBuilder.size() <= estimatedSize);
  kj::String body(bodyBuilder.releaseAsArray());
  KJ_DASSERT(jsg::check(
        v8::JSON::Parse(isolate->GetCurrentContext(), jsg::v8Str(isolate, body)))->IsObject());

  auto client = context.getHttpClient(subrequestChannel, true, nullptr, "queue_send"_kj);

  // The stage that we're sending a subrequest to provides a base URL that includes a scheme, the
  // queue broker's domain, and the start of the URL path including the account ID and queue ID. All
  // we have to do is provide the end of the path (which is "/batch") to send a message batch.
  kj::StringPtr url = "https://fake-host/batch"_kj;

  // We add info about the size of the batch to the headers so that the queue implementation can
  // decide whether it's too large.
  // TODO(someday): Enforce the size limits here instead for very slightly better performance.
  auto headers = kj::HttpHeaders(context.getHeaderTable());
  headers.add("CF-Queue-Batch-Count"_kj, kj::str(messageCount));
  headers.add("CF-Queue-Batch-Bytes"_kj, kj::str(totalSize));
  headers.add("CF-Queue-Largest-Msg"_kj, kj::str(largestMessage));
  headers.set(kj::HttpHeaderId::CONTENT_TYPE, "application/json"_kj);

  auto req = client->request(kj::HttpMethod::POST, url, headers, body.size());

  return req.body->write(body.begin(), body.size())
      .attach(kj::mv(body), kj::mv(req.body), context.registerPendingEvent())
      .then([resp = kj::mv(req.response), &context]() mutable {
    return resp.then([](kj::HttpClient::Response&& response) mutable {
      if (response.statusCode != 200) {
        // Manually construct exception so that we can include the status text.
        kj::throwFatalException(kj::Exception(kj::Exception::Type::FAILED, __FILE__, __LINE__,
              kj::str(JSG_EXCEPTION(Error) ": Queue sendBatch failed: ", response.statusText)));
      }

      // Read and discard response body, otherwise we might burn the HTTP connection.
      return response.body->readAllBytes().attach(kj::mv(response.body)).ignoreResult();
    }).attach(context.registerPendingEvent());
  }).attach(kj::mv(client));
};

QueueMessage::QueueMessage(
    v8::Isolate* isolate, rpc::QueueMessage::Reader message, IoPtr<QueueEventResult> result)
    : id(kj::str(message.getId())),
      timestamp(message.getTimestampNs() * kj::NANOSECONDS + kj::UNIX_EPOCH),
      body(isolate, jsg::Deserializer(isolate, message.getData()).readValue()),
      result(result) {}
// Note that we must make deep copies of all data here since the incoming Reader may be
// deallocated while JS's GC wrappers still exist.

QueueMessage::QueueMessage(
    v8::Isolate* isolate, IncomingQueueMessage message, IoPtr<QueueEventResult> result)
    : id(kj::mv(message.id)),
      timestamp(message.timestamp),
      body(isolate, jsg::Deserializer(isolate, message.body.asPtr()).readValue()),
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

QueueEvent::QueueEvent(v8::Isolate* isolate, rpc::EventDispatcher::QueueParams::Reader params, IoPtr<QueueEventResult> result)
    : ExtendableEvent("queue"), queueName(kj::heapString(params.getQueueName())), result(result) {
  // Note that we must make deep copies of all data here since the incoming Reader may be
  // deallocated while JS's GC wrappers still exist.
  auto incoming = params.getMessages();
  auto messagesBuilder = kj::heapArrayBuilder<jsg::Ref<QueueMessage>>(incoming.size());
  for (auto i: kj::indices(incoming)) {
    messagesBuilder.add(jsg::alloc<QueueMessage>(isolate, incoming[i], result));
  }
  messages = messagesBuilder.finish();
}

QueueEvent::QueueEvent(v8::Isolate* isolate, Params params, IoPtr<QueueEventResult> result)
    : ExtendableEvent("queue"), queueName(kj::mv(params.queueName)), result(result)  {
  auto messagesBuilder = kj::heapArrayBuilder<jsg::Ref<QueueMessage>>(params.messages.size());
  for (auto i: kj::indices(params.messages)) {
    messagesBuilder.add(jsg::alloc<QueueMessage>(isolate, kj::mv(params.messages[i]), result));
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

jsg::Ref<QueueEvent> startQueueEvent(
    EventTarget& globalEventTarget,
    kj::OneOf<rpc::EventDispatcher::QueueParams::Reader, QueueEvent::Params> params,
    IoPtr<QueueEventResult> result,
    Worker::Lock& lock, kj::Maybe<ExportedHandler&> exportedHandler,
    const jsg::TypeHandler<QueueExportedHandler>& handlerHandler) {
  auto isolate = lock.getIsolate();
  jsg::Ref<QueueEvent> event(nullptr);
  KJ_SWITCH_ONEOF(params) {
    KJ_CASE_ONEOF(p, rpc::EventDispatcher::QueueParams::Reader) {
      event = jsg::alloc<QueueEvent>(isolate, p, result);
    }
    KJ_CASE_ONEOF(p, QueueEvent::Params) {
      event = jsg::alloc<QueueEvent>(isolate, kj::mv(p), result);
    }
  }

  KJ_IF_MAYBE(h, exportedHandler) {
    auto queueHandler = KJ_ASSERT_NONNULL(handlerHandler.tryUnwrap(
        lock, h->self.getHandle(lock.getIsolate())));
    KJ_IF_MAYBE(f, queueHandler.queue) {
      auto promise = (*f)(lock, jsg::alloc<QueueController>(event.addRef()),
                          h->env.addRef(isolate), h->getCtx(isolate));
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
