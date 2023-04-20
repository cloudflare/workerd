// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "kv.h"
#include "util.h"
#include "system-streams.h"
#include "workerd/io/limit-enforcer.h"
#include <workerd/util/http-util.h>
#include <workerd/io/io-context.h>
#include <kj/encoding.h>
#include <kj/compat/http.h>

namespace workerd::api {

// As documented in Cloudflare's Worker KV limits.
static constexpr size_t kMaxKeyLength = 512;

static void checkForErrorStatus(kj::StringPtr method, const kj::HttpClient::Response& response) {
  if (response.statusCode < 200 || response.statusCode >= 300) {
    // Manually construct exception so that we can incorporate method and status into the text
    // that JavaScript sees.
    kj::throwFatalException(kj::Exception(kj::Exception::Type::FAILED, __FILE__, __LINE__,
        kj::str(JSG_EXCEPTION(Error) ": KV ", method, " failed: ",
            response.statusCode, ' ', response.statusText)));
  }
}

static void validateKeyName(kj::StringPtr method, kj::StringPtr name) {
  JSG_REQUIRE(name != "", TypeError, "Key name cannot be empty.");
  JSG_REQUIRE(name != ".", TypeError, "\".\" is not allowed as a key name.");
  JSG_REQUIRE(name != "..", TypeError, "\"..\" is not allowed as a key name.");
  JSG_REQUIRE(name.size() <= kMaxKeyLength, Error, "KV ", method, " failed: ",
      414, " UTF-8 encoded length of ", name.size(), " exceeds key length limit of ", kMaxKeyLength, ".");
}

static void parseListMetadata(jsg::Lock& js, v8::Local<v8::Value> listResponse) {
  auto isolate = js.v8Isolate;
  v8::HandleScope handleScope(isolate);
  KJ_ASSERT(listResponse->IsObject());
  v8::Local<v8::Object> obj = listResponse.As<v8::Object>();
  auto context = js.v8Context();
  auto keyName = jsg::v8Str(isolate, "keys"_kj);
  auto keys = jsg::check(obj->Get(context, keyName));
  if (keys->IsArray()) {
    auto keysArr = keys.As<v8::Array>();
    auto length = keysArr->Length();
    auto metaName = jsg::v8Str(isolate, "metadata"_kj);
    v8::Local<v8::Object> key;
    for (int i = 0; i < length; i++) {
      v8::HandleScope handleScope(isolate);
      key = jsg::check(keysArr->Get(context, i)).As<v8::Object>();
      if (jsg::check(key->HasOwnProperty(context, metaName))) {
        auto metadata = jsg::check(key->Get(context, metaName));
        KJ_ASSERT(metadata->IsString());
        auto metadataStr = metadata.As<v8::String>();
        auto json = jsg::check(v8::JSON::Parse(context, metadataStr));
        jsg::check(key->Set(context, metaName, json));
      }
    }
  }
}

constexpr auto FLPROD_405_HEADER = "CF-KV-FLPROD-405"_kj;

kj::Own<kj::HttpClient> KvNamespace::getHttpClient(
    IoContext& context,
    kj::HttpHeaders& headers,
    kj::OneOf<LimitEnforcer::KvOpType, kj::StringPtr> opTypeOrUnknown,
    kj::StringPtr urlStr) {
  const auto operationName = [&] {
    KJ_SWITCH_ONEOF(opTypeOrUnknown) {
      KJ_CASE_ONEOF(name, kj::StringPtr) {
        return name;
      }
      KJ_CASE_ONEOF(opType, LimitEnforcer::KvOpType) {
        // Check if we've hit KV usage limits. (This will throw if we have.)
        context.getLimitEnforcer().newKvRequest(opType);

        switch (opType) {
          case LimitEnforcer::KvOpType::GET:
            return "kv_get"_kj;
          case LimitEnforcer::KvOpType::PUT:
            return "kv_put"_kj;
          case LimitEnforcer::KvOpType::LIST:
            return "kv_list"_kj;
          case LimitEnforcer::KvOpType::DELETE:
            return "kv_delete"_kj;
        }
      }
    }

    KJ_UNREACHABLE;
  }();

  auto client = context.getHttpClient(subrequestChannel, true, nullptr, operationName);
  headers.add(FLPROD_405_HEADER, urlStr);
  for (const auto& header: additionalHeaders) {
    headers.add(header.name.asPtr(), header.value.asPtr());
  }

  return client;
}


jsg::Promise<KvNamespace::GetResult> KvNamespace::get(
    jsg::Lock& js, kj::String name, jsg::Optional<kj::OneOf<kj::String, GetOptions>> options) {
  return js.evalNow([&] {
    auto resp = getWithMetadata(js, kj::mv(name), kj::mv(options));
    return resp.then([](KvNamespace::GetWithMetadataResult result) {
      return kj::mv(result.value);
    });
  });
}

jsg::Promise<KvNamespace::GetWithMetadataResult> KvNamespace::getWithMetadata(
    jsg::Lock& js, kj::String name, jsg::Optional<kj::OneOf<kj::String, GetOptions>> options) {
  validateKeyName("GET", name);

  auto& context = IoContext::current();

  kj::Url url;
  url.scheme = kj::str("https");
  url.host = kj::str("fake-host");
  url.path.add(kj::mv(name));
  url.query.add(kj::Url::QueryParam { kj::str("urlencoded"), kj::str("true") });

  kj::Maybe<kj::String> type;
  KJ_IF_MAYBE(oneOfOptions, options) {
    KJ_SWITCH_ONEOF(*oneOfOptions) {
      KJ_CASE_ONEOF(t, kj::String) {
        type = kj::mv(t);
      }
      KJ_CASE_ONEOF(options, GetOptions) {
        KJ_IF_MAYBE(t, options.type) {
          type = kj::mv(*t);
        }
        KJ_IF_MAYBE(cacheTtl, options.cacheTtl) {
          url.query.add(kj::Url::QueryParam { kj::str("cache_ttl"), kj::str(*cacheTtl) });
        }
      }
    }
  }

  auto urlStr = url.toString(kj::Url::Context::HTTP_PROXY_REQUEST);

  auto headers = kj::HttpHeaders(context.getHeaderTable());
  auto client = getHttpClient(context, headers, LimitEnforcer::KvOpType::GET, urlStr);

  auto request = client->request(kj::HttpMethod::GET, urlStr, headers);
  return context.awaitIo(js,
      kj::mv(request.response),
      [type = kj::mv(type), &context, client = kj::mv(client)]
          (jsg::Lock& js, kj::HttpClient::Response&& response) mutable
          -> jsg::Promise<KvNamespace::GetWithMetadataResult> {

    if (response.statusCode == 404 || response.statusCode == 410) {
      return js.resolvedPromise(KvNamespace::GetWithMetadataResult {
        .value = nullptr,
        .metadata = nullptr,
      });
    }

    checkForErrorStatus("GET", response);

    auto metaheader = response.headers->get(context.getHeaderIds().cfKvMetadata);
    kj::Maybe<kj::String> maybeMeta = nullptr;
    KJ_IF_MAYBE(m, metaheader) {
      maybeMeta = kj::str(*m);
    }

    auto typeName =
        type.map([](const kj::String& s) -> kj::StringPtr { return s; })
            .orDefault("text");

    auto& context = IoContext::current();
    auto stream = newSystemStream(
        response.body.attach(kj::mv(client)), getContentEncoding(context, *response.headers));

    jsg::Promise<KvNamespace::GetResult> result = nullptr;

    if (typeName == "stream") {
      result = js.resolvedPromise(KvNamespace::GetResult(
          jsg::alloc<ReadableStream>(context, kj::mv(stream))));
    } else if (typeName == "text") {
      // NOTE: In theory we should be using awaitIoLegacy() here since ReadableStreamSource is
      //   supposed to handle pending events on its own, but we also know that the HTTP client
      //   backing a KV namepsace is never implemented in local JavaScript, so whatever.
      result = context.awaitIo(
          stream->readAllText(context.getLimitEnforcer().getBufferingLimit())
              .attach(kj::mv(stream)),
          [](kj::String text) {
        return KvNamespace::GetResult(kj::mv(text));
      });
    } else if (typeName == "arrayBuffer") {
      result = context.awaitIo(
          stream->readAllBytes(context.getLimitEnforcer().getBufferingLimit())
              .attach(kj::mv(stream)),
          [](kj::Array<byte> text) {
        return KvNamespace::GetResult(kj::mv(text));
      });
    } else if (typeName == "json") {
      result = context.awaitIo(js,
          stream->readAllText(context.getLimitEnforcer().getBufferingLimit())
              .attach(kj::mv(stream)),
          [](jsg::Lock& js, kj::String text) {
        return KvNamespace::GetResult(js.parseJson(text));
      });
    } else {
      JSG_FAIL_REQUIRE(TypeError,
          "Unknown response type. Possible types are \"text\", \"arrayBuffer\", "
          "\"json\", and \"stream\".");
    }
    return result.then(js,
        [maybeMeta = kj::mv(maybeMeta)](jsg::Lock& js, KvNamespace::GetResult result)
            -> KvNamespace::GetWithMetadataResult {
      kj::Maybe<jsg::Value> meta;
      KJ_IF_MAYBE (metaStr, maybeMeta) {
        meta = js.parseJson(*metaStr);
      }
      return KvNamespace::GetWithMetadataResult{kj::mv(result), kj::mv(meta)};
    });
  });
}

jsg::Promise<jsg::Value> KvNamespace::list(
    jsg::Lock& js, jsg::Optional<ListOptions> options) {
  return js.evalNow([&] {
    auto& context = IoContext::current();

    kj::Url url;
    url.scheme = kj::str("https");
    url.host = kj::str("fake-host");
    KJ_IF_MAYBE(o, options) {
      KJ_IF_MAYBE(limit, o->limit) {
        if (*limit > 0) {
          url.query.add(kj::Url::QueryParam { kj::str("key_count_limit"), kj::str(*limit) });
        }
      }
      KJ_IF_MAYBE(maybePrefix, o->prefix) {
        KJ_IF_MAYBE(prefix, *maybePrefix) {
          url.query.add(kj::Url::QueryParam { kj::str("prefix"), kj::str(*prefix) });
        }
      }
      KJ_IF_MAYBE(maybeCursor, o->cursor) {
        KJ_IF_MAYBE(cursor, *maybeCursor) {
          url.query.add(kj::Url::QueryParam { kj::str("cursor"), kj::str(*cursor) });
        }
      }
    }

    auto urlStr = url.toString(kj::Url::Context::HTTP_PROXY_REQUEST);

    auto headers = kj::HttpHeaders(context.getHeaderTable());
    auto client = getHttpClient(context, headers, LimitEnforcer::KvOpType::LIST, urlStr);

    auto request = client->request(kj::HttpMethod::GET, urlStr, headers);
    return context.awaitIo(js,
        kj::mv(request.response),
        [&context, client = kj::mv(client)]
        (jsg::Lock& js, kj::HttpClient::Response&& response) mutable
            -> jsg::Promise<jsg::Value> {

      checkForErrorStatus("GET", response);

      auto stream = newSystemStream(
          response.body.attach(kj::mv(client)), getContentEncoding(context, *response.headers));

      return context.awaitIo(js,
          stream->readAllText(context.getLimitEnforcer().getBufferingLimit())
              .attach(kj::mv(stream)),
          [](jsg::Lock& js, kj::String text) {
        auto result = js.parseJson(text);
        parseListMetadata(js, result.getHandle(js.v8Isolate));
        return result;
      });
    });
  });
}

jsg::Promise<void> KvNamespace::put(
    jsg::Lock& js,
    kj::String name,
    KvNamespace::PutBody body,
    jsg::Optional<PutOptions> options,
    const jsg::TypeHandler<KvNamespace::PutSupportedTypes>& putTypeHandler) {
  return js.evalNow([&] {
    validateKeyName("PUT", name);

    auto& context = IoContext::current();

    kj::Url url;
    url.scheme = kj::str("https");
    url.host = kj::str("fake-host");
    url.path.add(kj::mv(name));
    url.query.add(kj::Url::QueryParam { kj::str("urlencoded"), kj::str("true") });

    kj::HttpHeaders headers(context.getHeaderTable());

    // If any optional parameters were specified by the client, append them to
    // the URL's query parameters.
    KJ_IF_MAYBE(o, options) {
      KJ_IF_MAYBE(expiration, o->expiration) {
        url.query.add(kj::Url::QueryParam { kj::str("expiration"), kj::str(*expiration) });
      }
      KJ_IF_MAYBE(expirationTtl, o->expirationTtl) {
        url.query.add(kj::Url::QueryParam { kj::str("expiration_ttl"), kj::str(*expirationTtl) });
      }
      KJ_IF_MAYBE(maybeMetadata, o->metadata) {
        KJ_IF_MAYBE(metadata, *maybeMetadata) {
          kj::String json = js.serializeJson(*metadata);
          headers.set(context.getHeaderIds().cfKvMetadata, kj::mv(json));
        }
      }
    }

    PutSupportedTypes supportedBody;

    KJ_SWITCH_ONEOF(body) {
      KJ_CASE_ONEOF(text, kj::String) {
        supportedBody = kj::mv(text);
      }
      KJ_CASE_ONEOF(object, v8::Local<v8::Object>) {
        supportedBody = JSG_REQUIRE_NONNULL(putTypeHandler.tryUnwrap(js, object),
            TypeError, "KV put() accepts only strings, ArrayBuffers, ArrayBufferViews, and "
            "ReadableStreams as values.");
        JSG_REQUIRE(!supportedBody.is<kj::String>(),
            TypeError, "KV put() accepts only strings, ArrayBuffers, ArrayBufferViews, and "
            "ReadableStreams as values.");
        // TODO(someday): replace this with logic to do something smarter with Objects
      }
    }

    kj::Maybe<uint64_t> expectedBodySize;

    KJ_SWITCH_ONEOF(supportedBody) {
      KJ_CASE_ONEOF(text, kj::String) {
        headers.set(kj::HttpHeaderId::CONTENT_TYPE, "text/plain;charset=UTF-8");
        expectedBodySize = uint64_t(text.size());
      }
      KJ_CASE_ONEOF(data, kj::Array<byte>) {
        expectedBodySize = uint64_t(data.size());
      }
      KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
        expectedBodySize = stream->tryGetLength(StreamEncoding::IDENTITY);
      }
    }

    auto urlStr = url.toString(kj::Url::Context::HTTP_PROXY_REQUEST);

    auto client = getHttpClient(context, headers, LimitEnforcer::KvOpType::PUT, urlStr);

    auto promise = context.waitForOutputLocks()
        .then([&context, client = kj::mv(client), urlStr = kj::mv(urlStr),
               headers = kj::mv(headers), expectedBodySize,
               supportedBody = kj::mv(supportedBody)]() mutable {
      auto innerReq = client->request(
          kj::HttpMethod::PUT, urlStr,
          headers, expectedBodySize);
      struct RefcountedWrapper: public kj::Refcounted {
        explicit RefcountedWrapper(kj::Own<kj::HttpClient> client): client(kj::mv(client)) {}
        kj::Own<kj::HttpClient> client;
      };
      auto rcClient = kj::refcounted<RefcountedWrapper>(kj::mv(client));
      // TODO(perf): More efficient to explicitly attach rcClient below?
      auto req = attachToRequest(kj::mv(innerReq), kj::mv(rcClient));

      kj::Promise<void> writePromise = nullptr;
      KJ_SWITCH_ONEOF(supportedBody) {
        KJ_CASE_ONEOF(text, kj::String) {
          writePromise = req.body->write(text.begin(), text.size()).attach(kj::mv(text));
        }
        KJ_CASE_ONEOF(data, kj::Array<byte>) {
          writePromise = req.body->write(data.begin(), data.size()).attach(kj::mv(data));
        }
        KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
          writePromise = context.run([
              dest = newSystemStream(kj::mv(req.body), StreamEncoding::IDENTITY, context),
              stream = kj::mv(stream)](jsg::Lock& js) mutable {
            return IoContext::current().waitForDeferredProxy(
                stream->pumpTo(js, kj::mv(dest), true));
          });
        }
      }

      return writePromise.attach(kj::mv(req.body))
          .then([resp = kj::mv(req.response)]() mutable {
        return resp.then([](kj::HttpClient::Response&& response) mutable {
          checkForErrorStatus("PUT", response);

          // Read and discard response body, otherwise we might burn the HTTP connection.
          return response.body->readAllBytes().attach(kj::mv(response.body)).ignoreResult();
        });
      });
    });

    return context.awaitIo(js, kj::mv(promise));
  });
}

jsg::Promise<void> KvNamespace::delete_(jsg::Lock& js, kj::String name) {
  return js.evalNow([&] {
    validateKeyName("DELETE", name);

    auto& context = IoContext::current();

    auto urlStr = kj::str("https://fake-host/", kj::encodeUriComponent(name), "?urlencoded=true");

    kj::HttpHeaders headers(context.getHeaderTable());

    auto client = getHttpClient(context, headers, LimitEnforcer::KvOpType::DELETE, urlStr);

    auto promise = context.waitForOutputLocks()
        .then([headers = kj::mv(headers), client = kj::mv(client), urlStr = kj::mv(urlStr)]() mutable {
      return client->request(kj::HttpMethod::DELETE, urlStr, headers,
                            uint64_t(0))
          .response.then([](kj::HttpClient::Response&& response) mutable {
        checkForErrorStatus("DELETE", response);
      }).attach(kj::mv(client));
    });

    return context.awaitIo(js, kj::mv(promise));
  });
}

}  // namespace workerd::api
