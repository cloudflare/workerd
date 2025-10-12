// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "kv.h"

#include "system-streams.h"
#include "util.h"

#include <workerd/io/features.h>
#include <workerd/io/io-context.h>
#include <workerd/io/limit-enforcer.h>
#include <workerd/util/http-util.h>
#include <workerd/util/mimetype.h>

#include <kj/compat/http.h>
#include <kj/encoding.h>

namespace workerd::api {

// As documented in Cloudflare's Worker KV limits.
static constexpr size_t kMaxKeyLength = 512;

static void checkForErrorStatus(kj::StringPtr method, const kj::HttpClient::Response& response) {
  if (response.statusCode < 200 || response.statusCode >= 300) {
    // Manually construct exception so that we can incorporate method and status into the text
    // that JavaScript sees.
    kj::throwFatalException(kj::Exception(kj::Exception::Type::FAILED, __FILE__, __LINE__,
        kj::str(JSG_EXCEPTION(Error) ": KV ", method, " failed: ", response.statusCode, ' ',
            response.statusText)));
  }
}

static void validateKeyName(kj::StringPtr method, kj::StringPtr name) {
  JSG_REQUIRE(name != "", TypeError, "Key name cannot be empty.");
  JSG_REQUIRE(name != ".", TypeError, "\".\" is not allowed as a key name.");
  JSG_REQUIRE(name != "..", TypeError, "\"..\" is not allowed as a key name.");
  JSG_REQUIRE(name.size() <= kMaxKeyLength, Error, "KV ", method, " failed: ", 414,
      " UTF-8 encoded length of ", name.size(), " exceeds key length limit of ", kMaxKeyLength,
      ".");
}

static void parseListMetadata(TraceContext& traceContext,
    jsg::Lock& js,
    jsg::JsValue listResponse,
    kj::Maybe<jsg::JsValue> cacheStatus) {
  static constexpr auto METADATA = "metadata"_kjc;
  static constexpr auto KEYS = "keys"_kjc;
  static constexpr auto CURSOR = "cursor"_kjc;
  static constexpr auto LIST_COMPLETE = "list_complete"_kjc;
  static constexpr auto EXPIRATION = "expiration"_kjc;

  js.withinHandleScope([&] {
    auto obj = KJ_ASSERT_NONNULL(listResponse.tryCast<jsg::JsObject>());

    KJ_IF_SOME(boolVal, obj.get(js, LIST_COMPLETE).tryCast<jsg::JsBoolean>()) {
      traceContext.userSpan.setTag("cloudflare.kv.response.list_complete"_kjc, boolVal.value(js));
    }

    KJ_IF_SOME(cursor, obj.get(js, CURSOR).tryCast<jsg::JsString>()) {
      traceContext.userSpan.setTag("cloudflare.kv.response.cursor"_kjc, kj::str(cursor));
    }

    KJ_IF_SOME(expiration, obj.get(js, EXPIRATION).tryCast<jsg::JsNumber>()) {
      KJ_IF_SOME(value, expiration.value(js)) {
        traceContext.userSpan.setTag("cloudflare.kv.response.expiration"_kjc, int64_t(value));
      }
    }

    KJ_IF_SOME(keysArr, obj.get(js, KEYS).tryCast<jsg::JsArray>()) {
      auto length = keysArr.size();
      traceContext.userSpan.setTag(
          "cloudflare.kv.response.returned_rows"_kjc, static_cast<int64_t>(length));
      for (int i = 0; i < length; i++) {
        js.withinHandleScope([&] {
          KJ_IF_SOME(key, keysArr.get(js, i).tryCast<jsg::JsObject>()) {
            KJ_IF_SOME(str, key.get(js, METADATA).tryCast<jsg::JsString>()) {
              key.set(js, METADATA, jsg::JsValue::fromJson(js, str));
            }
          }
        });
      }
    }

    obj.set(js, "cacheStatus"_kjc, cacheStatus.orDefault(js.null()));
  });
}

constexpr auto FLPROD_405_HEADER = "CF-KV-FLPROD-405"_kj;

kj::Own<kj::HttpClient> KvNamespace::getHttpClient(IoContext& context,
    kj::HttpHeaders& headers,
    kj::OneOf<LimitEnforcer::KvOpType, kj::LiteralStringConst> opTypeOrName,
    kj::StringPtr urlStr,
    TraceContext& traceContext) {

  KJ_SWITCH_ONEOF(opTypeOrName) {
    KJ_CASE_ONEOF(name, kj::LiteralStringConst) {}
    KJ_CASE_ONEOF(opType, LimitEnforcer::KvOpType) {
      // Check if we've hit KV usage limits. (This will throw if we have.)
      context.getLimitEnforcer().newKvRequest(opType);
    }
  }

  auto client = context.getHttpClient(subrequestChannel, true, kj::none, traceContext);

  headers.addPtrPtr(FLPROD_405_HEADER, urlStr);
  for (const auto& header: additionalHeaders) {
    headers.addPtrPtr(header.name.asPtr(), header.value.asPtr());
  }

  return client;
}

jsg::Promise<KvNamespace::GetResult> KvNamespace::getSingle(jsg::Lock& js,
    IoContext& context,
    TraceContext& traceContext,
    kj::String name,
    jsg::Optional<kj::OneOf<kj::String, GetOptions>> options) {
  return js.evalNow([&] {
    auto resp = getWithMetadataImpl(
        js, context, traceContext, kj::mv(name), kj::mv(options), LimitEnforcer::KvOpType::GET);
    return resp.then(js,
        [](jsg::Lock&, KvNamespace::GetWithMetadataResult result) { return kj::mv(result.value); });
  });
}

jsg::Promise<jsg::JsRef<jsg::JsMap>> KvNamespace::getBulk(jsg::Lock& js,
    IoContext& context,
    TraceContext& traceContext,
    kj::Array<kj::String> name,
    jsg::Optional<kj::OneOf<kj::String, GetOptions>> options,
    bool withMetadata) {
  return js.evalNow([&] {
    kj::Url url;
    url.scheme = kj::str("https");
    url.host = kj::str("fake-host");
    url.path.add(kj::str("bulk"));
    url.path.add(kj::str("get"));

    kj::String body = formBulkBodyString(js, name, withMetadata, options);
    kj::Maybe<uint64_t> expectedBodySize = uint64_t(body.size());
    auto headers = kj::HttpHeaders(context.getHeaderTable());
    headers.set(kj::HttpHeaderId::CONTENT_TYPE, MimeType::JSON.toString());

    auto urlStr = url.toString(kj::Url::Context::HTTP_PROXY_REQUEST);

    // This could be quite large, so let's limit the string length to 512 characters
    auto keysStr = kj::strArray(name, ", ");
    if (keysStr.size() > 512) {
      keysStr = kj::str(keysStr.slice(0, 509), "...");
    }
    traceContext.userSpan.setTag("cloudflare.kv.query.keys"_kjc, kj::mv(keysStr));
    traceContext.userSpan.setTag(
        "cloudflare.kv.query.keys.count"_kjc, static_cast<int64_t>(name.size()));

    KJ_IF_SOME(_options, options) {
      KJ_SWITCH_ONEOF(_options) {
        KJ_CASE_ONEOF(type, kj::String) {
          traceContext.userSpan.setTag("cloudflare.kv.query.type"_kjc, kj::mv(type));
        }
        KJ_CASE_ONEOF(o, GetOptions) {
          KJ_IF_SOME(type, o.type) {
            traceContext.userSpan.setTag("cloudflare.kv.query.type"_kjc, kj::mv(type));
          }
          KJ_IF_SOME(cacheTtl, o.cacheTtl) {
            traceContext.userSpan.setTag("cloudflare.kv.query.cache_ttl"_kjc, (int64_t)cacheTtl);
          }
        }
      }
    }

    auto client =
        getHttpClient(context, headers, LimitEnforcer::KvOpType::GET_BULK, urlStr, traceContext);

    auto promise = context.waitForOutputLocks().then(
        [client = kj::mv(client), urlStr = kj::mv(urlStr), headers = kj::mv(headers),
            expectedBodySize, supportedBody = kj::mv(body)]() mutable {
      auto innerReq = client->request(kj::HttpMethod::POST, urlStr, headers, expectedBodySize);
      struct RefcountedWrapper: public kj::Refcounted {
        explicit RefcountedWrapper(kj::Own<kj::HttpClient> client): client(kj::mv(client)) {}
        kj::Own<kj::HttpClient> client;
      };
      auto rcClient = kj::refcounted<RefcountedWrapper>(kj::mv(client));
      auto req = attachToRequest(kj::mv(innerReq), kj::mv(rcClient));

      kj::Promise<void> writePromise = nullptr;
      writePromise = req.body->write(supportedBody.asBytes()).attach(kj::mv(supportedBody));

      return writePromise.attach(kj::mv(req.body)).then([resp = kj::mv(req.response)]() mutable {
        return resp.then([](kj::HttpClient::Response&& response) mutable {
          checkForErrorStatus("GET_BULK", response);
          return response.body->readAllText().attach(kj::mv(response.body));
        });
      });
    });

    return context.awaitIo(js, kj::mv(promise),
        [&, traceContext = kj::mv(traceContext)](jsg::Lock& js, kj::String text) mutable {
      traceContext.userSpan.setTag(
          "cloudflare.kv.response.size"_kjc, static_cast<int64_t>(text.size()));
      auto result = jsg::JsValue::fromJson(js, text);
      auto map = js.map();
      KJ_IF_SOME(obj, result.tryCast<jsg::JsObject>()) {
        auto values = obj.getPropertyNames(js, jsg::KeyCollectionFilter::OWN_ONLY,
            jsg::PropertyFilter::SKIP_SYMBOLS, jsg::IndexFilter::SKIP_INDICES);
        for (int i = 0; i < values.size(); i++) {
          auto key = values.get(js, i);
          map.set(js, kj::mv(key), obj.get(js, key));
        }
        traceContext.userSpan.setTag(
            "cloudflare.kv.response.returned_rows"_kjc, static_cast<int64_t>(values.size()));
      }
      return jsg::JsRef(js, map);
    });
  });
}

kj::String KvNamespace::formBulkBodyString(jsg::Lock& js,
    kj::Array<kj::String>& names,
    bool withMetadata,
    jsg::Optional<kj::OneOf<kj::String, GetOptions>>& options) {

  kj::String type = kj::str("");
  kj::String cacheTtlStr = kj::str("");
  KJ_IF_SOME(oneOfOptions, options) {
    KJ_SWITCH_ONEOF(oneOfOptions) {
      KJ_CASE_ONEOF(t, kj::String) {
        type = kj::str(t);
      }
      KJ_CASE_ONEOF(options, GetOptions) {
        KJ_IF_SOME(t, options.type) {
          type = kj::str(t);
        }
        KJ_IF_SOME(cacheTtl, options.cacheTtl) {
          cacheTtlStr = kj::str(cacheTtl);
        }
      }
    }
  }
  auto object = js.obj();

  auto keysArray =
      js.arr(names.asPtr(), [](jsg::Lock& js, const kj::String& val) { return js.str(val); });
  object.set(js, "keys", keysArray);

  if (type != kj::str("")) {
    object.set(js, "type", js.str(type));
  }
  if (withMetadata) {
    object.set(js, "withMetadata", js.boolean(true));
  }
  if (cacheTtlStr != kj::str("")) {
    object.set(js, "cacheTtl", js.str(cacheTtlStr));
  }
  return jsg::JsValue(object).toJson(js);
}

kj::OneOf<jsg::Promise<KvNamespace::GetResult>, jsg::Promise<jsg::JsRef<jsg::JsMap>>> KvNamespace::
    get(jsg::Lock& js,
        kj::OneOf<kj::String, kj::Array<kj::String>> name,
        jsg::Optional<kj::OneOf<kj::String, GetOptions>> options) {
  auto& context = IoContext::current();
  auto traceSpan = context.makeTraceSpan("kv_get"_kjc);
  auto userSpan = context.makeUserTraceSpan("kv_get"_kjc);
  TraceContext traceContext(kj::mv(traceSpan), kj::mv(userSpan));
  traceContext.userSpan.setTag("db.system.name"_kjc, kj::str("cloudflare-kv"_kjc));
  traceContext.userSpan.setTag("db.operation.name"_kjc, kj::str("get"_kjc));
  traceContext.userSpan.setTag("cloudflare.binding.name"_kjc, kj::str(bindingName));
  traceContext.userSpan.setTag("cloudflare.binding.type"_kjc, kj::str("KV"_kjc));

  KJ_SWITCH_ONEOF(name) {
    KJ_CASE_ONEOF(arr, kj::Array<kj::String>) {
      return context.attachSpans(js,
          getBulk(js, context, traceContext, kj::mv(arr), kj::mv(options), false),
          kj::mv(traceContext));
    }
    KJ_CASE_ONEOF(str, kj::String) {
      return context.attachSpans(js,
          getSingle(js, context, traceContext, kj::mv(str), kj::mv(options)), kj::mv(traceContext));
    }
  }
  KJ_UNREACHABLE;
};

jsg::Promise<KvNamespace::GetWithMetadataResult> KvNamespace::getWithMetadataSingle(jsg::Lock& js,
    IoContext& context,
    TraceContext& traceContext,
    kj::String name,
    jsg::Optional<kj::OneOf<kj::String, GetOptions>> options) {
  return getWithMetadataImpl(
      js, context, traceContext, kj::mv(name), kj::mv(options), LimitEnforcer::KvOpType::GET_WITH);
}

kj::OneOf<jsg::Promise<KvNamespace::GetWithMetadataResult>, jsg::Promise<jsg::JsRef<jsg::JsMap>>>
KvNamespace::getWithMetadata(jsg::Lock& js,
    kj::OneOf<kj::Array<kj::String>, kj::String> name,
    jsg::Optional<kj::OneOf<kj::String, GetOptions>> options) {

  auto& context = IoContext::current();
  auto traceSpan = context.makeTraceSpan("kv_getWithMetadata"_kjc);
  auto userSpan = context.makeUserTraceSpan("kv_getWithMetadata"_kjc);
  TraceContext traceContext(kj::mv(traceSpan), kj::mv(userSpan));
  traceContext.userSpan.setTag("db.system.name"_kjc, kj::str("cloudflare-kv"_kjc));
  traceContext.userSpan.setTag("db.operation.name"_kjc, kj::str("get"_kjc));
  traceContext.userSpan.setTag("cloudflare.binding.name"_kjc, kj::str(bindingName));
  traceContext.userSpan.setTag("cloudflare.binding.type"_kjc, kj::str("KV"_kjc));
  KJ_SWITCH_ONEOF(name) {
    KJ_CASE_ONEOF(arr, kj::Array<kj::String>) {
      return context.attachSpans(js,
          getBulk(js, context, traceContext, kj::mv(arr), kj::mv(options), true),
          kj::mv(traceContext));
    }
    KJ_CASE_ONEOF(str, kj::String) {
      return context.attachSpans(js,
          getWithMetadataSingle(js, context, traceContext, kj::mv(str), kj::mv(options)),
          kj::mv(traceContext));
    }
  }
  KJ_UNREACHABLE;
}

jsg::Promise<KvNamespace::GetWithMetadataResult> KvNamespace::getWithMetadataImpl(jsg::Lock& js,
    IoContext& context,
    TraceContext& traceContext,
    kj::String name,
    jsg::Optional<kj::OneOf<kj::String, GetOptions>> options,
    LimitEnforcer::KvOpType op) {
  validateKeyName("GET", name);

  traceContext.userSpan.setTag("cloudflare.kv.query.keys"_kjc, kj::str(name));
  traceContext.userSpan.setTag("cloudflare.kv.query.keys.count"_kjc, static_cast<int64_t>(1));

  kj::Url url;
  url.scheme = kj::str("https");
  url.host = kj::str("fake-host");
  url.path.add(kj::mv(name));
  url.query.add(kj::Url::QueryParam{kj::str("urlencoded"), kj::str("true")});

  kj::Maybe<kj::String> type;
  KJ_IF_SOME(oneOfOptions, options) {
    KJ_SWITCH_ONEOF(oneOfOptions) {
      KJ_CASE_ONEOF(t, kj::String) {
        type = kj::str(t);
        traceContext.userSpan.setTag("cloudflare.kv.query.type"_kjc, kj::mv(t));
      }
      KJ_CASE_ONEOF(options, GetOptions) {
        KJ_IF_SOME(t, options.type) {
          type = kj::str(t);
          traceContext.userSpan.setTag("cloudflare.kv.query.type"_kjc, kj::mv(t));
        }
        KJ_IF_SOME(cacheTtl, options.cacheTtl) {
          url.query.add(kj::Url::QueryParam{kj::str("cache_ttl"), kj::str(cacheTtl)});
          traceContext.userSpan.setTag("cloudflare.kv.query.cache_ttl"_kjc, (int64_t)cacheTtl);
        }
      }
    }
  }

  auto urlStr = url.toString(kj::Url::Context::HTTP_PROXY_REQUEST);

  auto headers = kj::HttpHeaders(context.getHeaderTable());
  auto client = getHttpClient(context, headers, op, urlStr, traceContext);

  auto request = client->request(kj::HttpMethod::GET, urlStr, headers);
  return context.awaitIo(js, kj::mv(request.response),
      [type = kj::mv(type), &context, client = kj::mv(client), traceContext = kj::mv(traceContext)](
          jsg::Lock& js, kj::HttpClient::Response&& response) mutable
      -> jsg::Promise<KvNamespace::GetWithMetadataResult> {
    auto cacheStatus =
        response.headers->get(context.getHeaderIds().cfCacheStatus).map([&](kj::StringPtr cs) {
      traceContext.userSpan.setTag("cloudflare.kv.response.cache_status"_kjc, kj::str(cs));
      return jsg::JsRef<jsg::JsValue>(js, js.strIntern(cs));
    });

    if (response.statusCode == 404 || response.statusCode == 410) {
      return js.resolvedPromise(KvNamespace::GetWithMetadataResult{
        .value = kj::none,
        .metadata = kj::none,
        .cacheStatus = kj::mv(cacheStatus),
      });
    }

    checkForErrorStatus("GET", response);

    auto metaheader = response.headers->get(context.getHeaderIds().cfKvMetadata);
    kj::Maybe<kj::String> maybeMeta;
    KJ_IF_SOME(m, metaheader) {
      traceContext.userSpan.setTag("cloudflare.kv.response.metadata"_kjc, true);
      maybeMeta = kj::str(m);
    }

    auto typeName =
        type.map([](const kj::String& s) -> kj::StringPtr { return s; }).orDefault("text");

    auto& context = IoContext::current();
    auto stream = newSystemStream(response.body.attach(kj::mv(client)),
        getContentEncoding(
            context, *response.headers, Response::BodyEncoding::AUTO, FeatureFlags::get(js)));

    jsg::Promise<KvNamespace::GetResult> result = nullptr;

    KJ_IF_SOME(size, stream->tryGetLength(StreamEncoding::IDENTITY)) {
      traceContext.userSpan.setTag("cloudflare.kv.response.size"_kjc, static_cast<int64_t>(size));
    }
    // This method always returns a single result, but this attribute should be consistent with getBulk
    traceContext.userSpan.setTag(
        "cloudflare.kv.response.returned_rows"_kjc, static_cast<int64_t>(1));

    if (typeName == "stream") {
      result = js.resolvedPromise(
          KvNamespace::GetResult(js.alloc<ReadableStream>(context, kj::mv(stream))));
    } else if (typeName == "text") {
      // NOTE: In theory we should be using awaitIoLegacy() here since ReadableStreamSource is
      //   supposed to handle pending events on its own, but we also know that the HTTP client
      //   backing a KV namespace is never implemented in local JavaScript, so whatever.
      result = context.awaitIo(js,
          stream->readAllText(context.getLimitEnforcer().getBufferingLimit())
              .attach(kj::mv(stream)),
          [](jsg::Lock&, kj::String text) { return KvNamespace::GetResult(kj::mv(text)); });
    } else if (typeName == "arrayBuffer") {
      result = context.awaitIo(js,
          stream->readAllBytes(context.getLimitEnforcer().getBufferingLimit())
              .attach(kj::mv(stream)),
          [](jsg::Lock&, kj::Array<byte> text) { return KvNamespace::GetResult(kj::mv(text)); });
    } else if (typeName == "json") {
      result = context.awaitIo(js,
          stream->readAllText(context.getLimitEnforcer().getBufferingLimit())
              .attach(kj::mv(stream)),
          [](jsg::Lock& js, kj::String text) {
        auto ref = jsg::JsRef(js, jsg::JsValue::fromJson(js, text));
        return KvNamespace::GetResult(kj::mv(ref));
      });
    } else {
      JSG_FAIL_REQUIRE(TypeError,
          "Unknown response type. Possible types are \"text\", \"arrayBuffer\", "
          "\"json\", and \"stream\".");
    }
    return result.then(js,
        [maybeMeta = kj::mv(maybeMeta), cacheStatus = kj::mv(cacheStatus)](jsg::Lock& js,
            KvNamespace::GetResult result) mutable -> KvNamespace::GetWithMetadataResult {
      kj::Maybe<jsg::JsRef<jsg::JsValue>> meta;
      KJ_IF_SOME(metaStr, maybeMeta) {
        meta = jsg::JsRef(js, jsg::JsValue::fromJson(js, metaStr));
      }
      return KvNamespace::GetWithMetadataResult{
        kj::mv(result),
        kj::mv(meta),
        kj::mv(cacheStatus),
      };
    });
  });
}

jsg::Promise<jsg::JsRef<jsg::JsValue>> KvNamespace::list(
    jsg::Lock& js, jsg::Optional<ListOptions> options) {
  return js.evalNow([&] {
    auto& context = IoContext::current();
    auto traceSpan = context.makeTraceSpan("kv_list"_kjc);
    auto userSpan = context.makeUserTraceSpan("kv_list"_kjc);
    TraceContext traceContext(kj::mv(traceSpan), kj::mv(userSpan));

    traceContext.userSpan.setTag("db.system.name"_kjc, kj::str("cloudflare-kv"_kjc));
    traceContext.userSpan.setTag("db.operation.name"_kjc, kj::str("list"_kjc));
    traceContext.userSpan.setTag("cloudflare.binding.name"_kjc, kj::str(bindingName));
    traceContext.userSpan.setTag("cloudflare.binding.type"_kjc, kj::str("KV"_kjc));

    kj::Url url;
    url.scheme = kj::str("https");
    url.host = kj::str("fake-host");
    KJ_IF_SOME(o, options) {
      KJ_IF_SOME(limit, o.limit) {
        traceContext.userSpan.setTag("cloudflare.kv.query.limit"_kjc, static_cast<int64_t>(limit));
        if (limit > 0) {
          url.query.add(kj::Url::QueryParam{kj::str("key_count_limit"), kj::str(limit)});
        }
      }
      KJ_IF_SOME(maybePrefix, o.prefix) {
        KJ_IF_SOME(prefix, maybePrefix) {
          traceContext.userSpan.setTag("cloudflare.kv.query.prefix"_kjc, kj::str(prefix));
          url.query.add(kj::Url::QueryParam{kj::str("prefix"), kj::str(prefix)});
        }
      }
      KJ_IF_SOME(maybeCursor, o.cursor) {
        KJ_IF_SOME(cursor, maybeCursor) {
          traceContext.userSpan.setTag("cloudflare.kv.query.cursor"_kjc, kj::str(cursor));
          url.query.add(kj::Url::QueryParam{kj::str("cursor"), kj::str(cursor)});
        }
      }
    }

    auto urlStr = url.toString(kj::Url::Context::HTTP_PROXY_REQUEST);

    auto headers = kj::HttpHeaders(context.getHeaderTable());
    auto client =
        getHttpClient(context, headers, LimitEnforcer::KvOpType::LIST, urlStr, traceContext);

    auto request = client->request(kj::HttpMethod::GET, urlStr, headers);
    return context.attachSpans(js,
        context.awaitIo(js, kj::mv(request.response),
            [&context, client = kj::mv(client), traceContext = kj::mv(traceContext)](
                jsg::Lock& js, kj::HttpClient::Response&& response) mutable
            -> jsg::Promise<jsg::JsRef<jsg::JsValue>> {
      checkForErrorStatus("GET", response);

      kj::Maybe<jsg::JsRef<jsg::JsValue>> cacheStatus =
          [&]() -> kj::Maybe<jsg::JsRef<jsg::JsValue>> {
        KJ_IF_SOME(cs, response.headers->get(context.getHeaderIds().cfCacheStatus)) {
          traceContext.userSpan.setTag("cloudflare.kv.response.cache_status"_kjc, kj::str(cs));
          return jsg::JsRef<jsg::JsValue>(js, js.strIntern(cs));
        }
        return kj::none;
      }();

      auto stream = newSystemStream(response.body.attach(kj::mv(client)),
          getContentEncoding(
              context, *response.headers, Response::BodyEncoding::AUTO, FeatureFlags::get(js)));

      KJ_IF_SOME(size, stream->tryGetLength(StreamEncoding::IDENTITY)) {
        traceContext.userSpan.setTag("cloudflare.kv.response.size"_kjc, static_cast<int64_t>(size));
      }

      return context.awaitIo(js,
          stream->readAllText(context.getLimitEnforcer().getBufferingLimit())
              .attach(kj::mv(stream)),
          [cacheStatus = kj::mv(cacheStatus), traceContext = kj::mv(traceContext)](
              jsg::Lock& js, kj::String text) mutable {
        auto result = jsg::JsValue::fromJson(js, text);
        parseListMetadata(traceContext, js, result,
            cacheStatus.map(
                [&](jsg::JsRef<jsg::JsValue>& cs) -> jsg::JsValue { return cs.getHandle(js); }));
        return jsg::JsRef(js, result);
      });
    }),
        kj::mv(traceContext));
  });
}

jsg::Promise<void> KvNamespace::put(jsg::Lock& js,
    kj::String name,
    KvNamespace::PutBody body,
    jsg::Optional<PutOptions> options,
    const jsg::TypeHandler<KvNamespace::PutSupportedTypes>& putTypeHandler) {
  return js.evalNow([&] {
    validateKeyName("PUT", name);

    auto& context = IoContext::current();
    auto traceSpan = context.makeTraceSpan("kv_put"_kjc);
    auto userSpan = context.makeUserTraceSpan("kv_put"_kjc);
    TraceContext traceContext(kj::mv(traceSpan), kj::mv(userSpan));

    traceContext.userSpan.setTag("db.system.name"_kjc, kj::str("cloudflare-kv"_kjc));
    traceContext.userSpan.setTag("db.operation.name"_kjc, kj::str("put"_kjc));
    traceContext.userSpan.setTag("cloudflare.binding.name"_kjc, kj::str(bindingName));
    traceContext.userSpan.setTag("cloudflare.binding.type"_kjc, kj::str("KV"_kjc));
    traceContext.userSpan.setTag("cloudflare.kv.query.keys"_kjc, kj::str(name));
    traceContext.userSpan.setTag("cloudflare.kv.query.keys.count"_kjc, static_cast<int64_t>(1));

    kj::Url url;
    url.scheme = kj::str("https");
    url.host = kj::str("fake-host");
    url.path.add(kj::mv(name));
    url.query.add(kj::Url::QueryParam{kj::str("urlencoded"), kj::str("true")});

    kj::HttpHeaders headers(context.getHeaderTable());

    // If any optional parameters were specified by the client, append them to
    // the URL's query parameters.
    KJ_IF_SOME(o, options) {
      KJ_IF_SOME(expiration, o.expiration) {
        traceContext.userSpan.setTag("cloudflare.kv.query.expiration"_kjc, int64_t(expiration));
        url.query.add(kj::Url::QueryParam{kj::str("expiration"), kj::str(expiration)});
      }
      KJ_IF_SOME(expirationTtl, o.expirationTtl) {
        traceContext.userSpan.setTag(
            "cloudflare.kv.query.expiration_ttl"_kjc, int64_t(expirationTtl));
        url.query.add(kj::Url::QueryParam{kj::str("expiration_ttl"), kj::str(expirationTtl)});
      }
      KJ_IF_SOME(maybeMetadata, o.metadata) {
        KJ_IF_SOME(metadata, maybeMetadata) {
          kj::String json = metadata.getHandle(js).toJson(js);
          headers.set(context.getHeaderIds().cfKvMetadata, kj::mv(json));
          traceContext.userSpan.setTag("cloudflare.kv.query.metadata"_kjc, true);
        }
      }
    }

    PutSupportedTypes supportedBody;

    KJ_SWITCH_ONEOF(body) {
      KJ_CASE_ONEOF(text, kj::String) {
        supportedBody = kj::mv(text);
      }
      KJ_CASE_ONEOF(object, jsg::JsObject) {
        supportedBody = JSG_REQUIRE_NONNULL(putTypeHandler.tryUnwrap(js, object), TypeError,
            "KV put() accepts only strings, ArrayBuffers, ArrayBufferViews, and "
            "ReadableStreams as values.");
        JSG_REQUIRE(!supportedBody.is<kj::String>(), TypeError,
            "KV put() accepts only strings, ArrayBuffers, ArrayBufferViews, and "
            "ReadableStreams as values.");
        // TODO(someday): replace this with logic to do something smarter with Objects
      }
    }

    kj::Maybe<uint64_t> expectedBodySize;

    KJ_SWITCH_ONEOF(supportedBody) {
      KJ_CASE_ONEOF(text, kj::String) {
        headers.setPtr(kj::HttpHeaderId::CONTENT_TYPE, MimeType::PLAINTEXT_STRING);
        expectedBodySize = uint64_t(text.size());
        traceContext.userSpan.setTag("cloudflare.kv.query.value_type"_kjc, kj::str("text"));
      }
      KJ_CASE_ONEOF(data, kj::Array<byte>) {
        expectedBodySize = uint64_t(data.size());
        traceContext.userSpan.setTag("cloudflare.kv.query.value_type"_kjc, kj::str("ArrayBuffer"));
      }
      KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
        expectedBodySize = stream->tryGetLength(StreamEncoding::IDENTITY);
        traceContext.userSpan.setTag(
            "cloudflare.kv.query.value_type"_kjc, kj::str("ReadableStream"));
      }
    }

    KJ_IF_SOME(bodySize, expectedBodySize) {
      traceContext.userSpan.setTag("cloudflare.kv.query.payload.size"_kjc, int64_t(bodySize));
    }

    auto urlStr = url.toString(kj::Url::Context::HTTP_PROXY_REQUEST);

    auto client =
        getHttpClient(context, headers, LimitEnforcer::KvOpType::PUT, urlStr, traceContext);

    auto promise = context.waitForOutputLocks().then(
        [&context, client = kj::mv(client), urlStr = kj::mv(urlStr), headers = kj::mv(headers),
            expectedBodySize, supportedBody = kj::mv(supportedBody)]() mutable {
      auto innerReq = client->request(kj::HttpMethod::PUT, urlStr, headers, expectedBodySize);
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
          writePromise = req.body->write(text.asBytes()).attach(kj::mv(text));
        }
        KJ_CASE_ONEOF(data, kj::Array<byte>) {
          writePromise = req.body->write(data).attach(kj::mv(data));
        }
        KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
          writePromise = context.run(
              [dest = newSystemStream(kj::mv(req.body), StreamEncoding::IDENTITY, context),
                  stream = kj::mv(stream)](jsg::Lock& js) mutable {
            return IoContext::current().waitForDeferredProxy(
                stream->pumpTo(js, kj::mv(dest), true));
          });
        }
      }

      return writePromise.attach(kj::mv(req.body)).then([resp = kj::mv(req.response)]() mutable {
        return resp.then([](kj::HttpClient::Response&& response) mutable {
          checkForErrorStatus("PUT", response);

          // Read and discard response body, otherwise we might burn the HTTP connection.
          return response.body->readAllBytes().attach(kj::mv(response.body)).ignoreResult();
        });
      });
    });

    return context.attachSpans(js, context.awaitIo(js, kj::mv(promise)), kj::mv(traceContext));
  });
}

jsg::Promise<void> KvNamespace::delete_(jsg::Lock& js, kj::String name) {
  return js.evalNow([&] {
    validateKeyName("DELETE", name);

    auto& context = IoContext::current();
    auto traceSpan = context.makeTraceSpan("kv_delete"_kjc);
    auto userSpan = context.makeUserTraceSpan("kv_delete"_kjc);
    TraceContext traceContext(kj::mv(traceSpan), kj::mv(userSpan));

    traceContext.userSpan.setTag("db.system.name"_kjc, kj::str("cloudflare-kv"_kjc));
    traceContext.userSpan.setTag("db.operation.name"_kjc, kj::str("delete"_kjc));
    traceContext.userSpan.setTag("cloudflare.binding.name"_kjc, kj::str(bindingName));
    traceContext.userSpan.setTag("cloudflare.binding.type"_kjc, kj::str("KV"_kjc));
    traceContext.userSpan.setTag("cloudflare.kv.query.keys"_kjc, kj::str(name));
    traceContext.userSpan.setTag("cloudflare.kv.query.keys.count"_kjc, static_cast<int64_t>(1));

    auto urlStr = kj::str("https://fake-host/", kj::encodeUriComponent(name), "?urlencoded=true");

    kj::HttpHeaders headers(context.getHeaderTable());

    auto client =
        getHttpClient(context, headers, LimitEnforcer::KvOpType::DELETE, urlStr, traceContext);

    auto promise = context.waitForOutputLocks().then(
        [headers = kj::mv(headers), client = kj::mv(client), urlStr = kj::mv(urlStr)]() mutable {
      return client->request(kj::HttpMethod::DELETE, urlStr, headers, uint64_t(0))
          .response
          .then([](kj::HttpClient::Response&& response) mutable {
        checkForErrorStatus("DELETE", response);
      }).attach(kj::mv(client));
    });

    return context.attachSpans(js, context.awaitIo(js, kj::mv(promise)), kj::mv(traceContext));
  });
}

jsg::Ref<JsRpcPromise> KvNamespace::deleteBulk(const v8::FunctionCallbackInfo<v8::Value>& args) {
  jsg::Lock& js = jsg::Lock::from(args.GetIsolate());
  auto fetcher = js.alloc<Fetcher>(subrequestChannel, Fetcher::RequiresHostAndProtocol::NO, true);
  auto method = JSG_REQUIRE_NONNULL(
      fetcher->getRpcMethodInternal(js, kj::str("delete"_kj)), Error, "missing delete method");
  return method->call(args);
}

}  // namespace workerd::api
