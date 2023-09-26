// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "cache.h"
#include "util.h"
#include <kj/encoding.h>
#include <workerd/io/io-context.h>

namespace workerd::api {

// =======================================================================================
// Cache

namespace {

// TODO(someday): Implement Cache API in preview.
constexpr auto CACHE_API_PREVIEW_WARNING =
    "The Service Workers Cache API is currently unimplemented in the Cloudflare Workers Preview. "
    "Cache API operations which would function normally in production will not throw any errors, "
    "but will have no effect. Notably, Cache.match() will always return undefined, and "
    "Cache.delete() will always return false. When you deploy your script to production, its "
    "caching behavior will function as expected."_kj;

#define LOG_CACHE_ERROR_ONCE(TEXT, RESPONSE)
// TODO(someday): Fix Cache API bugs. We logged them for two years as a reminder, but... they
//   never got fixed. The logging is making it hard to see other problems. So we're ending it.
//   If someone decides to take this on again, you can restore this macro's implementation as
//   follows:
//
// #define LOG_CACHE_ERROR_ONCE(TEXT, RESPONSE) ({ \
//   static bool seen = false; \
//   if (!seen) { \
//     seen = true; \
//     KJ_LOG(ERROR, TEXT, RESPONSE.statusCode, RESPONSE.statusText); \
//   } \
// })

// Throw an application-visible exception if the URL won't be parsed correctly at a lower
// layer. If the URL is valid then just return it. The purpose of this function is to avoid
// throwing an "internal error".
kj::StringPtr validateUrl(kj::StringPtr url) {
  // TODO(bug): We should parse and process URLs the same way we would URLs passed to fetch().
  //   But, that might mean e.g. discarding fragments ("hashes", stuff after a '#'), which would
  //   be a change in behavior that could subtly affect production workers...

  static constexpr auto urlOptions = kj::Url::Options {
    .percentDecode = false,
    .allowEmpty = true,
  };

  JSG_REQUIRE(kj::Url::tryParse(url, kj::Url::HTTP_PROXY_REQUEST, urlOptions) != kj::none,
              TypeError, "Invalid URL. Cache API keys must be fully-qualified, valid URLs.");

  return url;
}

}  // namespace

Cache::Cache(kj::Maybe<kj::String> cacheName): cacheName(kj::mv(cacheName)) {}

jsg::Unimplemented Cache::add(Request::Info request) {
  return {};
}

jsg::Unimplemented Cache::addAll(kj::Array<Request::Info> requests) {
  return {};
}

jsg::Promise<jsg::Optional<jsg::Ref<Response>>> Cache::match(
    jsg::Lock& js, Request::Info requestOrUrl, jsg::Optional<CacheQueryOptions> options) {
  // TODO(someday): Implement Cache API in preview.
  auto& context = IoContext::current();
  if (context.isFiddle()) {
    context.logWarningOnce(CACHE_API_PREVIEW_WARNING);
    return js.resolvedPromise(jsg::Optional<jsg::Ref<Response>>(nullptr));
  }

  // This use of evalNow() is obsoleted by the capture_async_api_throws compatibility flag, but
  // we need to keep it here for people who don't have that flag set.
  return js.evalNow([&]() -> jsg::Promise<jsg::Optional<jsg::Ref<Response>>> {
    auto jsRequest = Request::coerce(js, kj::mv(requestOrUrl), nullptr);

    if (!options.orDefault({}).ignoreMethod.orDefault(false) &&
        jsRequest->getMethodEnum() != kj::HttpMethod::GET) {
      return js.resolvedPromise(jsg::Optional<jsg::Ref<Response>>(nullptr));
    }

    auto httpClient = getHttpClient(context, jsRequest->serializeCfBlobJson(js),
                                    "cache_match"_kjc);
    auto requestHeaders = kj::HttpHeaders(context.getHeaderTable());
    jsRequest->shallowCopyHeadersTo(requestHeaders);
    requestHeaders.set(context.getHeaderIds().cacheControl, "only-if-cached");
    auto nativeRequest = httpClient->request(
        kj::HttpMethod::GET, validateUrl(jsRequest->getUrl()), requestHeaders, uint64_t(0));

    return context.awaitIo(js, kj::mv(nativeRequest.response),
        [httpClient = kj::mv(httpClient), &context]
        (jsg::Lock& js, kj::HttpClient::Response&& response)
        mutable -> jsg::Optional<jsg::Ref<Response>> {
      response.body = response.body.attach(kj::mv(httpClient));

      kj::StringPtr cacheStatus;
      KJ_IF_SOME(cs, response.headers->get(context.getHeaderIds().cfCacheStatus)) {
        cacheStatus = cs;
      } else {
        // This is an internal error representing a violation of the contract between us and
        // the cache. Since it is always conformant to return undefined from Cache::match()
        // (because we are allowed to evict any asset at any time), we don't really need to make the
        // script fail. However, it might be indicative of a larger problem, and should be
        // investigated.
        LOG_CACHE_ERROR_ONCE("Response to Cache API GET has no CF-Cache-Status: ", response);
        return nullptr;
      }

      // The status code should be a 504 on cache miss, but we need to rely on CF-Cache-Status
      // because someone might cache a 504.
      // See https://httpwg.org/specs/rfc7234.html#cache-request-directive.only-if-cached
      //
      // TODO(cleanup): CACHE-5949 We should never receive EXPIRED or UPDATING responses, but we do.
      //   We treat them the same as a MISS mostly to keep from blowing up our Sentry reports.
      // TODO(someday): If the cache status is EXPIRED and we return undefined here, does a PURGE on
      //   this URL result in a 200, causing us to return true from Cache::delete_()? If so, that's
      //   a small inconsistency: we shouldn't have a match failure but a delete success.
      if (cacheStatus == "MISS" || cacheStatus == "EXPIRED" || cacheStatus == "UPDATING") {
        return nullptr;
      } else if (cacheStatus != "HIT") {
        // Another internal error. See above comment where we retrieve the CF-Cache-Status header.
        LOG_CACHE_ERROR_ONCE("Response to Cache API GET has invalid CF-Cache-Status: ", response);
        return nullptr;
      }

      return makeHttpResponse(
          js, kj::HttpMethod::GET, {},
          response.statusCode, response.statusText, *response.headers,
          kj::mv(response.body), kj::none);
    });
  });
}

// Send a PUT request to the cache whose URL is the original request URL and whose body is the
// HTTP response we'd like to cache for that request.
//
// The HTTP response in the PUT request body (the "PUT payload") must itself be an HTTP message,
// except that it MUST NOT have chunked encoding applied to it, even if it has a
// Transfer-Encoding: chunked header. To be clear, the PUT request itself may be chunked, but it
// must not have any nested chunked encoding.
//
// In order to extract the response's data to serialize it, we'll need to call
// `jsResponse->send()`, which will properly encode the response's body if a Content-Encoding
// header is present. This means we'll need to create an instance of kj::HttpService::Response.
jsg::Promise<void> Cache::put(jsg::Lock& js, Request::Info requestOrUrl,
    jsg::Ref<Response> jsResponse, CompatibilityFlags::Reader flags) {

  // Fake kj::HttpService::Response implementation that allows us to reuse jsResponse->send() to
  // serialize the response (headers + body) in the format needed to serve as the payload of
  // our cache PUT request.
  class ResponseSerializer final: public kj::HttpService::Response {
  public:
    struct Payload {
      // The serialized form of the response to be cached. This stream itself contains a full
      // HTTP response, with headers and body, representing the content of jsResponse to be written
      // to the cache.
      kj::Own<kj::AsyncInputStream> stream;

      // A promise which resolves once the payload's headers have been written. Normally, this
      // couldn't possibly resolve until the body has been written, and jsRepsonse->send() won't
      // complete until then -- except if the body is empty, in which case jsResponse->send() may
      // return immediately.
      kj::Promise<void> writeHeadersPromise;
    };

    Payload getPayload() {
      return KJ_ASSERT_NONNULL(kj::mv(payload));
    }

  private:
    kj::Own<kj::AsyncOutputStream> send(
        uint statusCode, kj::StringPtr statusText, const kj::HttpHeaders& headers,
        kj::Maybe<uint64_t> expectedBodySize) override {
      kj::String contentLength;

      kj::StringPtr connectionHeaders[kj::HttpHeaders::CONNECTION_HEADERS_COUNT];
      KJ_IF_SOME(ebs, expectedBodySize) {
        contentLength = kj::str(ebs);
        connectionHeaders[kj::HttpHeaders::BuiltinIndices::CONTENT_LENGTH] = contentLength;
      } else {
        connectionHeaders[kj::HttpHeaders::BuiltinIndices::TRANSFER_ENCODING] = "chunked";
      }

      auto serializedHeaders = headers.serializeResponse(statusCode, statusText, connectionHeaders);

      auto expectedPayloadSize = expectedBodySize.map([&](uint64_t size) {
        return size + serializedHeaders.size();
      });

      // We want to create an AsyncInputStream that represents the payload, including both headers
      // and body. To do this, we'll create a one-way pipe, using the input end of the pipe as
      // said stream. This means we have to write the headers, followed by the body, to the output
      // end of the pipe.
      //
      // send() needs to return a stream to which the caller can write the body. Since we need to
      // make sure the headers are written first, we'll return a kj::newPromisedStream(), using a
      // promise that resolves to the pipe output as soon as the headers are written.
      //
      // There's a catch: Unfortunately, if the caller doesn't intend to write any body, then they
      // will probably drop the return stream immediately. This could prematurely cancel our header
      // write. To avoid that, we split the promise and keep a branch in `writeHeadersPromise`,
      // which will have to be awaited separately.
      auto payloadPipe = kj::newOneWayPipe(expectedPayloadSize);

      static auto constexpr handleHeaders = [](kj::Own<kj::AsyncOutputStream> out,
                                               kj::String serializedHeaders)
          -> kj::Promise<kj::Tuple<kj::Own<kj::AsyncOutputStream>, bool>> {
        co_await out->write(serializedHeaders.begin(), serializedHeaders.size());
        co_return kj::tuple(kj::mv(out), false);
      };

      auto headersPromises = handleHeaders(kj::mv(payloadPipe.out),
                                           kj::mv(serializedHeaders)).split();

      payload = Payload {
        .stream = kj::mv(payloadPipe.in),
        .writeHeadersPromise = kj::get<1>(headersPromises).ignoreResult()
      };

      return kj::newPromisedStream(kj::mv(kj::get<0>(headersPromises)));
    }

    kj::Own<kj::WebSocket> acceptWebSocket(const kj::HttpHeaders&) override {
      JSG_FAIL_REQUIRE(TypeError, "Cannot cache WebSocket upgrade response.");
    }

    kj::Maybe<Payload> payload;
  };

  // This use of evalNow() is obsoleted by the capture_async_api_throws compatibility flag, but
  // we need to keep it here for people who don't have that flag set.
  return js.evalNow([&] {
    auto jsRequest = Request::coerce(js, kj::mv(requestOrUrl), nullptr);

    // TODO(conform): Require that jsRequest's url has an http or https scheme. This is only
    //   important if api::Request is changed to parse its URL eagerly (as required by spec), rather
    //   than at fetch()-time.

    JSG_REQUIRE(jsRequest->getMethodEnum() == kj::HttpMethod::GET,
        TypeError, "Cannot cache response to non-GET request.");

    JSG_REQUIRE(jsResponse->getStatus() != 206,
        TypeError, "Cannot cache response to a range request (206 Partial Content).");

    auto responseHeadersRef = jsResponse->getHeaders(js);
    KJ_IF_SOME(vary, responseHeadersRef->get(jsg::ByteString(kj::str("vary")))) {
      JSG_REQUIRE(vary.findFirst('*') == kj::none,
          TypeError, "Cannot cache response with 'Vary: *' header.");
    }

    auto& context = IoContext::current();

    if (jsResponse->getStatus() == 304) {
      // Silently discard 304 status responses to conditional requests. Caching 304s could be a
      // source of bugs in a worker, since a worker which blindly stuffs responses from `fetch()`
      // into cache could end up caching one, then later respond to non-conditional requests with
      // the cached 304.
      //
      // Unlike the 206 response status check above, we don't throw here because we used to allow
      // this behavior. Silently discarding 304s maintains backwards compatibility and is actually
      // still spec-conformant.

      if (context.isInspectorEnabled()) {
        context.logWarning("Ignoring attempt to Cache.put() a 304 status response. 304 responses "
            "are not meaningful to cache, and a potential source of bugs. Consider validating that "
            "the response status is meaningful to cache before calling Cache.put().");
      }

      return js.resolvedPromise();
    }

    ResponseSerializer serializer;
    // We need to send the response to our serializer immediately in order to fulfill Cache.put()'s
    // contract: the caller should be able to observe that the response body is disturbed as soon
    // as put() returns.
    auto serializePromise = jsResponse->send(js, serializer, {}, kj::none);
    auto payload = serializer.getPayload();

    // TODO(someday): Implement Cache API in preview. This bail-out lives all the way down here,
    //   after all KJ_REQUIRE checks and the start of response serialization, so that Cache.put()
    //   fulfills its contract, even in the preview. This prevents buggy code from working in the
    //   preview, but failing in production.
    if (context.isFiddle()) {
      context.logWarningOnce(CACHE_API_PREVIEW_WARNING);
      return js.resolvedPromise();
    }

    // Wait for output locks and cache put quota, trying to avoid returning to the KJ event loop
    // in the common case where no waits are needed.
    jsg::Promise<kj::Maybe<IoOwn<kj::AsyncInputStream>>> startStreamPromise = nullptr;
    auto makeCachePutStream = [&context, stream = kj::mv(payload.stream)](jsg::Lock& js) mutable {
      return context.makeCachePutStream(js, kj::mv(stream));
    };
    KJ_IF_SOME(p, context.waitForOutputLocksIfNecessary()) {
      startStreamPromise = context.awaitIo(js, kj::mv(p), kj::mv(makeCachePutStream));
    } else {
      startStreamPromise = makeCachePutStream(js);
    }

    return startStreamPromise.then(js, context.addFunctor(
        [this, &context, jsRequest = kj::mv(jsRequest),
         serializePromise = kj::mv(serializePromise),
         writePayloadHeadersPromise = kj::mv(payload.writeHeadersPromise)]
        (jsg::Lock& js, kj::Maybe<IoOwn<kj::AsyncInputStream>> maybeStream) mutable
        -> jsg::Promise<void> {
      if (maybeStream == kj::none) {
        // Cache API PUT quota must have been exceeded.
        return js.resolvedPromise();
      }

      kj::Own<kj::AsyncInputStream> payloadStream = KJ_ASSERT_NONNULL(kj::mv(maybeStream));

      // Make the PUT request to cache.
      auto httpClient = getHttpClient(context, jsRequest->serializeCfBlobJson(js),
                                      "cache_put"_kjc);
      auto requestHeaders = kj::HttpHeaders(context.getHeaderTable());
      jsRequest->shallowCopyHeadersTo(requestHeaders);
      auto nativeRequest = httpClient->request(
          kj::HttpMethod::PUT, validateUrl(jsRequest->getUrl()),
          requestHeaders, payloadStream->tryGetLength());

      auto pumpRequestBodyPromise = payloadStream->pumpTo(*nativeRequest.body)
          .ignoreResult();
          // NOTE: We don't attach nativeRequest.body here because we want to control its
          //   destruction timing in the event of an error; see below.

      // The next step is a bit complicated as it occurs in two separate async flows.
      // First, we await the serialization promise, then enter "deferred proxying" by issuing
      // `KJ_CO_MAGIC BEGIN_DEFERRED_PROXYING` from our coroutine. Everything after that
      // `KJ_CO_MAGIC` constitutes the second async flow that actually handles the request and
      // response.
      //
      // Weird: It's important that these objects be torn down in the right order and that the
      // DeferredProxy promise is handled separately from the inner promise.
      //
      // Moreover, there is an interesting property: In the event that `httpClient` is destroyed
      // immediately after `bodyStream` (i.e. without returning to the KJ event loop in between),
      // and the body is chunked, then the connection will be closed before the terminating chunk
      // can be written. This is actually convenient as it allows us to make sure that when we
      // bail out due to an error, the cache is able to see that the request was incomplete and
      // should therefore not commit the cache entry.
      //
      // This is a bit of an accident. It would be much better if KJ's AsyncOutputStream had an
      // explicit `end()` method to indicate all data had been written successfully, rather than
      // just assume so in the destructor. But, that's a major refactor, and it's immediately
      // important to us that we don't write incomplete cache entries, so we rely on this hack for
      // now. See EW-812 for the broader problem.
      //
      // A little funky: The process of "serializing" the cache entry payload means reading all the
      // data from the payload body stream and writing it to cache. But, the payload body might
      // originate from the app's own JavaScript, rather than being the response to some remote
      // request. If the stream is JS-backed, then we want to be careful to track "pending events".
      // Specifically, if the stream hasn't reported EOF yet, but JavaScript stops executing and
      // there is no external I/O that we're waiting for, then we know that the stream will never
      // end, and we want to cancel out the IoContext proactively.
      //
      // If we were to use `context.awaitIo(serializePromise)` here, we'd lose this property,
      // because the context would believe that waiting for the stream itself constituted I/O, even
      // if the stream is backed by JS.
      //
      // On the other hand, once the serialization step completes, we need to wait for the cache
      // backend to respond. At that point, we *are* awaiting I/O, and want to record that
      // correctly.
      //
      // So basically, we have an asynchorous promise we need to wait for, and for the first part
      // of that wait, we don't want to count it as pending I/O, but for the second part, we do.
      // How do we accomplish this?
      //
      // Well, it just so happens that `serializePromise` is a special kind of promise that might
      // help us -- it's kj::Promise<DeferredProxy<void>>, a deferred proxy stream promise. This
      // is a promise-for-a-promise, with an interesting property: the outer promise is used to
      // wait for JavaScript-backed stream events, while the inner promise represents pure external
      // I/O. The method context.awaitDeferredProxy() awaits this special kind of promise, and it
      // already only counts the inner promise as being external pending I/O.
      //
      // However, we have some additional work we want to do *after* serializePromise (both parts)
      // completes -- additional work that is also external I/O. So how do we handle that? Well...
      // we can actually append it to `serializePromise`'s inner promise! Then awaitDeferredProxy()
      // will properly treat it as pending I/O, but only *after* the outer promise completes. This
      // gets us everything we want.
      //
      // Hence, what you see here: we first await the serializePromise, then enter deferred proxying
      // with our magic `KJ_CO_MAGIC`, then perform all our additional work. Then we
      // `awaitDeferredProxy()` the whole thing.

      // Here we handle the promise for the DeferredProxy itself.
      static auto constexpr handleSerialize = [](
          kj::Promise<DeferredProxy<void>> serialize,
          kj::Own<kj::HttpClient> httpClient,
          kj::Promise<kj::HttpClient::Response> responsePromise,
          kj::Own<kj::AsyncOutputStream> bodyStream,
          kj::Promise<void> pumpRequestBodyPromise,
          kj::Promise<void> writePayloadHeadersPromise,
          kj::Own<kj::AsyncInputStream> payloadStream)
              -> kj::Promise<DeferredProxy<void>> {
        // This is extremely odd and a bit annoying but we have to make sure
        // these are destroyed in a particular order due to cross-dependencies
        // for each. If the kj::Promise returned by handleSerialize is dropped
        // before the co_await serialize completes, then these won't ever be
        // moved away into the handleResponse method (which ensures proper
        // cleanup order). In such a case, we explicitly layout a cleanup order
        // here to make it clear.
        // Note: we could do this by ordering the arguments in a particular way,
        // or by doing what a previous iteration of this code did and put everything
        // into a struct in a particular order but pulling things out like this makes
        // what is going on here much more intentional and explicit.
        //
        // If these are not cleaned up in the right order, there can be subtle
        // use-after-free issues reported by asan and certain flows can end up
        // hanging.
        KJ_DEFER({
          pumpRequestBodyPromise = nullptr;
          payloadStream = nullptr;
          bodyStream = nullptr;
          responsePromise = nullptr;
          writePayloadHeadersPromise = nullptr;
          httpClient = nullptr;
        });
        try {
          auto deferred = co_await serialize;

          // With our `serialize` promise having resolved to a DeferredProxy, we can now enter
          // deferred proxying ourselves.
          KJ_CO_MAGIC BEGIN_DEFERRED_PROXYING;

          co_await deferred.proxyTask;
          // Make sure headers get written even if the body was empty -- see comments earlier.
          co_await writePayloadHeadersPromise;
          // Make sure the request body is done being pumped and had no errors. If serialization
          // completed successfully, then this should also complete immediately thereafter.
          co_await pumpRequestBodyPromise;
          // It is important to destroy the bodyStream before actually waiting on the
          // responsePromise to ensure that the terminal chunk is written since the bodyStream
          // may only write the terminal chunk in the streams destructor.
          bodyStream = nullptr;
          payloadStream = nullptr;
          auto response = co_await responsePromise;
          // We expect to see either 204 (success) or 413 (failure). Any other status code is a
          // violation of the contract between us and the cache, and is an internal
          // error, which we log. However, there's no need to throw, since the Cache API is an
          // ephemeral K/V store, and we never guaranteed the script we'd actually cache anything.
          if (response.statusCode != 204 && response.statusCode != 413) {
            LOG_CACHE_ERROR_ONCE(
                "Response to Cache API PUT was neither 204 nor 413: ", response);
          }
        } catch (...) {
          auto exception = kj::getCaughtExceptionAsKj();
          if (exception.getType() != kj::Exception::Type::DISCONNECTED) {
            kj::throwFatalException(kj::mv(exception));
          }
          // If the origin or the cache disconnected, we don't treat this as an error, as put()
          // doesn't guarantee that it stores anything anyway.
          //
          // TODO(someday): I (Kenton) don't undestand why we'd explicitly want to hide this
          //   error, even though hiding it is technically not a violation of the contract. To me
          //   this seems undesirable, especially when it was the origin that failed. The caller
          //   can always choose to ignore errors if they want (and many do, by passing to
          //   waitUntil()). However, there is at least one test which depends on this behavior,
          //   and probably production Workers in the wild, so I'm not changing it for now.
        }
      };

      return context.awaitDeferredProxy(handleSerialize(
          kj::mv(serializePromise),
          kj::mv(httpClient),
          kj::mv(nativeRequest.response),
          kj::mv(nativeRequest.body),
          kj::mv(pumpRequestBodyPromise),
          kj::mv(writePayloadHeadersPromise),
          kj::mv(payloadStream)));
    }));
  });
}

jsg::Promise<bool> Cache::delete_(
    jsg::Lock& js, Request::Info requestOrUrl, jsg::Optional<CacheQueryOptions> options) {
  // TODO(someday): Implement Cache API in preview.
  auto& context = IoContext::current();
  if (context.isFiddle()) {
    context.logWarningOnce(CACHE_API_PREVIEW_WARNING);
    return js.resolvedPromise(false);
  }

  // This use of evalNow() is obsoleted by the capture_async_api_throws compatibility flag, but
  // we need to keep it here for people who don't have that flag set.
  return js.evalNow([&]() -> jsg::Promise<bool> {
    auto jsRequest = Request::coerce(js, kj::mv(requestOrUrl), nullptr);

    if (!options.orDefault({}).ignoreMethod.orDefault(false) &&
        jsRequest->getMethodEnum() != kj::HttpMethod::GET) {
      return js.resolvedPromise(false);
    }

    // Make the PURGE request to cache.

    auto httpClient = getHttpClient(context, jsRequest->serializeCfBlobJson(js),
                                    "cache_delete"_kjc);
    auto requestHeaders = kj::HttpHeaders(context.getHeaderTable());
    jsRequest->shallowCopyHeadersTo(requestHeaders);
    // HACK: The cache doesn't permit PURGE requests from the outside world. It does this by
    //   filtering on X-Real-IP, which can't be set from the outside world. X-Real-IP can, however,
    //   be set by a Worker when making requests to its own origin, as "spoofing" client IPs to
    //   your own origin isn't a security flaw. Also, a Worker sending PURGE requests to its own
    //   origin's cache is not a security flaw (that's what this very API is implementing after
    //   all) so it all lines up nicely.
    requestHeaders.add("X-Real-IP"_kj, "127.0.0.1"_kj);
    auto nativeRequest = httpClient->request(
        kj::HttpMethod::PURGE, validateUrl(jsRequest->getUrl()), requestHeaders, uint64_t(0));

    return context.awaitIo(js, kj::mv(nativeRequest.response),
        [httpClient = kj::mv(httpClient)]
        (jsg::Lock&, kj::HttpClient::Response&& response) -> bool {
      if (response.statusCode == 200) {
        return true;
      } else if (response.statusCode == 404) {
        return false;
      } else if (response.statusCode == 429) {
        // Throw, but do not log the response to Sentry, as rate-limited subrequests are normal
        JSG_FAIL_REQUIRE(Error,
            "Unable to delete cached response. Subrequests are being rate-limited.");
      }
      LOG_CACHE_ERROR_ONCE("Response to Cache API PURGE was neither 200 nor 404: ", response);
      JSG_FAIL_REQUIRE(Error, "Unable to delete cached response.");
    });
  });
}

kj::Own<kj::HttpClient> Cache::getHttpClient(IoContext& context,
                                             kj::Maybe<kj::String> cfBlobJson,
                                             kj::ConstString operationName) {
  auto span = context.makeTraceSpan(kj::mv(operationName));

  auto cacheClient = context.getCacheClient();
  auto httpClient = cacheName.map([&](kj::String& n) {
    return cacheClient->getNamespace(n, kj::mv(cfBlobJson), span);
  }).orDefault([&]() {
    return cacheClient->getDefault(kj::mv(cfBlobJson), span);
  });
  httpClient = httpClient.attach(kj::mv(span), kj::mv(cacheClient));
  return httpClient;
}

// =======================================================================================
// CacheStorage

CacheStorage::CacheStorage()
    : default_(jsg::alloc<Cache>(nullptr)) {}

jsg::Promise<jsg::Ref<Cache>> CacheStorage::open(jsg::Lock& js, kj::String cacheName) {
  // Set some reasonable limit to prevent scripts from blowing up our control header size.
  static constexpr auto MAX_CACHE_NAME_LENGTH = 1024;
  JSG_REQUIRE(cacheName.size() < MAX_CACHE_NAME_LENGTH,
      TypeError, "Cache name is too long.");  // Mah spoon is toooo big.

  // TODO(someday): Implement Cache API in preview.

  // It is possible here that open() will be called in the global scope in fiddle
  // mode in which case the warning will not be emitted. But that's ok? The warning
  // is not critical by any stretch.
  if (IoContext::hasCurrent()) {
    auto& context = IoContext::current();
    if (context.isFiddle()) {
      context.logWarningOnce(CACHE_API_PREVIEW_WARNING);
    }
  }

  return js.resolvedPromise(jsg::alloc<Cache>(kj::mv(cacheName)));
}

}  // namespace workerd::api
