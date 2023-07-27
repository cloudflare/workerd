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

v8::Local<v8::Value> R2Error::getStack(jsg::Lock& js) {
  auto stackString = jsg::v8StrIntern(js.v8Isolate, "stack");
  return jsg::check(KJ_ASSERT_NONNULL(errorForStack).Get(js.v8Isolate)->Get(
      js.v8Context(), stackString));
}

kj::Maybe<uint> R2Result::v4ErrorCode() {
  KJ_IF_MAYBE(e, toThrow) {
    return (*e)->v4Code;
  }
  return nullptr;
}

void R2Result::throwIfError(kj::StringPtr action,
    const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType) {
  KJ_IF_MAYBE(e, toThrow) {
    // TODO(soon): Once jsg::JsPromise exists, switch to using that to tunnel out the exception. As
    // it stands today, unfortunately, all we can send back to the user is a message. R2Error isn't
    // a registered type in the runtime. When reenabling, make sure to update overrides/r2.d.ts to
    // reenable the type
#if 0
    auto isolate = IoContext::current().getCurrentLock().getIsolate();
    (*e)->action = kj::str(action);
    (*e)->errorForStack = v8::Global<v8::Object>(
        isolate, v8::Exception::Error(jsg::v8Str(isolate, "")).As<v8::Object>());
    isolate->ThrowException(errorType.wrapRef(kj::mv(*e)));
    throw jsg::JsExceptionThrown();
#else
    JSG_FAIL_REQUIRE(Error, kj::str(
        action, ": ", (e)->get()->getMessage(), " (", (*e)->v4Code, ')'));
#endif
  }
}

kj::Promise<R2Result> doR2HTTPGetRequest(kj::Own<kj::HttpClient> client,
    kj::String metadataPayload, kj::ArrayPtr<kj::StringPtr> path, kj::Maybe<kj::StringPtr> jwt, CompatibilityFlags::Reader flags) {
  auto& context = IoContext::current();
  kj::Url url;


  url.scheme = kj::str("https");
  url.host = kj::str("fake-host");
  for (const auto &p : path) {
    url.path.add(kj::str(p));
  }

  auto& headerIds = context.getHeaderIds();

  auto requestHeaders = kj::HttpHeaders(context.getHeaderTable());
  requestHeaders.set(headerIds.cfBlobRequest, kj::mv(metadataPayload));
  KJ_IF_MAYBE(j, jwt) {
    requestHeaders.set(headerIds.authorization, kj::str("Bearer ", *j));
  }
  return client->request(
      kj::HttpMethod::GET, url.toString(kj::Url::Context::HTTP_PROXY_REQUEST),
      requestHeaders)
      .response.then([client = kj::mv(client), &context, &headerIds, flags]
                     (kj::HttpClient::Response&& response) mutable -> kj::Promise<R2Result> {
    auto processStream = [&](kj::StringPtr metadata) -> kj::Promise<R2Result> {
      auto stream = newSystemStream(
        response.body.attach(kj::mv(client)),
        getContentEncoding(context, *response.headers, Response::BodyEncoding::AUTO, flags),
        context);
      auto metadataSize = atoi((metadata).cStr());
      // R2 itself will try to stick to a cap of 256 KiB of response here. However for listing
      // sometimes our heuristics have corner cases. This way we're more lenient in case someone
      // finds a corner case for the heuristic so that we don't fail the GET with an opaque
      // internal error.
      KJ_REQUIRE(metadataSize <= 1024 * 1024, "R2 metadata size seems way too large");
      auto metadataBuffer = kj::heapArray<char>(metadataSize);
      auto promise = stream->tryRead((void*)metadataBuffer.begin(),
          metadataBuffer.size(), metadataBuffer.size());
      return promise.then([metadataBuffer = kj::mv(metadataBuffer),
          stream = kj::mv(stream), response = kj::mv(response)]
          (size_t metadataReadLength) mutable -> kj::Promise<R2Result> {
        KJ_ASSERT(metadataReadLength == metadataBuffer.size(),
            "R2 metadata buffer not read fully/overflow?");
        return R2Result {
          .httpStatus = response.statusCode,
          .metadataPayload = kj::mv(metadataBuffer),
          .stream = kj::mv(stream)
        };
      });
    };

    if (response.statusCode >= 400) {
      // Error responses should have a cfR2ErrorHeader but don't always. If there
      // isn't one, we'll use a generic error.
      if (response.headers->get(headerIds.cfR2ErrorHeader) == nullptr) {
        LOG_WARNING_ONCE("R2 error response does not contain the CF-R2-Error header.",
                         response.statusCode);
      }
      auto error = response.headers->get(headerIds.cfR2ErrorHeader).orDefault(
          "{\"version\":0,\"v4code\":0,\"message\":\"Unspecified error\"}"_kj);
      R2Result result = {
        .httpStatus = response.statusCode,
        .toThrow = toError(response.statusCode, error),
      };

      KJ_IF_MAYBE(m, response.headers->get(headerIds.cfBlobMetadataSize)) {
        return processStream(*m).then([result = kj::mv(result)](R2Result processed) mutable {
          result.metadataPayload = kj::mv(processed.metadataPayload);
          result.stream = kj::mv(processed.stream);
          return kj::mv(result);
        });
      }

      return kj::mv(result);
    }

    KJ_IF_MAYBE(m, response.headers->get(headerIds.cfBlobMetadataSize)) {
      return processStream(*m);
    } else {
      return R2Result { .httpStatus = response.statusCode };
    }
  });
}

kj::Promise<R2Result> doR2HTTPPutRequest(jsg::Lock& js, kj::Own<kj::HttpClient> client,
    kj::Maybe<R2PutValue> supportedBody, kj::Maybe<uint64_t> streamSize, kj::String metadataPayload,
    kj::ArrayPtr<kj::StringPtr> path, kj::Maybe<kj::StringPtr> jwt) {
  // NOTE: A lot of code here is duplicated with kv.c++. Maybe it can be refactored to be more
  // reusable?
  auto& context = IoContext::current();
  auto headers = kj::HttpHeaders(context.getHeaderTable());
  kj::Url url;
  url.scheme = kj::str("https");
  url.host = kj::str("fake-host");
  for (const auto &p : path) {
    url.path.add(kj::str(p));
  }

  kj::Maybe<uint64_t> expectedBodySize;

  KJ_IF_MAYBE(b, supportedBody) {
    KJ_SWITCH_ONEOF(*b) {
      KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
        expectedBodySize = stream->tryGetLength(StreamEncoding::IDENTITY);
        if (expectedBodySize == nullptr) {
          expectedBodySize = streamSize;
        }
        JSG_REQUIRE(expectedBodySize != nullptr, TypeError,
            "Provided readable stream must have a known length (request/response body or readable "
            "half of FixedLengthStream)");
        JSG_REQUIRE(streamSize.orDefault(KJ_ASSERT_NONNULL(expectedBodySize)) == expectedBodySize,
            RangeError, "Provided stream length (", streamSize.orDefault(-1), ") doesn't match what "
            "the stream reports (", KJ_ASSERT_NONNULL(expectedBodySize), ")");
      }
      KJ_CASE_ONEOF(text, jsg::NonCoercible<kj::String>) {
        expectedBodySize = text.value.size();
        KJ_REQUIRE(streamSize == nullptr);
      }
      KJ_CASE_ONEOF(data, kj::Array<kj::byte>) {
        expectedBodySize = data.size();
        KJ_REQUIRE(streamSize == nullptr);
      }
      KJ_CASE_ONEOF(data, jsg::Ref<Blob>) {
        expectedBodySize = data->getSize();
        KJ_REQUIRE(streamSize == nullptr);
      }
    }
  } else {
    expectedBodySize = uint64_t(0);
    KJ_REQUIRE(streamSize == nullptr);
  }

  headers.set(context.getHeaderIds().cfBlobMetadataSize, kj::str(metadataPayload.size()));
  KJ_IF_MAYBE(j, jwt) {
    headers.set(context.getHeaderIds().authorization, kj::str("Bearer ", *j));
  }

  auto urlStr = url.toString(kj::Url::Context::HTTP_PROXY_REQUEST);

  return context.waitForOutputLocks()
      .then([&context, client = kj::mv(client), urlStr = kj::mv(urlStr),
      metadataPayload = kj::mv(metadataPayload), headers = kj::mv(headers),
      expectedBodySize, supportedBody = kj::mv(supportedBody)]
      () mutable {
    uint64_t combinedSize = metadataPayload.size() + KJ_ASSERT_NONNULL(expectedBodySize);
    auto innerReq = client->request(kj::HttpMethod::PUT, urlStr, headers, combinedSize);

    struct RefcountedWrapper: public kj::Refcounted {
      explicit RefcountedWrapper(kj::Own<kj::HttpClient> client): client(kj::mv(client)) {}
      kj::Own<kj::HttpClient> client;
    };
    auto rcClient = kj::refcounted<RefcountedWrapper>(kj::mv(client));
    // TODO(perf): More efficient to explicitly attach rcClient below?
    auto req = attachToRequest(kj::mv(innerReq), kj::mv(rcClient));

    return req.body->write(metadataPayload.begin(), metadataPayload.size())
        .attach(kj::mv(metadataPayload))
        .then([&context, supportedBody = kj::mv(supportedBody),
               reqBody = kj::mv(req.body)]() mutable -> kj::Promise<void> {
      KJ_IF_MAYBE(b, supportedBody) {
        KJ_SWITCH_ONEOF(*b) {
          KJ_CASE_ONEOF(text, jsg::NonCoercible<kj::String>) {
            return reqBody->write(text.value.begin(), text.value.size())
                .attach(kj::mv(text.value), kj::mv(reqBody));
          }
          KJ_CASE_ONEOF(data, kj::Array<byte>) {
            return reqBody->write(data.begin(), data.size())
                .attach(kj::mv(data), kj::mv(reqBody));
          }
          KJ_CASE_ONEOF(blob, jsg::Ref<Blob>) {
            auto data = blob->getData();
            return reqBody->write(data.begin(), data.size());
          }
          KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
            // Because the ReadableStream might be a fully JavaScript-backed stream, we must
            // start running the pump within the IoContex/isolate lock.
            return context.run(
                [dest = newSystemStream(kj::mv(reqBody), StreamEncoding::IDENTITY, context),
                 stream = kj::mv(stream)](jsg::Lock& js) mutable {
              return IoContext::current().waitForDeferredProxy(
                  stream->pumpTo(js, kj::mv(dest), true));
            });
          }
        }

        KJ_UNREACHABLE;
      }

      return kj::READY_NOW;
    }).then([resp = kj::mv(req.response),&context]() mutable {
      return resp.then([&context](kj::HttpClient::Response&& response) mutable
        -> kj::Promise<R2Result> {
        if (response.statusCode >= 400) {
          // Error responses should have a cfR2ErrorHeader but don't always. If there
          // isn't one, we'll use a generic error.
          auto& headerIds = context.getHeaderIds();
          if (response.headers->get(headerIds.cfR2ErrorHeader) == nullptr) {
            LOG_WARNING_ONCE("R2 error response does not contain the CF-R2-Error header.",
                            response.statusCode);
          }
          auto error = response.headers->get(headerIds.cfR2ErrorHeader).orDefault(
              "{\"version\":0,\"v4code\":0,\"message\":\"Unspecified error\"}"_kj);

          return R2Result {
            .httpStatus = response.statusCode,
            .toThrow = toError(response.statusCode, error),
          };
        }

        return response.body->readAllText().attach(kj::mv(response.body)).then(
            [statusCode = response.statusCode, statusText = kj::str(response.statusText)]
            (kj::String responseBody) mutable -> R2Result {
          return R2Result{
            .httpStatus = statusCode,
            .metadataPayload = responseBody.releaseArray(),
          };
        });
      });
    });
  });
}
}  // namespace workerd::api
