// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "r2-rpc.h"

#include <workerd/api/r2-api.capnp.h>
#include <workerd/api/streams/common.h>
#include <workerd/api/streams/readable.h>
#include <workerd/api/system-streams.h>
#include <workerd/api/util.h>
#include <workerd/io/trace.h>
#include <workerd/util/http-util.h>
// This is imported for the error type and that's shared between internal and public beta.

#include <capnp/compat/json.h>
#include <capnp/message.h>
#include <kj/compat/http.h>

#include <cmath>
#include <limits>

namespace workerd::api {

namespace {

constexpr size_t R2_RPC_INLINE_BODY_LIMIT = 16u << 20;

JsReadableStream makeR2RpcMemoryStream(
    jsg::Lock& js, kj::ArrayPtr<const byte> bytes, kj::Maybe<kj::Own<void>> backing = kj::none) {
  return JsReadableStream::create(
      js, IoContext::current(), newMemorySource(bytes, kj::mv(backing)));
}

v8::Local<v8::Value> getEnvelopeProperty(
    jsg::Lock& js, v8::Local<v8::Object> object, kj::StringPtr name) {
  return jsg::check(object->Get(js.v8Context(), jsg::v8StrIntern(js.v8Isolate, name)));
}

uint requireEnvelopeInteger(
    v8::Local<v8::Value> value, uint minimum, uint maximum, kj::StringPtr message) {
  KJ_REQUIRE(value->IsNumber(), message);
  auto number = value.As<v8::Number>()->Value();
  KJ_REQUIRE(std::isfinite(number) && std::floor(number) == number && number >= minimum &&
          number <= maximum,
      message);
  return static_cast<uint>(number);
}

R2RpcBackendError parseEnvelopeBackendError(jsg::Lock& js, v8::Local<v8::Value> value) {
  KJ_REQUIRE(value->IsObject() && !value->IsArray(), "Malformed R2 RPC response error.");
  auto object = value.As<v8::Object>();
  auto code = requireEnvelopeInteger(getEnvelopeProperty(js, object, "v4code"_kj), 0,
      std::numeric_limits<uint>::max(), "Malformed R2 RPC response error code."_kj);
  auto message = getEnvelopeProperty(js, object, "message"_kj);
  KJ_REQUIRE(message->IsString(), "Malformed R2 RPC response error message.");
  return {.v4Code = code, .message = jsg::JsValue(message).toString(js)};
}

R2RpcEnvelope parseR2RpcEnvelope(jsg::Lock& js, jsg::Value raw) {
  auto value = raw.getHandle(js);
  KJ_REQUIRE(
      value->IsObject() && !value->IsArray() && !value->IsFunction(), "Malformed R2 RPC envelope.");
  auto object = value.As<v8::Object>();

  auto successValue = getEnvelopeProperty(js, object, "success"_kj);
  KJ_REQUIRE(successValue->IsBoolean(), "Malformed R2 RPC envelope success flag.");
  bool success = successValue->IsTrue();

  auto responseValue = getEnvelopeProperty(js, object, "response"_kj);
  KJ_REQUIRE(responseValue->IsObject() && !responseValue->IsArray(),
      "Malformed R2 RPC response information.");
  auto responseObject = responseValue.As<v8::Object>();
  auto httpStatus = requireEnvelopeInteger(getEnvelopeProperty(js, responseObject, "httpStatus"_kj),
      200, 599, "Malformed R2 RPC response status."_kj);
  auto responseErrorValue = getEnvelopeProperty(js, responseObject, "error"_kj);
  kj::Maybe<R2RpcBackendError> responseError;
  if (httpStatus >= 400) {
    responseError = parseEnvelopeBackendError(js, responseErrorValue);
  } else {
    KJ_REQUIRE(responseErrorValue->IsUndefined(), "Malformed R2 RPC response information.");
  }

  R2RpcResponseInfo response{.httpStatus = httpStatus, .error = kj::mv(responseError)};
  auto resultsValue = getEnvelopeProperty(js, object, "results"_kj);
  auto errorValue = getEnvelopeProperty(js, object, "error"_kj);
  if (success) {
    KJ_REQUIRE(!resultsValue->IsUndefined() && errorValue->IsUndefined(),
        "Malformed successful R2 RPC envelope.");
    return {.success = true,
      .response = kj::mv(response),
      .results = jsg::Value(js.v8Isolate, resultsValue),
      .error = kj::none};
  }

  KJ_REQUIRE(errorValue->IsNativeError() && resultsValue->IsUndefined(),
      "Malformed failed R2 RPC envelope.");
  return {.success = false,
    .response = kj::mv(response),
    .results = kj::none,
    .error = jsg::Value(js.v8Isolate, errorValue)};
}

void addResponseSpanTags(TraceContext& traceContext,
    uint httpStatus,
    kj::Maybe<uint> code,
    kj::Maybe<kj::StringPtr> message) {
  traceContext.setTag("cloudflare.r2.response.success"_kjc, httpStatus >= 200 && httpStatus < 400);
  KJ_IF_SOME(value, message) {
    traceContext.setTag("error.type"_kjc, value);
    traceContext.setTag("cloudflare.r2.error.message"_kjc, value);
  }
  KJ_IF_SOME(value, code) {
    traceContext.setTag("cloudflare.r2.error.code"_kjc, static_cast<int64_t>(value));
  }
}

}  // namespace

jsg::Promise<jsg::Value> normalizeR2RpcPromise(jsg::Lock& js, jsg::Value rpcPromise) {
  auto paf = js.newPromiseAndResolver<jsg::Value>();
  paf.resolver.resolve(js, kj::mv(rpcPromise));
  return kj::mv(paf.promise);
}

jsg::Promise<R2RpcEnvelope> unwrapR2RpcEnvelopePromise(jsg::Lock& js, jsg::Value rpcPromise) {
  return normalizeR2RpcPromise(js, kj::mv(rpcPromise)).then(js, [](jsg::Lock& js, jsg::Value raw) {
    return parseR2RpcEnvelope(js, kj::mv(raw));
  });
}

jsg::Promise<R2RpcEnvelope> unwrapR2RpcEnvelopePromise(jsg::Lock& js,
    jsg::Value rpcPromise,
    kj::Function<jsg::Promise<void>(jsg::Lock&, jsg::Value)> cleanup) {
  return normalizeR2RpcPromise(js, kj::mv(rpcPromise))
      .then(js,
          [cleanup = kj::mv(cleanup)](
              jsg::Lock& js, jsg::Value raw) mutable -> jsg::Promise<R2RpcEnvelope> {
    auto rawHandle = raw.getHandle(js);
    KJ_TRY {
      return js.resolvedPromise(parseR2RpcEnvelope(js, kj::mv(raw)));
    }
    KJ_CATCH(exception) {
      kj::Maybe<jsg::Promise<void>> cleanupPromise;
      KJ_TRY {
        cleanupPromise = cleanup(js, jsg::Value(js.v8Isolate, rawHandle));
      }
      KJ_CATCH(_) {
        cleanupPromise = js.resolvedPromise();
      }
      auto cleaned = kj::mv(cleanupPromise).orDefault(js.resolvedPromise());
      return cleaned.catch_(js, [](jsg::Lock&, jsg::Value) {
      }).then(js, [exception = kj::mv(exception)](jsg::Lock&) mutable -> R2RpcEnvelope {
        kj::throwRecoverableException(kj::mv(exception));
        KJ_UNREACHABLE;
      });
    }
  });
}

void addR2ResponseSpanTags(TraceContext& traceContext, const R2RpcResponseInfo& response) {
  kj::Maybe<uint> code;
  kj::Maybe<kj::StringPtr> message;
  KJ_IF_SOME(error, response.error) {
    code = error.v4Code;
    message = error.message.asPtr();
  }
  addResponseSpanTags(traceContext, response.httpStatus, code, message);
}

PreparedR2RpcBody prepareR2RpcBody(jsg::Lock& js, R2PutValue& value) {
  KJ_SWITCH_ONEOF(value) {
    KJ_CASE_ONEOF(stream, JsReadableStream) {
      auto size = stream.tryGetLength(js, StreamEncoding::IDENTITY);

      JSG_REQUIRE(size != kj::none, TypeError,
          "Provided readable stream must have a known length (request/response body or readable "
          "half of FixedLengthStream)");
      auto exactSize = KJ_ASSERT_NONNULL(size);
      JSG_REQUIRE(exactSize <= 9007199254740991ull, RangeError,
          "Provided readable stream is too large to represent its length exactly");
      return {.value = kj::mv(stream), .size = static_cast<double>(exactSize)};
    }
    KJ_CASE_ONEOF(data, kj::Array<byte>) {
      auto size = data.size();
      if (size > R2_RPC_INLINE_BODY_LIMIT) {
        // Type-wrapper arrays may alias V8 memory, which cannot be pumped without the isolate lock.
        auto owned = kj::heapArray<byte>(data.asPtr());
        auto view = owned.asPtr();
        return {.value = makeR2RpcMemoryStream(js, view, kj::heap(kj::mv(owned))),
          .size = static_cast<double>(size)};
      }
      return {.value = kj::mv(data), .size = static_cast<double>(size)};
    }
    KJ_CASE_ONEOF(text, jsg::NonCoercible<kj::String>) {
      auto size = text.value.size();
      if (size > R2_RPC_INLINE_BODY_LIMIT) {
        auto bytes = text.value.asBytes();
        return {.value = makeR2RpcMemoryStream(js, bytes, kj::heap(kj::mv(text.value))),
          .size = static_cast<double>(size)};
      }
      return {.value = kj::mv(text.value), .size = static_cast<double>(size)};
    }
    KJ_CASE_ONEOF(blob, jsg::Ref<Blob>) {
      auto size = blob->getSize();
      if (size > R2_RPC_INLINE_BODY_LIMIT) {
        // Blob bytes are V8-backed, so newMemorySource() must copy them into native memory.
        return {
          .value = makeR2RpcMemoryStream(js, blob->getData()), .size = static_cast<double>(size)};
      }
      return {.value = kj::mv(blob), .size = static_cast<double>(size)};
    }
  }
  KJ_UNREACHABLE;
}

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

kj::Maybe<kj::String> R2Result::getR2ErrorMessage() {
  KJ_IF_SOME(e, toThrow) {
    return kj::str(e->getMessage());
  }
  return kj::none;
}

void addR2ResponseSpanTags(TraceContext& traceContext, R2Result& r2Result) {
  auto message = r2Result.getR2ErrorMessage();
  addResponseSpanTags(traceContext, r2Result.httpStatus, r2Result.v4ErrorCode(),
      message.map([](const kj::String& value) { return value.asPtr(); }));
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

  auto request =
      client->request(kj::HttpMethod::GET, url, requestHeaders, static_cast<uint64_t>(0));

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

namespace {
// The coroutine half of doR2HTTPPutRequest(). Split out of the public function because
// computing the expected body size requires a jsg::Lock, and a jsg::Lock must never be
// captured in a KJ coroutine frame. Everything up to the first co_await runs synchronously
// in the caller's context (in particular, `path` is consumed before any suspension).
kj::Promise<R2Result> doR2HTTPPutRequestImpl(kj::Own<kj::HttpClient> client,
    kj::Maybe<R2PutValue> supportedBody,
    uint64_t expectedBodySize,
    kj::String metadataPayload,
    kj::ArrayPtr<kj::StringPtr> path,
    kj::Maybe<kj::StringPtr> jwt) {
  auto& context = IoContext::current();
  auto headers = kj::HttpHeaders(context.getHeaderTable());
  auto url = getFakeUrl(path);

  headers.set(context.getHeaderIds().cfBlobMetadataSize, kj::str(metadataPayload.size()));
  KJ_IF_SOME(j, jwt) {
    headers.set(context.getHeaderIds().authorization, kj::str("Bearer ", j));
  }

  uint64_t combinedSize = metadataPayload.size() + expectedBodySize;

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
      KJ_CASE_ONEOF(stream, JsReadableStream) {
        // Because the ReadableStream might be a fully JavaScript-backed stream, we must
        // start running the pump within the IoContext/isolate lock.
        co_await context.run(
            [dest = newSystemStream(kj::mv(request.body), StreamEncoding::IDENTITY, context),
                stream = kj::mv(stream)](jsg::Lock& js) mutable {
          return IoContext::current().waitForDeferredProxy(
              stream.pumpTo(js, kj::mv(dest), EndStream::YES));
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
}  // namespace

kj::Promise<R2Result> doR2HTTPPutRequest(jsg::Lock& js,
    kj::Own<kj::HttpClient> client,
    kj::Maybe<R2PutValue> supportedBody,
    kj::Maybe<uint64_t> streamSize,
    kj::String metadataPayload,
    kj::ArrayPtr<kj::StringPtr> path,
    kj::Maybe<kj::StringPtr> jwt) {
  // NOTE: A lot of code here is duplicated with kv.c++. Maybe it can be refactored to be more
  // reusable?
  kj::Maybe<uint64_t> expectedBodySize;

  KJ_IF_SOME(b, supportedBody) {
    KJ_SWITCH_ONEOF(b) {
      KJ_CASE_ONEOF(stream, JsReadableStream) {
        expectedBodySize = stream.tryGetLength(js, StreamEncoding::IDENTITY);
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
    expectedBodySize = static_cast<uint64_t>(0);
    KJ_REQUIRE(streamSize == kj::none);
  }

  return doR2HTTPPutRequestImpl(kj::mv(client), kj::mv(supportedBody),
      KJ_ASSERT_NONNULL(expectedBodySize), kj::mv(metadataPayload), path, kj::mv(jwt));
}
}  // namespace workerd::api
