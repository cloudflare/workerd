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

v8::Local<v8::Value> R2Error::getStack(v8::Isolate* isolate) {
  auto stackString = jsg::v8Str(isolate, "stack", v8::NewStringType::kInternalized);
  return jsg::check(KJ_ASSERT_NONNULL(errorForStack).Get(isolate)->Get(
      isolate->GetCurrentContext(), stackString));
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
    kj::String metadataPayload, kj::Maybe<kj::String> path) {
  auto& context = IoContext::current();
  kj::Url url;
  url.scheme = kj::str("https");
  url.host = kj::str("fake-host");
  KJ_IF_MAYBE(p, path) {
    url.path.add(kj::mv(*p));
  }

  auto& headerIds = context.getHeaderIds();

  auto requestHeaders = kj::HttpHeaders(context.getHeaderTable());
  requestHeaders.set(headerIds.cfBlobRequest, kj::mv(metadataPayload));
  return client->request(
      kj::HttpMethod::GET, url.toString(kj::Url::Context::HTTP_PROXY_REQUEST),
      requestHeaders)
      .response.then([client = kj::mv(client), &context, &headerIds]
                     (kj::HttpClient::Response&& response) mutable -> kj::Promise<R2Result> {
    auto processStream = [&](kj::StringPtr metadata) -> kj::Promise<R2Result> {
      auto stream = newSystemStream(
        response.body.attach(kj::mv(client)), getContentEncoding(context, *response.headers),
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
      auto error = KJ_REQUIRE_NONNULL(response.headers->get(headerIds.cfR2ErrorHeader));
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
    kj::Maybe<kj::String> path) {
  // NOTE: A lot of code here is duplicated with kv.c++. Maybe it can be refactored to be more
  // reusable?
  auto& context = IoContext::current();
  auto headers = kj::HttpHeaders(context.getHeaderTable());
  kj::Url url;
  url.scheme = kj::str("https");
  url.host = kj::str("fake-host");
  KJ_IF_MAYBE(p, path) {
    url.path.add(kj::mv(*p));
  }

  kj::Maybe<uint64_t> expectedBodySize;
  kj::Own<ReadableStreamSource> streamSource;

  KJ_IF_MAYBE(b, supportedBody) {
    KJ_SWITCH_ONEOF(*b) {
      KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
        streamSource = stream->removeSource(js);
        expectedBodySize = streamSource->tryGetLength(StreamEncoding::IDENTITY);
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

  auto urlStr = url.toString(kj::Url::Context::HTTP_PROXY_REQUEST);

  return context.waitForOutputLocks()
      .then([&context, client = kj::mv(client), urlStr = kj::mv(urlStr),
      metadataPayload = kj::mv(metadataPayload), headers = kj::mv(headers),
      streamSource = kj::mv(streamSource), expectedBodySize, supportedBody = kj::mv(supportedBody)]
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
        .then([&context, supportedBody = kj::mv(supportedBody), streamSource = kj::mv(streamSource),
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
            auto dest = newSystemStream(kj::mv(reqBody), StreamEncoding::IDENTITY, context);
            return context.waitForDeferredProxy(streamSource->pumpTo(*dest, true))
                .attach(kj::mv(streamSource), kj::mv(dest));
          }
        }

        KJ_UNREACHABLE;
      }

      return kj::READY_NOW;
    }).then([resp = kj::mv(req.response),&context]() mutable {
      return resp.then([&context](kj::HttpClient::Response&& response) mutable
        -> kj::Promise<R2Result> {
        if (response.statusCode >= 400) {
          auto error = KJ_ASSERT_NONNULL(response.headers->get(
              context.getHeaderIds().cfR2ErrorHeader));

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
