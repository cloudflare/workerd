#include "queue.h"

#include <workerd/jsg/ser.h>
#include <workerd/api/global-scope.h>

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

}  // namespace workerd::api
