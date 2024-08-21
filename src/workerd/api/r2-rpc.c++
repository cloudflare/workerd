// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "r2-rpc.h"
#include <workerd/api/util.h>
#include <workerd/api/system-streams.h>
#include <workerd/util/http-util.h>
#include <workerd/api/r2-api.capnp.h>
// This is imported for the error type and that's shared between internal and public beta.

#include <kj/compat/http.h>
#include <capnp/message.h>
#include <capnp/compat/json.h>

namespace workerd::api {
static kj::Own<R2Error> toError(uint statusCode, kj::StringPtr responseBody) {
  capnp::JsonCodec json;
  json.handleByAnnotation<public_beta::R2ErrorResponse>();
  capnp::MallocMessageBuilder errorMessageArena;
  auto errorMessage = errorMessageArena.initRoot<public_beta::R2ErrorResponse>();
  json.decode(responseBody, errorMessage);

  return kj::refcounted<R2Error>(errorMessage.getV4code(), kj::str(errorMessage.getMessage()));
}

jsg::JsValue R2Error::getStack(jsg::Lock& js) {
  return jsg::JsObject(KJ_ASSERT_NONNULL(errorForStack).Get(js.v8Isolate)).get(js, "stack"_kj);
}

kj::Maybe<uint> R2Result::v4ErrorCode() {
  KJ_IF_SOME(e, toThrow) {
    return e->v4Code;
  }
  return kj::none;
}

void R2Result::throwIfError(
    kj::StringPtr action, const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType) {
  KJ_IF_SOME(e, toThrow) {
    // TODO(soon): Once jsg::JsPromise exists, switch to using that to tunnel out the exception. As
    // it stands today, unfortunately, all we can send back to the user is a message. R2Error isn't
    // a registered type in the runtime. When reenabling, make sure to update overrides/r2.d.ts to
    // reenable the type
#if 0
    auto isolate = IoContext::current().getCurrentLock().getIsolate();
    (*e)->action = kj::str(action);
    (*e)->errorForStack = v8::Global<v8::Object>(
        isolate, v8::Exception::Error(v8::String::Empty(isolate)).As<v8::Object>());
    isolate->ThrowException(errorType.wrapRef(kj::mv(*e)));
    throw jsg::JsExceptionThrown();
#else
    JSG_FAIL_REQUIRE(Error, kj::str(action, ": ", e.get()->getMessage(), " (", e->v4Code, ')'));
#endif
  }
}

namespace {
kj::String getFakeUrl(kj::ArrayPtr<kj::StringPtr> path) {
  kj::Url url;
  url.scheme = kj::str("https");
  url.host = kj::str("fake-host");
  for (const auto& p: path) {
    url.path.add(kj::str(p));
  }
  return url.toString(kj::Url::Context::HTTP_PROXY_REQUEST);
}
}  // namespace

kj::Promise<R2Result> doR2HTTPGetRequest(kj::Own<kj::HttpClient> client,
    kj::String metadataPayload,
    kj::ArrayPtr<kj::StringPtr> path,
    kj::Maybe<kj::StringPtr> jwt,
    CompatibilityFlags::Reader flags) {
  auto& context = IoContext::current();
  auto url = getFakeUrl(path);

  auto& headerIds = context.getHeaderIds();

  auto requestHeaders = kj::HttpHeaders(context.getHeaderTable());
  requestHeaders.set(headerIds.cfBlobRequest, kj::mv(metadataPayload));
  KJ_IF_SOME(j, jwt) {
    requestHeaders.set(headerIds.authorization, kj::str("Bearer ", j));
  }

  static auto constexpr processStream =
      [](kj::StringPtr metadata, kj::HttpClient::Response& response, kj::Own<kj::HttpClient> client,
          CompatibilityFlags::Reader flags, IoContext& context) -> kj::Promise<R2Result> {
    auto stream = newSystemStream(response.body.attach(kj::mv(client)),
        getContentEncoding(context, *response.headers, Response::BodyEncoding::AUTO, flags),
        context);
    auto metadataSize = atoi((metadata).cStr());
    // R2 itself will try to stick to a cap of 256 KiB of response here. However for listing
    // sometimes our heuristics have corner cases. This way we're more lenient in case someone
    // finds a corner case for the heuristic so that we don't fail the GET with an opaque
    // internal error.
    KJ_REQUIRE(metadataSize <= 1024 * 1024, "R2 metadata size seems way too large");
    KJ_REQUIRE(metadataSize >= 0, "R2 metadata size parsed as negative");

    auto metadataBuffer = kj::heapArray<char>(metadataSize);
    auto metadataReadLength =
        co_await stream->tryRead(metadataBuffer.begin(), metadataSize, metadataSize);

    KJ_ASSERT(
        metadataReadLength == metadataBuffer.size(), "R2 metadata buffer not read fully/overflow?");

    co_return R2Result{.httpStatus = response.statusCode,
      .metadataPayload = kj::mv(metadataBuffer),
      .stream = kj::mv(stream)};
  };

  auto request = client->request(kj::HttpMethod::GET, url, requestHeaders, (uint64_t)0);

  auto response = co_await request.response;

  if (response.statusCode >= 400) {
    // Error responses should have a cfR2ErrorHeader but don't always. If there
    // isn't one, we'll use a generic error.
    if (response.headers->get(headerIds.cfR2ErrorHeader) == kj::none) {
      LOG_WARNING_ONCE(
          "R2 error response does not contain the CF-R2-Error header.", response.statusCode);
    }
    auto error =
        response.headers->get(headerIds.cfR2ErrorHeader)
            .orDefault("{\"version\":0,\"v4code\":0,\"message\":\"Unspecified error\"}"_kj);

    R2Result result = {
      .httpStatus = response.statusCode,
      .toThrow = toError(response.statusCode, error),
    };

    KJ_IF_SOME(m, response.headers->get(headerIds.cfBlobMetadataSize)) {
      auto processed = co_await processStream(m, response, kj::mv(client), flags, context);
      result.metadataPayload = kj::mv(processed.metadataPayload);
      result.stream = kj::mv(processed.stream);
    }

    co_return kj::mv(result);
  }

  KJ_IF_SOME(m, response.headers->get(headerIds.cfBlobMetadataSize)) {
    co_return co_await processStream(m, response, kj::mv(client), flags, context);
  } else {
    co_return R2Result{.httpStatus = response.statusCode};
  }
}

kj::Promise<R2Result> doR2HTTPPutRequest(kj::Own<kj::HttpClient> client,
    kj::Maybe<R2PutValue> supportedBody,
    kj::Maybe<uint64_t> streamSize,
    kj::String metadataPayload,
    kj::ArrayPtr<kj::StringPtr> path,
    kj::Maybe<kj::StringPtr> jwt) {
  // NOTE: A lot of code here is duplicated with kv.c++. Maybe it can be refactored to be more
  // reusable?
  auto& context = IoContext::current();
  auto headers = kj::HttpHeaders(context.getHeaderTable());
  auto url = getFakeUrl(path);

  kj::Maybe<uint64_t> expectedBodySize;

  KJ_IF_SOME(b, supportedBody) {
    KJ_SWITCH_ONEOF(b) {
      KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
        expectedBodySize = stream->tryGetLength(StreamEncoding::IDENTITY);
        if (expectedBodySize == kj::none) {
          expectedBodySize = streamSize;
        }
        JSG_REQUIRE(expectedBodySize != kj::none, TypeError,
            "Provided readable stream must have a known length (request/response body or readable "
            "half of FixedLengthStream)");
        JSG_REQUIRE(streamSize.orDefault(KJ_ASSERT_NONNULL(expectedBodySize)) == expectedBodySize,
            RangeError, "Provided stream length (", streamSize.orDefault(-1),
            ") doesn't match what "
            "the stream reports (",
            KJ_ASSERT_NONNULL(expectedBodySize), ")");
      }
      KJ_CASE_ONEOF(text, jsg::NonCoercible<kj::String>) {
        expectedBodySize = text.value.size();
        KJ_REQUIRE(streamSize == kj::none);
      }
      KJ_CASE_ONEOF(data, kj::Array<kj::byte>) {
        expectedBodySize = data.size();
        KJ_REQUIRE(streamSize == kj::none);
      }
      KJ_CASE_ONEOF(data, jsg::Ref<Blob>) {
        expectedBodySize = data->getSize();
        KJ_REQUIRE(streamSize == kj::none);
      }
    }
  } else {
    expectedBodySize = uint64_t(0);
    KJ_REQUIRE(streamSize == kj::none);
  }

  headers.set(context.getHeaderIds().cfBlobMetadataSize, kj::str(metadataPayload.size()));
  KJ_IF_SOME(j, jwt) {
    headers.set(context.getHeaderIds().authorization, kj::str("Bearer ", j));
  }

  uint64_t combinedSize = metadataPayload.size() + KJ_ASSERT_NONNULL(expectedBodySize);

  co_await context.waitForOutputLocks();

  auto request = client->request(kj::HttpMethod::PUT, url, headers, combinedSize);

  co_await request.body->write(metadataPayload.asBytes());

  KJ_IF_SOME(b, supportedBody) {
    KJ_SWITCH_ONEOF(b) {
      KJ_CASE_ONEOF(text, jsg::NonCoercible<kj::String>) {
        co_await request.body->write(text.value.asBytes());
      }
      KJ_CASE_ONEOF(data, kj::Array<byte>) {
        co_await request.body->write(data);
      }
      KJ_CASE_ONEOF(blob, jsg::Ref<Blob>) {
        auto data = blob->getData();
        co_await request.body->write(data);
      }
      KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
        // Because the ReadableStream might be a fully JavaScript-backed stream, we must
        // start running the pump within the IoContex/isolate lock.
        co_await context.run(
            [dest = newSystemStream(kj::mv(request.body), StreamEncoding::IDENTITY, context),
                stream = kj::mv(stream)](jsg::Lock& js) mutable {
          return IoContext::current().waitForDeferredProxy(stream->pumpTo(js, kj::mv(dest), true));
        });
      }
    }
  }

  auto response = co_await request.response;

  if (response.statusCode >= 400) {
    // Error responses should have a cfR2ErrorHeader but don't always. If there
    // isn't one, we'll use a generic error.
    auto& headerIds = context.getHeaderIds();
    if (response.headers->get(headerIds.cfR2ErrorHeader) == kj::none) {
      LOG_WARNING_ONCE(
          "R2 error response does not contain the CF-R2-Error header.", response.statusCode);
    }
    auto error =
        response.headers->get(headerIds.cfR2ErrorHeader)
            .orDefault("{\"version\":0,\"v4code\":0,\"message\":\"Unspecified error\"}"_kj);

    co_return R2Result{
      .httpStatus = response.statusCode,
      .toThrow = toError(response.statusCode, error),
    };
  }

  auto responseBody = co_await response.body->readAllText();

  co_return R2Result{
    .httpStatus = response.statusCode,
    .metadataPayload = responseBody.releaseArray(),
  };
}
}  // namespace workerd::api
