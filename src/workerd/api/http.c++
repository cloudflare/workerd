// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "http.h"

#include "data-url.h"
#include "headers.h"
#include "queue.h"
#include "sockets.h"
#include "streams/readable-source.h"
#include "system-streams.h"
#include "util.h"
#include "worker-rpc.h"
#include "workerd/jsg/jsvalue.h"

#include <workerd/io/features.h>
#include <workerd/io/io-context.h>
#include <workerd/jsg/ser.h>
#include <workerd/jsg/url.h>
#include <workerd/util/abortable.h>
#include <workerd/util/autogate.h>
#include <workerd/util/entropy.h>
#include <workerd/util/http-util.h>
#include <workerd/util/mimetype.h>
#include <workerd/util/own-util.h>
#include <workerd/util/stream-utils.h>
#include <workerd/util/strings.h>
#include <workerd/util/thread-scopes.h>

#include <capnp/compat/http-over-capnp.capnp.h>
#include <kj/compat/url.h>
#include <kj/encoding.h>
#include <kj/memory.h>
#include <kj/parse/char.h>

namespace workerd::api {

namespace {
Request::CacheMode getCacheModeFromName(kj::StringPtr value) {
  if (value == "no-store") return Request::CacheMode::NOSTORE;
  if (value == "no-cache") return Request::CacheMode::NOCACHE;
  if (value == "reload") return Request::CacheMode::RELOAD;
  JSG_FAIL_REQUIRE(TypeError, kj::str("Unsupported cache mode: ", value));
}

jsg::Optional<kj::StringPtr> getCacheModeName(Request::CacheMode mode) {
  switch (mode) {
    case (Request::CacheMode::NONE):
      return kj::none;
    case (Request::CacheMode::NOCACHE):
      return "no-cache"_kj;
    case (Request::CacheMode::NOSTORE):
      return "no-store"_kj;
    case (Request::CacheMode::RELOAD):
      return "reload"_kj;
  }
  KJ_UNREACHABLE;
}

}  // namespace

// -----------------------------------------------------------------------------
// serialization of headers
//
// http-over-capnp.capnp has a nice list of common header names, taken from the HTTP/2 standard.
// We'll use it as an optimization.
//
// Note that using numeric IDs for headers implies we lose the original capitalization. However,
// the JS Headers API doesn't actually give the application any way to observe the capitalization
// of header names -- it only becomes relevant when serializing over HTTP/1.1. And at that point,
// we are actually free to change the capitalization anyway, and we commonly do (KJ itself will
// normalize capitalization of all registered headers, and http-over-capnp also loses
// capitalization). So, it's certainly not worth it to try to keep the original capitalization
// across serialization.

Body::Buffer Body::Buffer::clone(jsg::Lock& js) {
  Buffer result;
  result.view = view;
  KJ_SWITCH_ONEOF(ownBytes) {
    KJ_CASE_ONEOF(refcounted, kj::Own<RefcountedBytes>) {
      result.ownBytes = kj::addRef(*refcounted);
    }
    KJ_CASE_ONEOF(blob, jsg::Ref<Blob>) {
      result.ownBytes = blob.addRef();
    }
  }
  return result;
}

Body::ExtractedBody::ExtractedBody(
    jsg::Ref<ReadableStream> stream, kj::Maybe<Buffer> buffer, kj::Maybe<kj::String> contentType)
    : impl{kj::mv(stream), kj::mv(buffer)},
      contentType(kj::mv(contentType)) {
  // This check is in the constructor rather than `extractBody()`, because we often construct
  // ExtractedBodys from ReadableStreams directly.
  JSG_REQUIRE(!impl.stream->isDisturbed(), TypeError,
      "This ReadableStream is disturbed (has already been read from), and cannot "
      "be used as a body.");
}

Body::ExtractedBody Body::extractBody(jsg::Lock& js, Initializer init) {
  Buffer buffer;
  kj::Maybe<kj::String> contentType;

  KJ_SWITCH_ONEOF(init) {
    KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
      return kj::mv(stream);
    }
    KJ_CASE_ONEOF(gen, jsg::AsyncGeneratorIgnoringStrings<jsg::Value>) {
      return ReadableStream::from(js, gen.release());
    }
    KJ_CASE_ONEOF(text, kj::String) {
      contentType = kj::str(MimeType::PLAINTEXT_STRING);
      buffer = kj::mv(text);
    }
    KJ_CASE_ONEOF(bytes, kj::Array<byte>) {
      // NOTE: The spec would have us create a copy of the input buffer here, but that would be a
      //   sad waste of CPU and memory. This is technically a non-conformity that would allow a user
      //   to construct a Body from a BufferSource and then later modify the BufferSource. However,
      //   redirects cause body streams to be reconstructed from the original, possibly mutated,
      //   buffer anyway, so this is unlikely to be a problem in practice.
      buffer = kj::mv(bytes);
    }
    KJ_CASE_ONEOF(blob, jsg::Ref<Blob>) {
      // Blobs always have a type, but it defaults to an empty string. We should NOT set
      // Content-Type when the blob type is empty.
      kj::StringPtr blobType = blob->getType();
      if (blobType != nullptr) {
        contentType = kj::str(blobType);
      }
      buffer = kj::mv(blob);
    }
    KJ_CASE_ONEOF(formData, jsg::Ref<FormData>) {
      // Make an array of characters containing random hexadecimal digits.
      //
      // Note: Rather than use random hex digits, we could generate the hex digits by hashing the
      //   form-data content itself! This would give us pleasing assurance that our boundary string
      //   is not present in the content being divided. The downside is CPU usage if, say, a user
      //   uploads an enormous file.
      kj::FixedArray<kj::byte, 16> boundaryBuffer;
      workerd::getEntropy(boundaryBuffer);
      auto boundary = kj::encodeHex(boundaryBuffer);
      contentType = MimeType::formDataWithBoundary(boundary);
      buffer = formData->serialize(boundary);
    }
    KJ_CASE_ONEOF(searchParams, jsg::Ref<URLSearchParams>) {
      contentType = MimeType::formUrlEncodedWithCharset("UTF-8"_kj);
      buffer = searchParams->toString();
    }
    KJ_CASE_ONEOF(searchParams, jsg::Ref<url::URLSearchParams>) {
      contentType = MimeType::formUrlEncodedWithCharset("UTF-8"_kj);
      buffer = searchParams->toString();
    }
  }

  auto buf = buffer.clone(js);

  // We use streams::newMemorySource() here rather than newSystemStream() wrapping a
  // newMemoryInputStream() because we do NOT want deferred proxying for bodies with
  // V8 heap provenance. Specifically, the bufferCopy.view here, while being a kj::ArrayPtr,
  // will typically be wrapping a v8::BackingStore, and we must ensure that is is consumed
  // and destroyed while under the isolate lock, which means deferred proxying is not allowed.
  auto rs = streams::newMemorySource(buf.view, kj::heap(kj::mv(buf.ownBytes)));

  return {js.alloc<ReadableStream>(IoContext::current(), kj::mv(rs)), kj::mv(buffer),
    kj::mv(contentType)};
}

Body::Body(jsg::Lock& js, kj::Maybe<ExtractedBody> init, Headers& headers)
    : impl(kj::mv(init).map([&headers](auto i) -> Impl {
        KJ_IF_SOME(ct, i.contentType) {
          if (!headers.hasCommon(capnp::CommonHeaderName::CONTENT_TYPE)) {
            // The spec allows the user to override the Content-Type, if they wish, so we only set
            // the Content-Type if it doesn't already exist.
            headers.setCommon(capnp::CommonHeaderName::CONTENT_TYPE, kj::mv(ct));
          } else KJ_IF_SOME(parsed, MimeType::tryParse(ct)) {
            if (MimeType::FORM_DATA == parsed) {
              // Custom content-type request/responses with FormData are broken since they require a
              // boundary parameter only the FormData serializer can provide. Let's warn if a dev does this.
              IoContext::current().logWarning(
                  "A FormData body was provided with a custom Content-Type header when constructing "
                  "a Request or Response object. This will prevent the recipient of the Request or "
                  "Response from being able to parse the body. Consider omitting the custom "
                  "Content-Type header.");
            }
          }
        }
        return kj::mv(i.impl);
      })),
      headersRef(headers) {}

kj::Maybe<Body::Buffer> Body::getBodyBuffer(jsg::Lock& js) {
  KJ_IF_SOME(i, impl) {
    KJ_IF_SOME(b, i.buffer) {
      return b.clone(js);
    }
  }
  return kj::none;
}

bool Body::canRewindBody() {
  KJ_IF_SOME(i, impl) {
    // We can only rewind buffer-backed bodies.
    return i.buffer != kj::none;
  }
  // Null bodies are trivially "rewindable".
  return true;
}

void Body::rewindBody(jsg::Lock& js) {
  KJ_DASSERT(canRewindBody());

  KJ_IF_SOME(i, impl) {
    auto bufferCopy = KJ_ASSERT_NONNULL(i.buffer).clone(js);

    // We use streams::newMemorySource() here rather than newSystemStream() wrapping a
    // newMemoryInputStream() because we do NOT want deferred proxying for bodies with
    // V8 heap provenance. Specifically, the bufferCopy.view here, while being a kj::ArrayPtr,
    // will typically be wrapping a v8::BackingStore, and we must ensure that is is consumed
    // and destroyed while under the isolate lock, which means deferred proxying is not allowed.
    auto rs = streams::newMemorySource(bufferCopy.view, kj::heap(kj::mv(bufferCopy.ownBytes)));
    i.stream = js.alloc<ReadableStream>(IoContext::current(), kj::mv(rs));
  }
}

void Body::nullifyBody() {
  impl = kj::none;
}

kj::Maybe<jsg::Ref<ReadableStream>> Body::getBody() {
  KJ_IF_SOME(i, impl) {
    return i.stream.addRef();
  }
  return kj::none;
}
bool Body::getBodyUsed() {
  KJ_IF_SOME(i, impl) {
    return i.stream->isDisturbed();
  }
  return false;
}
jsg::Promise<jsg::BufferSource> Body::arrayBuffer(jsg::Lock& js) {
  KJ_IF_SOME(i, impl) {
    return js.evalNow([&] {
      JSG_REQUIRE(!i.stream->isDisturbed(), TypeError,
          "Body has already been used. "
          "It can only be used once. Use tee() first if you need to read it twice.");
      return i.stream->getController().readAllBytes(
          js, IoContext::current().getLimitEnforcer().getBufferingLimit());
    });
  }

  // If there's no body, we just return an empty array.
  // See https://fetch.spec.whatwg.org/#concept-body-consume-body
  auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, 0);
  return js.resolvedPromise(jsg::BufferSource(js, kj::mv(backing)));
}

jsg::Promise<jsg::BufferSource> Body::bytes(jsg::Lock& js) {
  return arrayBuffer(js).then(js,
      [](jsg::Lock& js, jsg::BufferSource data) { return data.getTypedView<v8::Uint8Array>(js); });
}

jsg::Promise<kj::String> Body::text(jsg::Lock& js) {
  KJ_IF_SOME(i, impl) {
    return js.evalNow([&] {
      JSG_REQUIRE(!i.stream->isDisturbed(), TypeError,
          "Body has already been used. "
          "It can only be used once. Use tee() first if you need to read it twice.");

      // A common mistake is to call .text() on non-text content, e.g. because you're implementing a
      // search-and-replace across your whole site and you forgot that it'll apply to images too.
      // When running in the fiddle, let's warn the developer if they do this.
      auto& context = IoContext::current();
      if (context.hasWarningHandler()) {
        KJ_IF_SOME(type, headersRef.getCommon(js, capnp::CommonHeaderName::CONTENT_TYPE)) {
          maybeWarnIfNotText(js, type);
        }
      }

      return i.stream->getController().readAllText(
          js, context.getLimitEnforcer().getBufferingLimit());
    });
  }

  // If there's no body, we just return an empty string.
  // See https://fetch.spec.whatwg.org/#concept-body-consume-body
  return js.resolvedPromise(kj::String());
}

jsg::Promise<jsg::Ref<FormData>> Body::formData(jsg::Lock& js) {
  auto formData = js.alloc<FormData>();

  return js.evalNow([&] {
    JSG_REQUIRE(!getBodyUsed(), TypeError,
        "Body has already been used. "
        "It can only be used once. Use tee() first if you need to read it twice.");

    auto contentType =
        JSG_REQUIRE_NONNULL(headersRef.getCommon(js, capnp::CommonHeaderName::CONTENT_TYPE),
            TypeError, "Parsing a Body as FormData requires a Content-Type header.");

    KJ_IF_SOME(i, impl) {
      KJ_ASSERT(!i.stream->isDisturbed());
      auto& context = IoContext::current();
      return i.stream->getController()
          .readAllText(js, context.getLimitEnforcer().getBufferingLimit())
          .then(js,
              [contentType = kj::mv(contentType), formData = kj::mv(formData)](
                  auto& js, kj::String rawText) mutable {
        formData->parse(js, kj::mv(rawText), contentType,
            !FeatureFlags::get(js).getFormDataParserSupportsFiles());
        return kj::mv(formData);
      });
    }

    // Theoretically, we already know if this will throw: the empty string is a valid
    // application/x-www-form-urlencoded body, but not multipart/form-data. However, best to let
    // FormData::parse() make the decision, to keep the logic in one place.
    formData->parse(
        js, kj::String(), contentType, !FeatureFlags::get(js).getFormDataParserSupportsFiles());
    return js.resolvedPromise(kj::mv(formData));
  });
}

jsg::Promise<jsg::Value> Body::json(jsg::Lock& js) {
  return text(js).then(js, [](jsg::Lock& js, kj::String text) { return js.parseJson(text); });
}

jsg::Promise<jsg::Ref<Blob>> Body::blob(jsg::Lock& js) {
  return arrayBuffer(js).then(js, [this](jsg::Lock& js, jsg::BufferSource buffer) {
    kj::String contentType = headersRef.getCommon(js, capnp::CommonHeaderName::CONTENT_TYPE)
                                 .map([](auto&& b) -> kj::String {
      return kj::mv(b);
    }).orDefault(nullptr);

    if (FeatureFlags::get(js).getBlobStandardMimeType()) {
      contentType = MimeType::extract(contentType)
                        .map([](MimeType&& mt) -> kj::String {
        return mt.toString();
      }).orDefault(nullptr);
    }

    return js.alloc<Blob>(js, kj::mv(buffer), kj::mv(contentType));
  });
}

kj::Maybe<Body::ExtractedBody> Body::clone(jsg::Lock& js) {
  KJ_IF_SOME(i, impl) {
    auto branches = i.stream->tee(js);

    i.stream = kj::mv(branches[0]);

    return ExtractedBody{kj::mv(branches[1]), i.buffer.map([&](Buffer& b) { return b.clone(js); })};
  }

  return kj::none;
}

// =======================================================================================

jsg::Ref<Request> Request::coerce(
    jsg::Lock& js, Request::Info input, jsg::Optional<Request::Initializer> init) {
  return input.is<jsg::Ref<Request>>() && init == kj::none
      ? kj::mv(input.get<jsg::Ref<Request>>())
      : Request::constructor(js, kj::mv(input), kj::mv(init));
}

jsg::Optional<kj::StringPtr> Request::getCache(jsg::Lock& js) {
  return getCacheModeName(cacheMode);
}
Request::CacheMode Request::getCacheMode() {
  return cacheMode;
}

jsg::Ref<Request> Request::constructor(
    jsg::Lock& js, Request::Info input, jsg::Optional<Request::Initializer> init) {
  kj::String url;
  kj::HttpMethod method = kj::HttpMethod::GET;
  kj::Maybe<jsg::Ref<Headers>> headers;
  kj::Maybe<jsg::Ref<Fetcher>> fetcher;
  kj::Maybe<jsg::Ref<AbortSignal>> signal;
  CfProperty cf;
  kj::Maybe<Body::ExtractedBody> body;
  Redirect redirect = Redirect::FOLLOW;
  CacheMode cacheMode = CacheMode::NONE;
  Response_BodyEncoding responseBodyEncoding = Response_BodyEncoding::AUTO;

  KJ_SWITCH_ONEOF(input) {
    KJ_CASE_ONEOF(u, kj::String) {
      url = kj::mv(u);

      // TODO(later): This is rather unfortunate. The original implementation of
      // this used non-standard URL parsing in violation of the spec. Unfortunately
      // some users have come to depend on the non-standard behavior so we have to
      // gate the standard behavior with a compat flag. Ideally we'd just be able to
      // use the standard parsed URL throughout all of the code but in order to
      // minimize the number of changes, we're going to ultimately end up double
      // parsing (and serializing) the URL... here we parse it with the standard
      // parser, reserialize it back into a string for the sake of not modifying
      // the rest of the implementation. Fortunately the standard parser is fast
      // but it would eventually be nice to eliminate the double parsing.
      if (FeatureFlags::get(js).getFetchStandardUrl()) {
        auto parsed = JSG_REQUIRE_NONNULL(
            jsg::Url::tryParse(url.asPtr()), TypeError, kj::str("Invalid URL: ", url));
        url = kj::str(parsed.getHref());
      }
    }
    KJ_CASE_ONEOF(r, jsg::Ref<Request>) {
      // Check to see if we're getting a new body from `init`. If so, we want to ignore `input`'s
      // body. Note that this is technically non-conformant behavior, but the spec is broken:
      // https://github.com/whatwg/fetch/issues/674
      //
      // TODO(cleanup): The body extraction logic is getting difficult to follow with the current
      //   2-pass initialization we perform (first `input`, then `init`). It'd be nice to defer
      //   checks like the one we're avoiding here until the very end, so the `init` pass has a
      //   chance to override `input`'s members *before* we check if the body we're extracting is
      //   disturbed.
      bool ignoreInputBody = false;
      KJ_IF_SOME(i, init) {
        KJ_SWITCH_ONEOF(i) {
          KJ_CASE_ONEOF(initDict, InitializerDict) {
            if (initDict.body != kj::none) {
              ignoreInputBody = true;
            }
          }
          KJ_CASE_ONEOF(otherRequest, jsg::Ref<Request>) {
            // If our initializer dictionary is another Request object, it will always have a `body`
            // property. Even if it's null, we should treat it as an explicit body rewrite.
            ignoreInputBody = true;
          }
        }
      }

      jsg::Ref<Request> oldRequest = kj::mv(r);
      url = kj::str(oldRequest->getUrl());
      method = oldRequest->method;
      headers = js.alloc<Headers>(js, *oldRequest->headers);
      cf = oldRequest->cf.deepClone(js);
      if (!ignoreInputBody) {
        JSG_REQUIRE(!oldRequest->getBodyUsed(), TypeError,
            "Cannot reconstruct a Request with a used body.");
        KJ_IF_SOME(oldJsBody, oldRequest->getBody()) {
          // The stream spec says to "create a proxy" for the passed in readable, which it
          // defines generically as creating a TransformStream and using pipeThrough to pass
          // the input stream through, giving the TransformStream's readable to the extracted
          // body below. We don't need to do that. Instead, we just create a new ReadableStream
          // that takes over ownership of the internals of the given stream. The given stream
          // is left in a locked/disturbed mode so that it can no longer be used.
          body = Body::ExtractedBody((oldJsBody)->detach(js), oldRequest->getBodyBuffer(js));
        }
      }
      cacheMode = oldRequest->getCacheMode();
      redirect = oldRequest->getRedirectEnum();
      fetcher = oldRequest->getFetcher();
      signal = oldRequest->getSignal();
    }
  }

  KJ_IF_SOME(i, init) {
    KJ_SWITCH_ONEOF(i) {
      KJ_CASE_ONEOF(initDict, InitializerDict) {
        KJ_IF_SOME(integrity, initDict.integrity) {
          JSG_REQUIRE(integrity.size() == 0, TypeError,
              "Subrequest integrity checking is not implemented. "
              "The integrity option must be either undefined or an empty string.");
        }

        KJ_IF_SOME(m, initDict.method) {
          KJ_IF_SOME(code, tryParseHttpMethod(m)) {
            method = code;
          } else KJ_IF_SOME(code, kj::tryParseHttpMethod(toUpper(m))) {
            method = code;
            if (!FeatureFlags::get(js).getUpperCaseAllHttpMethods()) {
              // This is actually the spec defined behavior. We're expected to only
              // upper case get, post, put, delete, head, and options per the spec.
              // Other methods, even if they would be recognized if they were uppercased,
              // are supposed to be rejected.
              // Refs: https://fetch.spec.whatwg.org/#methods
              switch (method) {
                case kj::HttpMethod::GET:
                case kj::HttpMethod::POST:
                case kj::HttpMethod::PUT:
                case kj::HttpMethod::DELETE:
                case kj::HttpMethod::HEAD:
                case kj::HttpMethod::OPTIONS:
                  break;
                default:
                  JSG_FAIL_REQUIRE(TypeError, kj::str("Invalid HTTP method string: ", m));
              }
            }
          } else {
            JSG_FAIL_REQUIRE(TypeError, kj::str("Invalid HTTP method string: ", m));
          }
        }

        KJ_IF_SOME(h, initDict.headers) {
          headers = Headers::constructor(js, kj::mv(h));
        }

        KJ_IF_SOME(p, initDict.fetcher) {
          fetcher = kj::mv(p);
        }

        KJ_IF_SOME(s, initDict.signal) {
          // Note that since this is an optional-maybe, `s` is type Maybe<AbortSignal>. It could
          // be null. But that seems like what we want. If someone doesn't specify `signal` at all,
          // they want to inherit the `signal` property from the original request. But if they
          // explicitly say `signal: null`, they must want to drop the signal that was on the
          // original request.
          signal = kj::mv(s);
          initDict.signal = kj::none;
        }

        KJ_IF_SOME(newCf, initDict.cf) {
          // TODO(cleanup): When initDict.cf is updated to use jsg::JsRef instead
          // of jsg::V8Ref, we can clean this up a bit further.
          auto cloned = newCf.deepClone(js);
          cf = CfProperty(js, jsg::JsObject(cloned.getHandle(js)));
        }

        KJ_IF_SOME(b, kj::mv(initDict.body).orDefault(kj::none)) {
          body = Body::extractBody(js, kj::mv(b));
          JSG_REQUIRE(method != kj::HttpMethod::GET && method != kj::HttpMethod::HEAD, TypeError,
              "Request with a GET or HEAD method cannot have a body.");
        }

        KJ_IF_SOME(r, initDict.redirect) {
          redirect = JSG_REQUIRE_NONNULL(Request::tryParseRedirect(r), TypeError,
              "Invalid redirect value, must be one of \"follow\" or \"manual\" (\"error\" won't be "
              "implemented since it does not make sense at the edge; use \"manual\" and check the "
              "response status code).");
        }

        KJ_IF_SOME(c, initDict.cache) {
          cacheMode = getCacheModeFromName(c);
        }

        KJ_IF_SOME(e, initDict.encodeResponseBody) {
          if (e == "manual"_kj) {
            responseBodyEncoding = Response_BodyEncoding::MANUAL;
          } else if (e == "automatic"_kj) {
            responseBodyEncoding = Response_BodyEncoding::AUTO;
          } else {
            JSG_FAIL_REQUIRE(TypeError, kj::str("encodeResponseBody: unexpected value: ", e));
          }
        }

        if (initDict.method != kj::none || initDict.body != kj::none) {
          // We modified at least one of the method or the body. In this case, we enforce the
          // spec rule that GET/HEAD requests cannot have bodies. (On the other hand, if neither
          // of these fields was modified, but the original Request object that we're rewriting
          // already represented a GET/HEAD method with a body, we allow that to pass through.
          // We support proxying such requests and rewriting their URL/headers/etc.)
          JSG_REQUIRE(
              (method != kj::HttpMethod::GET && method != kj::HttpMethod::HEAD) || body == kj::none,
              TypeError, "Request with a GET or HEAD method cannot have a body.");
        }
      }
      KJ_CASE_ONEOF(otherRequest, jsg::Ref<Request>) {
        method = otherRequest->method;
        redirect = otherRequest->redirect;
        cacheMode = otherRequest->cacheMode;
        responseBodyEncoding = otherRequest->responseBodyEncoding;
        fetcher = otherRequest->getFetcher();
        signal = otherRequest->getSignal();
        headers = js.alloc<Headers>(js, *otherRequest->headers);
        cf = otherRequest->cf.deepClone(js);
        KJ_IF_SOME(b, otherRequest->getBody()) {
          // Note that unlike when `input` (Request ctor's 1st parameter) is a Request object, here
          // we're NOT stealing the other request's body, because we're supposed to pretend that the
          // other request is just a dictionary.
          body = Body::ExtractedBody(kj::mv(b));
        }
      }
    }
  }

  if (headers == kj::none) {
    headers = js.alloc<Headers>();
  }

  // TODO(conform): If `init` has a keepalive flag, pass it to the Body constructor.
  return js.alloc<Request>(js, method, url, redirect, KJ_ASSERT_NONNULL(kj::mv(headers)),
      kj::mv(fetcher), kj::mv(signal), kj::mv(cf), kj::mv(body), /* thisSignal */ kj::none,
      cacheMode, responseBodyEncoding);
}

jsg::Ref<Request> Request::clone(jsg::Lock& js) {
  auto headersClone = headers->clone(js);

  auto cfClone = cf.deepClone(js);
  auto bodyClone = Body::clone(js);

  return js.alloc<Request>(js, method, url, redirect, kj::mv(headersClone), getFetcher(),
      /* signal */ getSignal(), kj::mv(cfClone), kj::mv(bodyClone), /* thisSignal */ kj::none,
      cacheMode, responseBodyEncoding);
}

kj::StringPtr Request::getMethod() {
  return kj::toCharSequence(method);
}
kj::StringPtr Request::getUrl() {
  return url;
}
jsg::Ref<Headers> Request::getHeaders(jsg::Lock& js) {
  return headers.addRef();
}
kj::StringPtr Request::getRedirect() {
  // TODO(cleanup): Web IDL enum <-> JS string conversion boilerplate is a common need and could be
  //   factored out.

  switch (redirect) {
    case Redirect::FOLLOW:
      return "follow";
    case Redirect::MANUAL:
      return "manual";
  }

  KJ_UNREACHABLE;
}
kj::Maybe<jsg::Ref<Fetcher>> Request::getFetcher() {
  return fetcher.map([](jsg::Ref<Fetcher>& f) { return f.addRef(); });
}
kj::Maybe<jsg::Ref<AbortSignal>> Request::getSignal() {
  return signal.map([](jsg::Ref<AbortSignal>& s) { return s.addRef(); });
}

jsg::Optional<jsg::JsObject> Request::getCf(jsg::Lock& js) {
  return cf.get(js);
}

// If signal is given, getThisSignal returns a reference to it.
// Otherwise, we lazily create a new never-aborts AbortSignal that will not
// be used for anything because the spec wills it so.
// Note: To be pedantic, the spec actually calls for us to create a
// second AbortSignal in addition to the one being passed in, but
// that's a bit silly and unnecessary.
// The name "thisSignal" is derived from the fetch spec, which draws a
// distinction between the "signal" and "this' signal".
jsg::Ref<AbortSignal> Request::getThisSignal(jsg::Lock& js) {
  KJ_IF_SOME(s, signal) {
    return s.addRef();
  }
  KJ_IF_SOME(s, thisSignal) {
    return s.addRef();
  }
  auto newSignal = js.alloc<AbortSignal>(kj::none, kj::none, AbortSignal::Flag::NEVER_ABORTS);
  thisSignal = newSignal.addRef();
  return newSignal;
}

void Request::clearSignalIfIgnoredForSubrequest(jsg::Lock& js) {
  KJ_IF_SOME(s, signal) {
    if (s->isIgnoredForSubrequests(js)) {
      signal = kj::none;
    }
  }
}

kj::Maybe<Request::Redirect> Request::tryParseRedirect(kj::StringPtr redirect) {
  if (strcasecmp(redirect.cStr(), "follow") == 0) {
    return Redirect::FOLLOW;
  }
  if (strcasecmp(redirect.cStr(), "manual") == 0) {
    return Redirect::MANUAL;
  }
  return kj::none;
}

void Request::shallowCopyHeadersTo(kj::HttpHeaders& out) {
  headers->shallowCopyTo(out);
}

kj::Maybe<kj::String> Request::serializeCfBlobJson(jsg::Lock& js) {
  if (cacheMode == CacheMode::NONE) {
    return cf.serialize(js);
  }

  CfProperty clone;
  KJ_IF_SOME(obj, cf.get(js)) {
    (void)obj;
    clone = cf.deepClone(js);
  } else {
    clone = CfProperty(js, js.obj());
  }
  auto obj = KJ_ASSERT_NONNULL(clone.get(js));

  constexpr int NOCACHE_TTL = -1;
  switch (cacheMode) {
    case CacheMode::NOSTORE:
      if (obj.has(js, "cacheTtl")) {
        jsg::JsValue oldTtl = obj.get(js, "cacheTtl");
        JSG_REQUIRE(oldTtl.strictEquals(js.num(NOCACHE_TTL)), TypeError,
            kj::str("CacheTtl: ", oldTtl, ", is not compatible with cache: ",
                getCacheModeName(cacheMode).orDefault("none"_kj), " header."));
      } else {
        obj.set(js, "cacheTtl", js.num(NOCACHE_TTL));
      }
      KJ_FALLTHROUGH;
    case CacheMode::RELOAD:
      obj.set(js, "cacheLevel", js.str("bypass"_kjc));
      break;
    case CacheMode::NOCACHE:
      obj.set(js, "cacheForceRevalidate", js.boolean(true));
      break;
    case CacheMode::NONE:
      KJ_UNREACHABLE;
  }

  return clone.serialize(js);
}

void RequestInitializerDict::validate(jsg::Lock& js) {
  KJ_IF_SOME(c, cache) {
    // Check compatibility flag
    JSG_REQUIRE(FeatureFlags::get(js).getCacheOptionEnabled(), Error,
        kj::str("The 'cache' field on 'RequestInitializerDict' is not implemented."));

    // Validate that the cache type is valid
    auto cacheMode = getCacheModeFromName(c);

    bool invalidNoCache =
        !FeatureFlags::get(js).getCacheNoCache() && (cacheMode == Request::CacheMode::NOCACHE);
    bool invalidReload =
        !FeatureFlags::get(js).getCacheReload() && (cacheMode == Request::CacheMode::RELOAD);
    JSG_REQUIRE(
        !invalidNoCache && !invalidReload, TypeError, kj::str("Unsupported cache mode: ", c));
  }

  KJ_IF_SOME(e, encodeResponseBody) {
    JSG_REQUIRE(e == "manual"_kj || e == "automatic"_kj, TypeError,
        kj::str("encodeResponseBody: unexpected value: ", e));
  }
}

void Request::serialize(jsg::Lock& js,
    jsg::Serializer& serializer,
    const jsg::TypeHandler<RequestInitializerDict>& initDictHandler) {
  serializer.writeLengthDelimited(url);

  // Our strategy is to construct an initializer dict object and serialize that as a JS object.
  // This makes the deserialization end really simple (just call the constructor), and it also
  // gives us extensibility: we can add new fields without having to bump the serialization tag.
  // clang-format off
  serializer.write(js, jsg::JsValue(initDictHandler.wrap(js, RequestInitializerDict{
    // GET is the default, so only serialize the method if it's something else.
    .method = method == kj::HttpMethod::GET ? jsg::Optional<kj::String>() : kj::str(method),

    .headers = headers.addRef(),

    .body = getBody().map([](jsg::Ref<ReadableStream> stream) -> Body::Initializer {
      // jsg::Ref<ReadableStream> is one of the possible variants of Body::Initializer.
      return kj::mv(stream);
    }),

    // "manual" is the default for `redirect`, so only encode if it's not that.
    .redirect = redirect == Redirect::MANUAL ? kj::str(getRedirect())
                                              : kj::Maybe<kj::String>(kj::none),

    // We have to ignore .fetcher for serialization. We can't simply fail if a fetcher is present
    // because requests received by the top-level fetch handler actually have .fetcher set to
    // the hidden "next" binding, which historically could be different from null (although in
    // practice these days it is always the same). We obviously want to be able to serialize
    // requests received by the top-level fetch handler so... we have to ignore this. This
    // property should probably go away in any case.

    .cf = cf.getRef(js),

    .cache = getCacheModeName(cacheMode).map(
        [](kj::StringPtr name) -> kj::String { return kj::str(name); }),

    // .mode is unimplemented
    // .credentials is unimplemented
    // .referrer is unimplemented
    // .referrerPolicy is unimplemented
    // .integrity is required to be empty

    // If an AbortSignal is present, we'll try to serialize it. As of this writing, AbortSignal
    // is not serializable, but we could add support for sending it over RPC in the future.
    //
    // Note we have to double-Maybe this, so that if no signal is present, the property is absent
    // instead of `null`.
    .signal =
        signal.map([&js](jsg::Ref<AbortSignal>& s) -> kj::Maybe<jsg::Ref<AbortSignal>> {
      if (s->isIgnoredForSubrequests(js)) {
        return kj::none;
      }

      return s.addRef();
    }),

    // Only serialize responseBodyEncoding if it's not the default AUTO
    .encodeResponseBody = responseBodyEncoding == Response_BodyEncoding::AUTO
        ? jsg::Optional<kj::String>()
        : kj::str("manual")
  })));
}

jsg::Ref<Request> Request::deserialize(jsg::Lock& js,
    rpc::SerializationTag tag,
    jsg::Deserializer& deserializer,
    const jsg::TypeHandler<RequestInitializerDict>& initDictHandler) {
  auto url = deserializer.readLengthDelimitedString();
  auto init = KJ_ASSERT_NONNULL(initDictHandler.tryUnwrap(js, deserializer.readValue(js)));
  return Request::constructor(js, kj::mv(url), kj::mv(init));
}

// =======================================================================================

namespace {
constexpr kj::StringPtr defaultStatusText(uint statusCode) {
  // RFC 7231 recommendations, unless otherwise specified.
  // https://tools.ietf.org/html/rfc7231#section-6.1
#define STATUS(code, text) case code: return text##_kj
  switch (statusCode) {
    // Status code 0 is used exclusively with error responses
    // created using Response.error()
    STATUS(0, "");
    STATUS(100, "Continue");
    STATUS(101, "Switching Protocols");
    STATUS(102, "Processing");   // RFC 2518, WebDAV
    STATUS(103, "Early Hints");  // RFC 8297
    STATUS(200, "OK");
    STATUS(201, "Created");
    STATUS(202, "Accepted");
    STATUS(203, "Non-Authoritative Information");
    STATUS(204, "No Content");
    STATUS(205, "Reset Content");
    STATUS(206, "Partial Content");
    STATUS(207, "Multi-Status");      // RFC 4918, WebDAV
    STATUS(208, "Already Reported");  // RFC 5842, WebDAV
    STATUS(226, "IM Used");           // RFC 3229
    STATUS(300, "Multiple Choices");
    STATUS(301, "Moved Permanently");
    STATUS(302, "Found");
    STATUS(303, "See Other");
    STATUS(304, "Not Modified");
    STATUS(305, "Use Proxy");

    STATUS(307, "Temporary Redirect");
    STATUS(308, "Permanent Redirect");  // RFC 7538
    STATUS(400, "Bad Request");
    STATUS(401, "Unauthorized");
    STATUS(402, "Payment Required");
    STATUS(403, "Forbidden");
    STATUS(404, "Not Found");
    STATUS(405, "Method Not Allowed");
    STATUS(406, "Not Acceptable");
    STATUS(407, "Proxy Authentication Required");
    STATUS(408, "Request Timeout");
    STATUS(409, "Conflict");
    STATUS(410, "Gone");
    STATUS(411, "Length Required");
    STATUS(412, "Precondition Failed");
    STATUS(413, "Payload Too Large");
    STATUS(414, "URI Too Long");
    STATUS(415, "Unsupported Media Type");
    STATUS(416, "Range Not Satisfiable");
    STATUS(417, "Expectation Failed");
    STATUS(418, "I'm a teapot");          // RFC 2324
    STATUS(421, "Misdirected Request");   // RFC 7540
    STATUS(422, "Unprocessable Entity");  // RFC 4918, WebDAV
    STATUS(423, "Locked");                // RFC 4918, WebDAV
    STATUS(424, "Failed Dependency");     // RFC 4918, WebDAV
    STATUS(426, "Upgrade Required");
    STATUS(428, "Precondition Required");            // RFC 6585
    STATUS(429, "Too Many Requests");                // RFC 6585
    STATUS(431, "Request Header Fields Too Large");  // RFC 6585
    STATUS(451, "Unavailable For Legal Reasons");    // RFC 7725
    STATUS(500, "Internal Server Error");
    STATUS(501, "Not Implemented");
    STATUS(502, "Bad Gateway");
    STATUS(503, "Service Unavailable");
    STATUS(504, "Gateway Timeout");
    STATUS(505, "HTTP Version Not Supported");
    STATUS(506, "Variant Also Negotiates");          // RFC 2295
    STATUS(507, "Insufficient Storage");             // RFC 4918, WebDAV
    STATUS(508, "Loop Detected");                    // RFC 5842, WebDAV
    STATUS(510, "Not Extended");                     // RFC 2774
    STATUS(511, "Network Authentication Required");  // RFC 6585
    default:
      // If we don't recognize the status code, check which range it falls into and use the status
      // code class defined by RFC 7231, section 6, as the status text.
      if (statusCode >= 200 && statusCode < 300) {
        return "Successful"_kj;
      } else if (statusCode >= 300 && statusCode < 400) {
        return "Redirection"_kj;
      } else if (statusCode >= 400 && statusCode < 500) {
        return "Client Error"_kj;
      } else if (statusCode >= 500 && statusCode < 600) {
        return "Server Error"_kj;
      } else {
        return ""_kj;
      }
  }
#undef STATUS
}

constexpr bool isNullBodyStatusCode(uint statusCode) {
  switch (statusCode) {
    // Fetch spec section 2.2.3 defines these status codes as null body statuses:
    // https://fetch.spec.whatwg.org/#null-body-status
    case 101:
    case 204:
    case 205:
    case 304:
      return true;
    default:
      return false;
  }
}

constexpr bool isRedirectStatusCode(uint statusCode) {
  switch (statusCode) {
    // Fetch spec section 2.2.3 defines these status codes as redirect statuses:
    // https://fetch.spec.whatwg.org/#redirect-status
    case 301:
    case 302:
    case 303:
    case 307:
    case 308:
      return true;
    default:
      return false;
  }
}
}  // namespace

Response::Response(jsg::Lock& js,
    int statusCode,
    kj::Maybe<kj::String> statusText,
    jsg::Ref<Headers> headers,
    CfProperty&& cf,
    kj::Maybe<Body::ExtractedBody> body,
    kj::Array<kj::String> urlList,
    kj::Maybe<jsg::Ref<WebSocket>> webSocket,
    Response::BodyEncoding bodyEncoding)
    : Body(js, kj::mv(body), *headers),
      statusCode(statusCode),
      statusText(kj::mv(statusText)),
      headers(kj::mv(headers)),
      cf(kj::mv(cf)),
      urlList(kj::mv(urlList)),
      webSocket(kj::mv(webSocket)),
      bodyEncoding(bodyEncoding),
      asyncContext(jsg::AsyncContextFrame::currentRef(js)) {}

jsg::Ref<Response> Response::error(jsg::Lock& js) {
  return js.alloc<Response>(js, 0, kj::none, js.alloc<Headers>(), CfProperty(), kj::none);
};

jsg::Ref<Response> Response::constructor(jsg::Lock& js,
    jsg::Optional<kj::Maybe<Body::Initializer>> optionalBodyInit,
    jsg::Optional<Initializer> maybeInit) {
  auto bodyInit = kj::mv(optionalBodyInit).orDefault(kj::none);
  Initializer init = kj::mv(maybeInit).orDefault(InitializerDict());

  int statusCode = 200;
  BodyEncoding bodyEncoding = Response::BodyEncoding::AUTO;

  kj::Maybe<kj::String> statusText;
  kj::Maybe<Body::ExtractedBody> body = kj::none;
  jsg::Ref<Headers> headers = nullptr;
  CfProperty cf;
  kj::Maybe<jsg::Ref<WebSocket>> webSocket = kj::none;

  KJ_SWITCH_ONEOF(init) {
    KJ_CASE_ONEOF(initDict, InitializerDict) {
      KJ_IF_SOME(status, initDict.status) {
        statusCode = status;
      }
      KJ_IF_SOME(t, initDict.statusText) {
        statusText = kj::mv(t);
      }
      KJ_IF_SOME(v, initDict.encodeBody) {
        if (v == "manual"_kj) {
          bodyEncoding = Response::BodyEncoding::MANUAL;
        } else if (v == "automatic"_kj) {
          bodyEncoding = Response::BodyEncoding::AUTO;
        } else {
          JSG_FAIL_REQUIRE(TypeError, kj::str("encodeBody: unexpected value: ", v));
        }
      }

      KJ_IF_SOME(initHeaders, initDict.headers) {
        headers = Headers::constructor(js, kj::mv(initHeaders));
      } else {
        headers = js.alloc<Headers>();
      }

      KJ_IF_SOME(newCf, initDict.cf) {
        // TODO(cleanup): When initDict.cf is updated to use jsg::JsRef instead
        // of jsg::V8Ref, we can clean this up a bit further.
        auto cloned = newCf.deepClone(js);
        cf = CfProperty(js, jsg::JsObject(cloned.getHandle(js)));
      }

      KJ_IF_SOME(ws, initDict.webSocket) {
        KJ_IF_SOME(ws2, ws) {
          webSocket = ws2.addRef();
        }
      }
    }
    KJ_CASE_ONEOF(otherResponse, jsg::Ref<Response>) {
      // Note that in a true Fetch-conformant implementation, this entire case is enabled by Web IDL
      // treating objects as dictionaries. However, some of our Response class's properties are
      // jsg::WontImplement, which prevent us from relying on that Web IDL behavior ourselves.

      statusCode = otherResponse->statusCode;
      bodyEncoding = otherResponse->bodyEncoding;
      kj::StringPtr otherStatusText = otherResponse->getStatusText();
      if (otherStatusText != defaultStatusText(statusCode)) {
        statusText = kj::str(otherStatusText);
      }
      headers = js.alloc<Headers>(js, *otherResponse->headers);
      cf = otherResponse->cf.deepClone(js);
      KJ_IF_SOME(otherWs, otherResponse->webSocket) {
        webSocket = otherWs.addRef();
      }
    }
  }

  if (webSocket == kj::none) {
    JSG_REQUIRE(statusCode >= 200 && statusCode <= 599, RangeError,
        "Responses may only be constructed with status codes in the range 200 to 599, inclusive.");
  } else {
    JSG_REQUIRE(
        statusCode == 101, RangeError, "Responses with a WebSocket must have status code 101.");
  }

  KJ_IF_SOME(s, statusText) {
    // Disallow control characters (especially \r and \n) in statusText since it could allow
    // header injection.
    //
    // TODO(cleanup): Once this is deployed, update open-source KJ HTTP to do this automatically.
    for (char c: s) {
      if (static_cast<byte>(c) < 0x20u) {
        JSG_FAIL_REQUIRE(TypeError, "Invalid statusText");
      }
    }
  }

  KJ_IF_SOME(bi, bodyInit) {
    body = Body::extractBody(js, kj::mv(bi));
    if (isNullBodyStatusCode(statusCode)) {
      // TODO(conform): We *should* fail unconditionally here, but during the Workers beta we
      //   allowed Responses to have null body statuses with non-null, zero-length bodies. In order
      //   not to break anything in production, for now we allow the author to construct a Response
      //   with a zero-length buffer, but we give them a console warning. If we can ever verify that
      //   no one relies on this behavior, we should remove this non-conformity.

      // Fail if the body is not backed by a buffer (i.e., it's an opaque ReadableStream).
      auto& buffer = JSG_REQUIRE_NONNULL(KJ_ASSERT_NONNULL(body).impl.buffer, TypeError,
          "Response with null body status (101, 204, 205, or 304) cannot have a body.");

      // Fail if the body is backed by a non-zero-length buffer.
      JSG_REQUIRE(buffer.view.size() == 0, TypeError,
          "Response with null body status (101, 204, 205, or 304) cannot have a body.");

      auto& context = IoContext::current();
      if (context.hasWarningHandler()) {
        context.logWarning(kj::str("Constructing a Response with a null body status (", statusCode,
            ") and a non-null, "
            "zero-length body. This is technically incorrect, and we recommend you update your "
            "code to explicitly pass in a `null` body, e.g. `new Response(null, { status: ",
            statusCode,
            ", ... })`. (We continue to allow the zero-length body behavior because it "
            "was previously the only way to construct a Response with a null body status. This "
            "behavior may change in the future.)"));
      }

      // Treat the zero-length body as a null body.
      body = kj::none;
    }
  }

  return js.alloc<Response>(js, statusCode, kj::mv(statusText), kj::mv(headers),
      kj::mv(cf), kj::mv(body), nullptr, kj::mv(webSocket), bodyEncoding);
}

jsg::Ref<Response> Response::redirect(jsg::Lock& js, kj::String url, jsg::Optional<int> status) {
  auto statusCode = status.orDefault(302);
  if (!isRedirectStatusCode(statusCode)) {
    JSG_FAIL_REQUIRE(RangeError,
        kj::str(statusCode,
            " is not a redirect status code. "
            "It must be one of: 301, 302, 303, 307, or 308."));
  }

  // TODO(conform): The URL is supposed to be parsed relative to the "current setting's object's API
  //   base URL".
  kj::String parsedUrl = nullptr;
  if (FeatureFlags::get(js).getSpecCompliantResponseRedirect()) {
    auto parsed = JSG_REQUIRE_NONNULL(
        jsg::Url::tryParse(url.asPtr()), TypeError, "Unable to parse URL: ", url);
    parsedUrl = kj::str(parsed.getHref());
  } else {
    auto urlOptions = kj::Url::Options{.percentDecode = false, .allowEmpty = true};
    auto maybeParsedUrl = kj::Url::tryParse(url.asPtr(), kj::Url::REMOTE_HREF, urlOptions);
    if (maybeParsedUrl == kj::none) {
      JSG_FAIL_REQUIRE(TypeError, kj::str("Unable to parse URL: ", url));
    }
    parsedUrl = KJ_ASSERT_NONNULL(kj::mv(maybeParsedUrl)).toString();
  }

  if (!kj::HttpHeaders::isValidHeaderValue(parsedUrl)) {
    JSG_FAIL_REQUIRE(
        TypeError, kj::str("Redirect URL cannot contain '\\r', '\\n', or '\\0': ", url));
  }

  // Build our headers object with `Location` set to the parsed URL.
  kj::HttpHeaders kjHeaders(IoContext::current().getHeaderTable());
  kjHeaders.set(kj::HttpHeaderId::LOCATION, kj::mv(parsedUrl));
  auto headers = js.alloc<Headers>(js, kjHeaders, Headers::Guard::IMMUTABLE);

  return js.alloc<Response>(js, statusCode, kj::none, kj::mv(headers), nullptr, kj::none);
}

jsg::Ref<Response> Response::json_(
    jsg::Lock& js, jsg::JsValue any, jsg::Optional<Initializer> maybeInit) {

  const auto maybeSetContentType = [](jsg::Lock& js, auto headers) {
    if (!headers->hasCommon(capnp::CommonHeaderName::CONTENT_TYPE)) {
      headers->setCommon(capnp::CommonHeaderName::CONTENT_TYPE, MimeType::JSON.toString());
    }
    return kj::mv(headers);
  };

  // While this all looks a bit complicated, all the following is doing is checking
  // to see if maybeInit contains a content-type header. If it does, the existing
  // value is left alone. If it does not, then we set the value of content-type
  // to the default content type for JSON payloads. The reason this all looks a bit
  // complicated is that maybeInit is an optional kj::OneOf that might be either
  // a dict or a jsg::Ref<Response>. If it is a dict, then the optional headers
  // field is also an optional kj::OneOf that can be either a dict or a jsg::Ref<Headers>.
  // We have to deal with all of the various possibilities here to set the content-type
  // appropriately.
  KJ_IF_SOME(init, maybeInit) {
    KJ_SWITCH_ONEOF(init) {
      KJ_CASE_ONEOF(dict, InitializerDict) {
        KJ_IF_SOME(headers, dict.headers) {
          dict.headers = maybeSetContentType(js, Headers::constructor(js, kj::mv(headers)));
        } else {
          dict.headers = maybeSetContentType(js, js.alloc<Headers>());
        }
      }
      KJ_CASE_ONEOF(res, jsg::Ref<Response>) {
        auto otherStatusText = res->getStatusText();
        auto newInit = InitializerDict{
          .status = res->statusCode,
          .statusText = otherStatusText == nullptr ||
                        otherStatusText == defaultStatusText(res->statusCode)
                        ? jsg::Optional<kj::String>() : kj::str(otherStatusText),
          .headers = maybeSetContentType(js, Headers::constructor(js, res->headers.addRef())),
          .cf = res->cf.getRef(js),
          .encodeBody =
              kj::str(res->bodyEncoding == Response::BodyEncoding::MANUAL ? "manual" : "automatic"),
        };

        KJ_IF_SOME(otherWs, res->webSocket) {
          newInit.webSocket = otherWs.addRef();
        }

        maybeInit = kj::mv(newInit);
      }
    }
  } else {
    maybeInit = InitializerDict{
      .headers = maybeSetContentType(js, js.alloc<Headers>()),
    };
  }

  return constructor(js, kj::Maybe(any.toJson(js)), kj::mv(maybeInit));
}

jsg::Ref<Response> Response::clone(jsg::Lock& js) {
  JSG_REQUIRE(
      webSocket == kj::none, TypeError, "Cannot clone a response to a WebSocket handshake.");

  auto headersClone = headers->clone(js);
  auto cfClone = cf.deepClone(js);

  auto bodyClone = Body::clone(js);

  auto urlListClone = KJ_MAP(url, urlList) { return kj::str(url); };

  return js.alloc<Response>(js, statusCode,
      mapCopyString(statusText),
      kj::mv(headersClone), kj::mv(cfClone), kj::mv(bodyClone), kj::mv(urlListClone));
}

kj::Promise<DeferredProxy<void>> Response::send(jsg::Lock& js,
    kj::HttpService::Response& outer,
    SendOptions options,
    kj::Maybe<const kj::HttpHeaders&> maybeReqHeaders) {
  JSG_REQUIRE(!getBodyUsed(), TypeError,
      "Body has already been used. "
      "It can only be used once. Use tee() first if you need to read it twice.");

  // Careful: Keep in mind that the promise we return could be canceled in which case `outer` will
  // be destroyed. Additionally, the response body stream we get from calling send() must itself
  // be destroyed before `outer` is destroyed. So, it's important to make sure that only the
  // promise we return encapsulates any task that might write to the response body. We can't, for
  // example, put the response body into a JS heap object. That should all be fine as long as we
  // use a pumpTo() that can be canceled.

  auto& context = IoContext::current();
  kj::HttpHeaders outHeaders(context.getHeaderTable());
  headers->shallowCopyTo(outHeaders);

  KJ_IF_SOME(ws, webSocket) {
    // `Response::acceptWebSocket()` can throw if we did not ask for a WebSocket. This
    // would promote a js client error into an uncatchable server error. Thus, we throw early here
    // if we do not expect a WebSocket. This could also be a 426 status code response, but we think
    // that the majority of our users expect us to throw on a client-side fetch error instead of
    // returning a 4xx status code. A 426 status code error _might_ be more appropriate if the
    // request headers originate from outside the worker developer's control (e.g. a client
    // application by some other party).
    JSG_REQUIRE(options.allowWebSocket, TypeError,
        "Worker tried to return a WebSocket in a response to a request "
        "which did not contain the header \"Upgrade: websocket\".");

    const bool hasEnabledWebSocketCompression = FeatureFlags::get(js).getWebSocketCompression();

    if (hasEnabledWebSocketCompression &&
        outHeaders.get(kj::HttpHeaderId::SEC_WEBSOCKET_EXTENSIONS) == kj::none) {
      // Since workerd uses `MANUAL_COMPRESSION` mode for websocket compression, we need to
      // pass the headers we want to support to `acceptWebSocket()`.
      KJ_IF_SOME(config, ws->getPreferredExtensions(kj::WebSocket::ExtensionsContext::RESPONSE)) {
        // We try to get extensions for use in a response (i.e. for a server side websocket).
        // This allows us to `optimizedPumpTo()` `webSocket`.
        outHeaders.set(kj::HttpHeaderId::SEC_WEBSOCKET_EXTENSIONS, kj::mv(config));
      } else {
        // `webSocket` is not a WebSocketImpl, we want to support whatever valid config the client
        // requested, so we'll just use the client's requested headers.
        KJ_IF_SOME(reqHeaders, maybeReqHeaders) {
          KJ_IF_SOME(value, reqHeaders.get(kj::HttpHeaderId::SEC_WEBSOCKET_EXTENSIONS)) {
            outHeaders.setPtr(kj::HttpHeaderId::SEC_WEBSOCKET_EXTENSIONS, value);
          }
        }
      }
    } else if (!hasEnabledWebSocketCompression) {
      // While we guard against an origin server including `Sec-WebSocket-Extensions` in a Response
      // (we don't send the extension in an offer, and if the server includes it in a response we
      // will reject the connection), a Worker could still explicitly add the header to a Response.
      outHeaders.unset(kj::HttpHeaderId::SEC_WEBSOCKET_EXTENSIONS);
    }

    auto clientSocket = outer.acceptWebSocket(outHeaders);
    auto wsPromise = ws->couple(kj::mv(clientSocket), context.getMetrics());

    KJ_IF_SOME(a, context.getActor()) {
      KJ_IF_SOME(hib, a.getHibernationManager()) {
        // We attach a reference to the deferred proxy task so the HibernationManager lives at least
        // as long as the websocket connection.
        // The actor still retains its reference to the manager, so any subsequent requests prior
        // to hibernation will not need to re-obtain a reference.
        wsPromise = wsPromise.attach(kj::addRef(hib));
      }
    }
    return wsPromise;
  } else KJ_IF_SOME(jsBody, getBody()) {
    auto encoding = getContentEncoding(context, outHeaders, bodyEncoding, FeatureFlags::get(js));
    auto maybeLength = jsBody->tryGetLength(encoding);
    auto stream =
        newSystemStream(outer.send(statusCode, getStatusText(), outHeaders, maybeLength), encoding);
    // We need to enter the AsyncContextFrame that was captured when the
    // Response was created before starting the loop.
    jsg::AsyncContextFrame::Scope scope(js, asyncContext);
    return jsBody->pumpTo(js, kj::mv(stream), true);
  } else {
    outer.send(statusCode, getStatusText(), outHeaders, static_cast<uint64_t>(0));
    return addNoopDeferredProxy(kj::READY_NOW);
  }
}

int Response::getStatus() {
  return statusCode;
}
kj::StringPtr Response::getStatusText() {
  KJ_IF_SOME(text, statusText) {
    return text;
  }
  return defaultStatusText(statusCode);
}
jsg::Ref<Headers> Response::getHeaders(jsg::Lock& js) {
  return headers.addRef();
}

bool Response::getOk() {
  return statusCode >= 200 && statusCode < 300;
}
bool Response::getRedirected() {
  return urlList.size() > 1;
}
kj::StringPtr Response::getUrl() {
  if (urlList.size() > 0) {
    // We're supposed to drop any fragment from the URL. Instead of doing it here, we rely on the
    // code that calls the Response constructor (e.g. makeHttpResponse()) to drop the fragments
    // before giving the stringified URL to us.
    return urlList.back();
  } else {
    // Per spec, if the URL list is empty, we return an empty string. I dunno, man.
    return "";
  }
}

kj::Maybe<jsg::Ref<WebSocket>> Response::getWebSocket(jsg::Lock& js) {
  return webSocket.map([&](jsg::Ref<WebSocket>& ptr) { return ptr.addRef(); });
}

jsg::Optional<jsg::JsObject> Response::getCf(jsg::Lock& js) {
  return cf.get(js);
}

void Response::serialize(jsg::Lock& js,
    jsg::Serializer& serializer,
    const jsg::TypeHandler<InitializerDict>& initDictHandler,
    const jsg::TypeHandler<kj::Maybe<jsg::Ref<ReadableStream>>>& streamHandler) {
  serializer.write(js, jsg::JsValue(streamHandler.wrap(js, getBody())));

  // As with Request, we serialize the initializer dict as a JS object.
  serializer.write(js,
      jsg::JsValue(initDictHandler.wrap(js,
          InitializerDict{
            .status = statusCode == 200 ? jsg::Optional<int>() : statusCode,
            .statusText = statusText.map([](auto& txt) { return kj::str(txt); }),
            .headers = headers.addRef(),
            .cf = cf.getRef(js),

            // If a WebSocket is present, we'll try to serialize it. As of this writing, WebSocket
            // is not serializable, but we could add support for sending it over RPC in the future.
            //
            // Note we have to double-Maybe this, so that if no signal is present, the property is absent
            // instead of `null`.
            .webSocket =
                webSocket.map([](jsg::Ref<WebSocket>& s) -> kj::Maybe<jsg::Ref<WebSocket>> {
    return s.addRef();
  }),

            .encodeBody = bodyEncoding == BodyEncoding::AUTO ? jsg::Optional<kj::String>()
                                                             : kj::str("manual"),
          })));
}

jsg::Ref<Response> Response::deserialize(jsg::Lock& js,
    rpc::SerializationTag tag,
    jsg::Deserializer& deserializer,
    const jsg::TypeHandler<InitializerDict>& initDictHandler,
    const jsg::TypeHandler<kj::Maybe<jsg::Ref<ReadableStream>>>& streamHandler) {
  auto body = KJ_ASSERT_NONNULL(streamHandler.tryUnwrap(js, deserializer.readValue(js)));
  auto init = KJ_ASSERT_NONNULL(initDictHandler.tryUnwrap(js, deserializer.readValue(js)));

  // If the status code is zero, then it was an error response. We cannot
  // use the Response::constructor.
  KJ_IF_SOME(status, init.status) {
    if (status == 0) {
      return Response::error(js);
    }
  }

  return Response::constructor(js, kj::mv(body), kj::mv(init));
}

// =======================================================================================

jsg::Ref<Request> FetchEvent::getRequest() {
  return request.addRef();
}

kj::Maybe<jsg::Promise<jsg::Ref<Response>>> FetchEvent::getResponsePromise(jsg::Lock& js) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(_, AwaitingRespondWith) {
      state = ResponseSent();
      return kj::none;
    }
    KJ_CASE_ONEOF(called, RespondWithCalled) {
      auto result = kj::mv(called.promise);
      state = ResponseSent();
      return kj::mv(result);
    }
    KJ_CASE_ONEOF(_, ResponseSent) {
      KJ_FAIL_REQUIRE("can only call getResponsePromise() once");
    }
  }
  KJ_UNREACHABLE;
}

void FetchEvent::respondWith(jsg::Lock& js, jsg::Promise<jsg::Ref<Response>> promise) {
  preventDefault();

  if (IoContext::current().hasOutputGate()) {
    // Once a Response is returned, we need to apply the output lock.
    promise = promise.then(js, [](jsg::Lock& js, jsg::Ref<Response>&& response) {
      auto& context = IoContext::current();
      return context.awaitIo(js, context.waitForOutputLocks(),
          [response = kj::mv(response)](jsg::Lock&) mutable { return kj::mv(response); });
    });
  }

  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(_, AwaitingRespondWith) {
      state = RespondWithCalled{kj::mv(promise)};
    }
    KJ_CASE_ONEOF(called, RespondWithCalled) {
      JSG_FAIL_REQUIRE(DOMInvalidStateError,
          "FetchEvent.respondWith() has already been called; it can only be called once.");
    }
    KJ_CASE_ONEOF(_, ResponseSent) {
      JSG_FAIL_REQUIRE(DOMInvalidStateError,
          "Too late to call FetchEvent.respondWith(). It must be called synchronously in the "
          "event handler.");
    }
  }

  stopImmediatePropagation();
}

void FetchEvent::passThroughOnException() {
  IoContext::current().setFailOpen();
}

// =======================================================================================

namespace {

// Fetch spec requires (suggests?) 20: https://fetch.spec.whatwg.org/#http-redirect-fetch
constexpr auto MAX_REDIRECT_COUNT = 20;

jsg::Promise<jsg::Ref<Response>> handleHttpResponse(jsg::Lock& js,
    jsg::Ref<Fetcher> fetcher,
    jsg::Ref<Request> jsRequest,
    kj::Vector<kj::Url> urlList,
    kj::HttpClient::Response&& response);
jsg::Promise<jsg::Ref<Response>> handleHttpRedirectResponse(jsg::Lock& js,
    jsg::Ref<Fetcher> fetcher,
    jsg::Ref<Request> jsRequest,
    kj::Vector<kj::Url> urlList,
    uint status,
    kj::StringPtr location);

jsg::Promise<jsg::Ref<Response>> fetchImplNoOutputLock(jsg::Lock& js,
    jsg::Ref<Fetcher> fetcher,
    jsg::Ref<Request> jsRequest,
    kj::Vector<kj::Url> urlList) {
  KJ_ASSERT(!urlList.empty());

  auto& ioContext = IoContext::current();

  auto signal = jsRequest->getSignal();
  KJ_IF_SOME(s, signal) {
    // If the AbortSignal has already been triggered, then we need to stop here.
    if (s->getAborted(js)) {
      return js.rejectedPromise<jsg::Ref<Response>>(s->getReason(js));
    }
  }

  // Get client and trace context (if needed) in one clean call
  auto clientWithTracing = fetcher->getClientWithTracing(ioContext, jsRequest->serializeCfBlobJson(js), "fetch"_kjc);
  auto traceContext = kj::mv(clientWithTracing.traceContext);

  // TODO(cleanup): Don't convert to HttpClient. Use the HttpService interface instead. This
  //   requires a significant rewrite of the code below. It'll probably get simpler, though?
  kj::Own<kj::HttpClient> client = asHttpClient(kj::mv(clientWithTracing.client));

  // fetch() requests use a lot of unaccounted C++ memory, so we adjust memory usage to pressure
  // the GC and protect against OOMs.
  size_t adjustmentBytes = 3 * 1024;  // 3 KiB default
  if (util::Autogate::isEnabled(util::AutogateKey::INCREASE_EXTERNAL_MEMORY_ADJUSTMENT_FOR_FETCH)) {
    adjustmentBytes = 8 * 1024;
  }
  client = client.attach(js.getExternalMemoryAdjustment(adjustmentBytes));

  kj::HttpHeaders headers(ioContext.getHeaderTable());
  jsRequest->shallowCopyHeadersTo(headers);

  // If the jsRequest has a CacheMode, we need to handle that here.
  // Currently, the only cache mode we support is undefined and no-store, no-cache, and reload
  auto headerIds = ioContext.getHeaderIds();
  const auto cacheMode = jsRequest->getCacheMode();
  switch (cacheMode) {
    case Request::CacheMode::RELOAD:
      KJ_FALLTHROUGH;
    case Request::CacheMode::NOSTORE:
      KJ_FALLTHROUGH;
    case Request::CacheMode::NOCACHE:
      if (headers.get(headerIds.cacheControl) == kj::none) {
        headers.setPtr(headerIds.cacheControl, "no-cache");
      }
      if (headers.get(headerIds.pragma) == kj::none) {
        headers.setPtr(headerIds.pragma, "no-cache");
      }
      KJ_FALLTHROUGH;
    case Request::CacheMode::NONE:
      break;
    default:
      KJ_UNREACHABLE;
  }

  KJ_IF_SOME(ctx, traceContext) {
    ctx.setTag("network.protocol.name"_kjc, "http"_kjc);
    ctx.setTag("network.protocol.version"_kjc, "HTTP/1.1"_kjc);
    ctx.setTag("http.request.method"_kjc, kj::str(jsRequest->getMethodEnum()));
    ctx.setTag("url.full"_kjc, jsRequest->getUrl());

    KJ_IF_SOME(userAgent, headers.get(headerIds.userAgent)) {
      ctx.setTag("user_agent.original"_kjc, userAgent);
    }

    KJ_IF_SOME(contentType, headers.get(headerIds.contentType)) {
      ctx.setTag("http.request.header.content-type"_kjc, contentType);
    }

    KJ_IF_SOME(contentLength, headers.get(headerIds.contentLength)) {
      ctx.setTag("http.request.header.content-length"_kjc, contentLength);
    }

    KJ_IF_SOME(accept, headers.get(headerIds.accept)) {
      ctx.setTag("http.request.header.accept"_kjc, accept);
    }

    KJ_IF_SOME(acceptEncoding, headers.get(headerIds.acceptEncoding)) {
      ctx.setTag("http.request.header.accept-encoding"_kjc, acceptEncoding);
    }
  }

  kj::String url =
      uriEncodeControlChars(urlList.back().toString(kj::Url::HTTP_PROXY_REQUEST).asBytes());

  if (headers.isWebSocket()) {
    if (!FeatureFlags::get(js).getWebSocketCompression()) {
      // If we haven't enabled the websocket compression compatibility flag, strip the header from the
      // subrequest.
      headers.unset(kj::HttpHeaderId::SEC_WEBSOCKET_EXTENSIONS);
    }
    auto webSocketResponse = client->openWebSocket(url, headers);
    return ioContext.awaitIo(js,
        AbortSignal::maybeCancelWrap(js, signal, kj::mv(webSocketResponse)),
        [fetcher = kj::mv(fetcher), jsRequest = kj::mv(jsRequest), urlList = kj::mv(urlList),
            client = kj::mv(client), signal = kj::mv(signal)](
            jsg::Lock& js, kj::HttpClient::WebSocketResponse&& response) mutable
        -> jsg::Promise<jsg::Ref<Response>> {
      KJ_SWITCH_ONEOF(response.webSocketOrBody) {
        KJ_CASE_ONEOF(body, kj::Own<kj::AsyncInputStream>) {
          body = body.attach(kj::mv(client));
          return handleHttpResponse(js, kj::mv(fetcher), kj::mv(jsRequest), kj::mv(urlList),
              {response.statusCode, response.statusText, response.headers, kj::mv(body)});
        }
        KJ_CASE_ONEOF(webSocket, kj::Own<kj::WebSocket>) {
          KJ_ASSERT(response.statusCode == 101);
          webSocket = webSocket.attach(kj::mv(client));
          KJ_IF_SOME(s, signal) {
            // If the AbortSignal has already been triggered, then we need to stop here.
            if (s->getAborted(js)) {
              return js.rejectedPromise<jsg::Ref<Response>>(s->getReason(js));
            }
            webSocket = kj::refcounted<AbortableWebSocket>(kj::mv(webSocket), s->getCanceler());
          }
          return js.resolvedPromise(makeHttpResponse(js, jsRequest->getMethodEnum(),
              kj::mv(urlList), response.statusCode, response.statusText, *response.headers,
              newNullInputStream(), js.alloc<WebSocket>(kj::mv(webSocket)),
              jsRequest->getResponseBodyEncoding(), kj::mv(signal)));
        }
      }
      KJ_UNREACHABLE;
    });
  } else {
    kj::Maybe<kj::HttpClient::Request> nativeRequest;
    KJ_IF_SOME(jsBody, jsRequest->getBody()) {
      // Note that for requests, we do not automatically handle Content-Encoding, because the fetch()
      // standard does not say that we should. Hence, we always use StreamEncoding::IDENTITY.
      // https://github.com/whatwg/fetch/issues/589
      auto maybeLength = jsBody->tryGetLength(StreamEncoding::IDENTITY);
      KJ_IF_SOME(ctx, traceContext) {
        KJ_IF_SOME(length, maybeLength) {
          ctx.setTag("http.request.body.size"_kjc, static_cast<int64_t>(length));
        }
      }

      if (maybeLength.orDefault(1) == 0 &&
          headers.get(kj::HttpHeaderId::CONTENT_LENGTH) == kj::none &&
          headers.get(kj::HttpHeaderId::TRANSFER_ENCODING) == kj::none) {
        // Request has a non-null but explicitly empty body, and has neither a Content-Length nor
        // a Transfer-Encoding header. If we don't set one of those two, and the receiving end is
        // another worker (especially within a pipeline or reached via RPC, not real HTTP), then
        // the code in global-scope.c++ on the receiving end will decide the body should be null.
        // We'd like to avoid this weird discontinuity, so let's set Content-Length explicitly to
        // 0.
        headers.setPtr(kj::HttpHeaderId::CONTENT_LENGTH, "0"_kj);
      }

      KJ_IF_SOME(ctx, traceContext) {
        KJ_IF_SOME(cfRay, headers.get(headerIds.cfRay)) {
          ctx.setTag("cloudflare.ray_id"_kjc, cfRay);
        }
      }

      nativeRequest = client->request(jsRequest->getMethodEnum(), url, headers, maybeLength);
      auto& nr = KJ_ASSERT_NONNULL(nativeRequest);
      auto stream = newSystemStream(kj::mv(nr.body), StreamEncoding::IDENTITY);

      // We want to support bidirectional streaming, so we actually don't want to wait for the
      // request to finish before we deliver the response to the app.

      // jsBody is not used directly within the function but is passed in so that
      // the coroutine frame keeps it alive.
      static constexpr auto handleCancelablePump = [](kj::Promise<void> promise,
                                                       auto jsBody) -> kj::Promise<void> {
        try {
          co_await promise;
        } catch (...) {
          auto exception = kj::getCaughtExceptionAsKj();
          if (exception.getType() != kj::Exception::Type::DISCONNECTED) {
            kj::throwFatalException(kj::mv(exception));
          }
          // Ignore DISCONNECTED exceptions thrown by the writePromise, so that we always
          // return the server's response, which should identify if any issue occurred with
          // the body stream anyway.
        }
      };

      // TODO(someday): Allow deferred proxying for bidirectional streaming.
      ioContext.addWaitUntil(handleCancelablePump(
          AbortSignal::maybeCancelWrap(
              js, signal, ioContext.waitForDeferredProxy(jsBody->pumpTo(js, kj::mv(stream), true))),
          jsBody.addRef()));
    } else {
      nativeRequest = client->request(jsRequest->getMethodEnum(), url, headers, static_cast<uint64_t>(0));
    }
    return ioContext.awaitIo(js,
        AbortSignal::maybeCancelWrap(js, signal, kj::mv(KJ_ASSERT_NONNULL(nativeRequest).response))
            .catch_([](kj::Exception&& exception) -> kj::Promise<kj::HttpClient::Response> {
      if (exception.getDescription().startsWith("invalid Content-Length header value")) {
        return JSG_KJ_EXCEPTION(FAILED, Error, exception.getDescription());
      } else if (exception.getDescription().contains("NOSENTRY script not found")) {
        return JSG_KJ_EXCEPTION(FAILED, Error, "Worker not found.");
      }
      return kj::mv(exception);
    }),
        [fetcher = kj::mv(fetcher), jsRequest = kj::mv(jsRequest), urlList = kj::mv(urlList),
            client = kj::mv(client), traceContext = kj::mv(traceContext)](jsg::Lock& js,
            kj::HttpClient::Response&& response) mutable -> jsg::Promise<jsg::Ref<Response>> {
      response.body = response.body.attach(kj::mv(client));
      KJ_IF_SOME(ctx, traceContext) {
        ctx.setTag("http.response.status_code"_kjc, static_cast<int64_t>(response.statusCode));
        KJ_IF_SOME(length, response.body->tryGetLength()) {
          ctx.setTag("http.response.body.size"_kjc, static_cast<int64_t>(length));
        }
      }
      return handleHttpResponse(
          js, kj::mv(fetcher), kj::mv(jsRequest), kj::mv(urlList), kj::mv(response));
    });
  }
}

jsg::Promise<jsg::Ref<Response>> fetchImpl(jsg::Lock& js,
    jsg::Ref<Fetcher> fetcher,
    jsg::Ref<Request> jsRequest,
    kj::Vector<kj::Url> urlList) {
  auto& context = IoContext::current();
  // Optimization: For non-actors, which never have output locks, avoid the overhead of
  // awaitIo() and such by not going back to the event loop at all.
  KJ_IF_SOME(promise, context.waitForOutputLocksIfNecessary()) {
    return context.awaitIo(js, kj::mv(promise),
        [fetcher = kj::mv(fetcher), jsRequest = kj::mv(jsRequest), urlList = kj::mv(urlList)](
            jsg::Lock& js) mutable {
      return fetchImplNoOutputLock(js, kj::mv(fetcher), kj::mv(jsRequest), kj::mv(urlList));
    });
  } else {
    return fetchImplNoOutputLock(js, kj::mv(fetcher), kj::mv(jsRequest), kj::mv(urlList));
  }
}

jsg::Promise<jsg::Ref<Response>> handleHttpResponse(jsg::Lock& js,
    jsg::Ref<Fetcher> fetcher,
    jsg::Ref<Request> jsRequest,
    kj::Vector<kj::Url> urlList,
    kj::HttpClient::Response&& response) {
  auto signal = jsRequest->getSignal();

  KJ_IF_SOME(s, signal) {
    // If the AbortSignal has already been triggered, then we need to stop here.
    if (s->getAborted(js)) {
      return js.rejectedPromise<jsg::Ref<Response>>(s->getReason(js));
    }
    response.body = kj::refcounted<AbortableInputStream>(kj::mv(response.body), s->getCanceler());
  }

  if (isRedirectStatusCode(response.statusCode) &&
      jsRequest->getRedirectEnum() == Request::Redirect::FOLLOW) {
    KJ_IF_SOME(l, response.headers->get(kj::HttpHeaderId::LOCATION)) {

      // Pump the response body to a singleton null stream before following the redirect.
      auto& ioContext = IoContext::current();
      return ioContext.awaitIo(js,
          response.body->pumpTo(getGlobalNullOutputStream()).ignoreResult().attach(kj::mv(response.body)),
          [fetcher = kj::mv(fetcher), jsRequest = kj::mv(jsRequest), urlList = kj::mv(urlList),
           status = response.statusCode, location = kj::str(l)](jsg::Lock& js) mutable {
        return handleHttpRedirectResponse(
            js, kj::mv(fetcher), kj::mv(jsRequest), kj::mv(urlList), status, kj::mv(location));
      });
    } else {
      // No Location header. That's OK, we just return the response as is.
      // See https://fetch.spec.whatwg.org/#http-redirect-fetch step 2.
    }
  }

  auto result = makeHttpResponse(js, jsRequest->getMethodEnum(), kj::mv(urlList),
      response.statusCode, response.statusText, *response.headers, kj::mv(response.body), kj::none,
      jsRequest->getResponseBodyEncoding(), kj::mv(signal));

  return js.resolvedPromise(kj::mv(result));
}

jsg::Promise<jsg::Ref<Response>> handleHttpRedirectResponse(jsg::Lock& js,
    jsg::Ref<Fetcher> fetcher,
    jsg::Ref<Request> jsRequest,
    kj::Vector<kj::Url> urlList,
    uint status,
    kj::StringPtr location) {
  // Reconstruct the request body stream for retransmission in the face of a redirect. Before
  // reconstructing the stream, however, this function:
  //
  //   - Throws if `status` is non-303 and this request doesn't have a "rewindable" body.
  //   - Translates POST requests that hit 301, 302, or 303 into GET requests with null bodies.
  //   - Translates HEAD requests that hit 303 into HEAD requests with null bodies.
  //   - Translates all other requests that hit 303 into GET requests with null bodies.

  auto redirectedLocation = ([&]() -> kj::Maybe<kj::Url> {
    // TODO(later): This is a bit unfortunate. Per the fetch spec, we're supposed to be
    // using standard WHATWG URL parsing to resolve the redirect URL. However, changing it
    // now requires a compat flag. In order to minimize changes to the rest of the impl
    // we end up double parsing the URL here, once with the standard parser to produce the
    // correct result, and again with kj::Url in order to produce something that works with
    // the existing code. Fortunately the standard parser is fast but it would be nice to
    // be able to avoid the double parse at some point.
    if (FeatureFlags::get(js).getFetchStandardUrl()) {
      auto base = urlList.back().toString();
      KJ_IF_SOME(parsed, jsg::Url::tryParse(location, base.asPtr())) {
        auto str = kj::str(parsed.getHref());
        return kj::Url::tryParse(str.asPtr(), kj::Url::Context::REMOTE_HREF,
            kj::Url::Options{
              .percentDecode = false,
              .allowEmpty = true,
            });
      } else {
        return kj::none;
      }
    } else {
      return urlList.back().tryParseRelative(location);
    }
  })();

  if (redirectedLocation == kj::none) {
    auto exception =
        JSG_KJ_EXCEPTION(FAILED, TypeError, "Invalid Location header; unable to follow redirect.");
    return js.rejectedPromise<jsg::Ref<Response>>(kj::mv(exception));
  }

  // Note: RFC7231 says we should propagate fragments from the current request URL to the
  //   redirected URL. The Fetch spec seems to take the position that that's the navigator's
  //   job -- i.e., that you should be using redirect manual mode and deciding what to do with
  //   fragments in Location headers yourself. We follow the spec, and don't do any explicit
  //   fragment propagation.

  if (urlList.size() - 1 >= MAX_REDIRECT_COUNT) {
    auto exception = JSG_KJ_EXCEPTION(FAILED, TypeError, "Too many redirects.", urlList);
    return js.rejectedPromise<jsg::Ref<Response>>(kj::mv(exception));
  }

  if (FeatureFlags::get(js).getStripAuthorizationOnCrossOriginRedirect()) {
    auto base = urlList.back().toString();

    auto currentUrl = KJ_UNWRAP_OR(jsg::Url::tryParse(base.asPtr()), {
      auto exception =
        JSG_KJ_EXCEPTION(FAILED, TypeError, "Invalid current URL; unable to follow redirect.");
      return js.rejectedPromise<jsg::Ref<Response>>(kj::mv(exception));
    });

    auto locationUrl = KJ_UNWRAP_OR(jsg::Url::tryParse(location, base.asPtr()), {
      auto exception =
        JSG_KJ_EXCEPTION(FAILED, TypeError, "Invalid Location header; unable to follow redirect.");
      return js.rejectedPromise<jsg::Ref<Response>>(kj::mv(exception));
    });

    if (currentUrl.getOrigin() != locationUrl.getOrigin()) {
      // If request’s current URL’s origin is not same origin with locationURL’s origin, then
      // for each headerName of CORS non-wildcard request-header name, delete headerName from
      // request’s header list.
      // -- Fetch spec s. 4.4.13
      // <https://fetch.spec.whatwg.org/#http-redirect-fetch>
      //  (NB: "CORS non-wildcard request-header name" consists solely of "Authorization")

      jsRequest->getHeaders(js)->deleteCommon(capnp::CommonHeaderName::AUTHORIZATION);
    }
  }

  urlList.add(kj::mv(KJ_ASSERT_NONNULL(redirectedLocation)));

  // "If actualResponse’s status is not 303, request’s body is non-null, and
  //   request’s body’s source [buffer] is null, then return a network error."
  //   https://fetch.spec.whatwg.org/#http-redirect-fetch step 9.
  //
  // TODO(conform): this check pedantically enforces the spec, even if a POST hits a 301 or
  //   302. In that case, we're going to null out the body, anyway, so it feels strange to
  //   report an error. If we widen fetch()'s contract to allow POSTs with non-buffer-backed
  //   bodies to survive 301/302 redirects, our logic would get simpler here.
  //
  //   Follow up with the spec authors about this.
  if (status != 303 && !jsRequest->canRewindBody()) {
    auto exception = JSG_KJ_EXCEPTION(FAILED, TypeError,
        "A request with a one-time-use body (it was initialized from a stream, not a buffer) "
        "encountered a redirect requiring the body to be retransmitted. To avoid this error "
        "in the future, construct this request from a buffer-like body initializer.");
    return js.rejectedPromise<jsg::Ref<Response>>(kj::mv(exception));
  }

  auto method = jsRequest->getMethodEnum();

  // "If either actualResponse’s status is 301 or 302 and request’s method is
  //   `POST`, or actualResponse’s status is 303 and request's method is not `HEAD`, set request’s
  //   method to `GET` and request’s body to null."
  //   https://fetch.spec.whatwg.org/#http-redirect-fetch step 11.
  if (((status == 301 || status == 302) && method == kj::HttpMethod::POST) ||
      (status == 303 && method != kj::HttpMethod::HEAD)) {
    // TODO(conform): When translating a request with a body to a GET request, should we
    //   explicitly remove Content-* headers? See https://github.com/whatwg/fetch/issues/609
    jsRequest->setMethodEnum(kj::HttpMethod::GET);
    jsRequest->nullifyBody();
  } else {
    // Reconstruct the stream from our buffer. The spec does not specify that we should cancel the
    // current body transmission in HTTP/1.1, so I'm not neutering the stream. (For HTTP/2 it asks
    // us to send a RST_STREAM frame if possible.)
    //
    // We know `buffer` is non-null here because we checked `buffer`'s nullness when non-303, and
    // nulled out `impl` when 303. Combined, they guarantee that we have a backing buffer.
    jsRequest->rewindBody(js);
  }

  // No need to wait for output locks again when following a redirect, because we didn't interact
  // with the app state in any way.
  return fetchImplNoOutputLock(js, kj::mv(fetcher), kj::mv(jsRequest), kj::mv(urlList));
}

}  // namespace

jsg::Ref<Response> makeHttpResponse(jsg::Lock& js,
    kj::HttpMethod method,
    kj::Vector<kj::Url> urlListParam,
    uint statusCode,
    kj::StringPtr statusText,
    const kj::HttpHeaders& headers,
    kj::Own<kj::AsyncInputStream> body,
    kj::Maybe<jsg::Ref<WebSocket>> webSocket,
    Response::BodyEncoding bodyEncoding,
    kj::Maybe<jsg::Ref<AbortSignal>> signal) {
  auto responseHeaders = js.alloc<Headers>(js, headers, Headers::Guard::RESPONSE);
  auto& context = IoContext::current();

  // The Fetch spec defines responses to HEAD or CONNECT requests, or responses with null body
  // statuses, as having null bodies.
  // See https://fetch.spec.whatwg.org/#main-fetch step 21.
  //
  // Note that we don't handle the CONNECT case here because kj-http handles CONNECT specially,
  // and the Fetch spec doesn't allow users to create Requests with CONNECT methods.
  kj::Maybe<Body::ExtractedBody> responseBody = kj::none;
  if (method != kj::HttpMethod::HEAD && !isNullBodyStatusCode(statusCode)) {
    responseBody = Body::ExtractedBody(js.alloc<ReadableStream>(context,
        newSystemStream(kj::mv(body),
            getContentEncoding(context, headers, bodyEncoding, FeatureFlags::get(js)))));
  }

  // The Fetch spec defines "response URLs" as having no fragments. Since the last URL in the list
  // is the one reported by Response::getUrl(), we nullify its fragment before serialization.
  kj::Array<kj::String> urlList;
  if (!urlListParam.empty()) {
    urlListParam.back().fragment = kj::none;
    urlList = KJ_MAP(url, urlListParam) { return url.toString(); };
  }

  // TODO(someday): Fill response CF blob from somewhere?
  kj::Maybe<kj::String> maybeStatusText = statusText == defaultStatusText(statusCode)
      ? kj::Maybe<kj::String>()
      : kj::str(statusText);
  return js.alloc<Response>(js, statusCode, kj::mv(maybeStatusText), kj::mv(responseHeaders),
      nullptr, kj::mv(responseBody), kj::mv(urlList), kj::mv(webSocket), bodyEncoding);
}

namespace {

jsg::Promise<jsg::Ref<Response>> fetchImplNoOutputLock(jsg::Lock& js,
    kj::Maybe<jsg::Ref<Fetcher>> fetcher,
    Request::Info requestOrUrl,
    jsg::Optional<Request::Initializer> requestInit) {
  // This use of evalNow() is obsoleted by the capture_async_api_throws compatibility flag, but
  // we need to keep it here for people who don't have that flag set.
  return js.evalNow([&] {
    // The spec requires us to call Request's constructor here, so we do. This is unfortunate, but
    // important for a few reasons:
    //
    // 1. If Request's constructor would throw, we must throw here, too.
    // 2. If `requestOrUrl` is a Request object, we must disturb its body immediately and leave it
    //    disturbed. The typical fetch() call will do this naturally, except those which encounter
    //    303 redirects: they become GET requests with null bodies, which could then be reused.
    // 3. Following from the previous point, we must not allow the original request's method to
    //    mutate.
    //
    // We could emulate these behaviors with various hacks, but just reconstructing the request up
    // front is robust, and won't add significant overhead compared to the rest of fetch().
    auto jsRequest = Request::constructor(js, kj::mv(requestOrUrl), kj::mv(requestInit));

    // Clear the request's signal if the 'ignoreForSubrequests' flag is set. This happens when
    // a request from an incoming fetch is passed-through to another fetch. We want to avoid
    // aborting the subrequest in that case.
    jsRequest->clearSignalIfIgnoredForSubrequest(js);

    // This URL list keeps track of redirections and becomes a source for Response's URL list. The
    // first URL in the list is the Request's URL (visible to JS via Request::getUrl()). The last URL
    // in the list is the Request's "current" URL (eventually visible to JS via Response::getUrl()).
    auto urlList = kj::Vector<kj::Url>(1 + MAX_REDIRECT_COUNT);

    jsg::Ref<Fetcher> actualFetcher = nullptr;
    KJ_IF_SOME(f, fetcher) {
      actualFetcher = kj::mv(f);
    } else KJ_IF_SOME(f, jsRequest->getFetcher()) {
      actualFetcher = kj::mv(f);
    } else {
      actualFetcher =
          js.alloc<Fetcher>(IoContext::NULL_CLIENT_CHANNEL, Fetcher::RequiresHostAndProtocol::YES);
    }

    KJ_IF_SOME(dataUrl, DataUrl::tryParse(jsRequest->getUrl())) {
      // If the URL is a data URL, we need to handle it specially.
      kj::Maybe<kj::Array<kj::byte>> maybeResponseBody;

      // The Fetch spec defines responses to HEAD or CONNECT requests, or responses with null body
      // statuses, as having null bodies.
      // See https://fetch.spec.whatwg.org/#main-fetch step 21.
      //
      // Note that we don't handle the CONNECT case here because kj-http handles CONNECT specially,
      // and the Fetch spec doesn't allow users to create Requests with CONNECT methods.
      if (jsRequest->getMethodEnum() == kj::HttpMethod::GET) {
        maybeResponseBody.emplace(dataUrl.releaseData());
      }

      auto headers = js.alloc<Headers>();
      headers->setCommon(capnp::CommonHeaderName::CONTENT_TYPE, dataUrl.getMimeType().toString());
      return js.resolvedPromise(Response::constructor(js, kj::mv(maybeResponseBody),
          Response::InitializerDict{
            .status = 200,
            .headers = kj::mv(headers),
          }));
    }

    urlList.add(actualFetcher->parseUrl(js, jsRequest->getUrl()));
    return fetchImplNoOutputLock(js, kj::mv(actualFetcher), kj::mv(jsRequest), kj::mv(urlList));
  });
}

}  // namespace

jsg::Promise<jsg::Ref<Response>> fetchImpl(jsg::Lock& js,
    kj::Maybe<jsg::Ref<Fetcher>> fetcher,
    Request::Info requestOrUrl,
    jsg::Optional<Request::Initializer> requestInit) {
  auto& context = IoContext::current();
  // Optimization: For non-actors, which never have output locks, avoid the overhead of
  // awaitIo() and such by not going back to the event loop at all.
  KJ_IF_SOME(promise, context.waitForOutputLocksIfNecessary()) {
    return context.awaitIo(js, kj::mv(promise),
        [fetcher = kj::mv(fetcher), requestOrUrl = kj::mv(requestOrUrl),
            requestInit = kj::mv(requestInit)](jsg::Lock& js) mutable {
      return fetchImplNoOutputLock(js, kj::mv(fetcher), kj::mv(requestOrUrl), kj::mv(requestInit));
    });
  } else {
    return fetchImplNoOutputLock(js, kj::mv(fetcher), kj::mv(requestOrUrl), kj::mv(requestInit));
  }
}

jsg::Ref<Socket> Fetcher::connect(
    jsg::Lock& js, AnySocketAddress address, jsg::Optional<SocketOptions> options) {
  return connectImpl(js, JSG_THIS, kj::mv(address), kj::mv(options));
}

jsg::Promise<jsg::Ref<Response>> Fetcher::fetch(jsg::Lock& js,
    kj::OneOf<jsg::Ref<Request>, kj::String> requestOrUrl,
    jsg::Optional<kj::OneOf<RequestInitializerDict, jsg::Ref<Request>>> requestInit) {
  return fetchImpl(js, JSG_THIS, kj::mv(requestOrUrl), kj::mv(requestInit));
}

kj::Maybe<jsg::Ref<JsRpcProperty>> Fetcher::getRpcMethod(jsg::Lock& js, kj::String name) {
  // This is like JsRpcStub::getRpcMethod(), but we also initiate a whole new JS RPC session
  // each time the method is called (handled by `getClientForOneCall()`, below).

  auto flags = FeatureFlags::get(js);
  if (!flags.getFetcherRpc() && !flags.getWorkerdExperimental()) {
    // We need to pretend that we haven't implemented a wildcard property, as unfortunately it
    // breaks some workers in the wild. We would, however, like to warn users who are trying to use
    // RPC so they understand why it isn't working.

    if (name == "idFromName") {
      // HACK specifically for itty-durable: We will not write any warning here, since itty-durable
      // automatically checks for this property on all bindings in an effort to discover Durable
      // Object namespaces. The warning would be confusing.
      //
      // Reported here: https://github.com/kwhitley/itty-durable/issues/48
    } else {
      IoContext::current().logWarningOnce(kj::str("WARNING: Tried to access method or property '",
          name,
          "' on a Service Binding or "
          "Durable Object stub. Are you trying to use RPC? If so, please enable the 'rpc' compat "
          "flag or update your compat date to 2024-04-03 or later (see "
          "https://developers.cloudflare.com/workers/configuration/compatibility-dates/ ). If you "
          "are not trying to use RPC, please note that in the future, this property (and all other "
          "property names) will appear to be present as an RPC method."));
    }

    return kj::none;
  }

  return getRpcMethodInternal(js, kj::mv(name));
}

kj::Maybe<jsg::Ref<JsRpcProperty>> Fetcher::getRpcMethodInternal(jsg::Lock& js, kj::String name) {
  // Same as getRpcMethod, but skips compatibility check to allow RPC to be used from bindings
  // attached to workers without rpc flag.

  // Do not return a method for `then`, otherwise JavaScript decides this is a thenable, i.e. a
  // custom Promise, which will mean a Promise that resolves to this object will attempt to chain
  // with it, which is not what you want!
  if (name == "then"_kj) return kj::none;

  return js.alloc<JsRpcProperty>(JSG_THIS, kj::mv(name));
}

rpc::JsRpcTarget::Client Fetcher::getClientForOneCall(
    jsg::Lock& js, kj::Vector<kj::StringPtr>& path) {
  auto& ioContext = IoContext::current();
  auto worker = getClient(ioContext, kj::none, "jsRpcSession"_kjc);
  auto event = kj::heap<api::JsRpcSessionCustomEvent>(
      JsRpcSessionCustomEvent::WORKER_RPC_EVENT_TYPE);

  auto result = event->getCap();

  // Arrange to cancel the CustomEvent if our I/O context is destroyed. But otherwise, we don't
  // actually care about the result of the event. If it throws, the membrane will already have
  // propagated the exception to any RPC calls that we're waiting on, so we even ignore errors
  // here -- otherwise they'll end up logged as "uncaught exceptions" even if they were, in fact,
  // caught elsewhere.
  ioContext.addTask(worker->customEvent(kj::mv(event)).attach(kj::mv(worker)).then([](auto&&) {
  }, [](kj::Exception&&) {}));

  // (Don't extend `path` because we're the root.)

  return result;
}

void Fetcher::serialize(jsg::Lock& js, jsg::Serializer& serializer) {
  auto channel = getSubrequestChannel(IoContext::current());
  channel->requireAllowsTransfer();

  KJ_IF_SOME(handler, serializer.getExternalHandler()) {
    KJ_IF_SOME(frankenvalueHandler, kj::tryDowncast<Frankenvalue::CapTableBuilder>(handler)) {
      // Encoding a Frankenvalue (e.g. for dynamic loopback props or dynamic isolate env).
      serializer.writeRawUint32(frankenvalueHandler.add(kj::mv(channel)));
      return;
    } else KJ_IF_SOME(rpcHandler, kj::tryDowncast<RpcSerializerExternalHandler>(handler)) {
      JSG_REQUIRE(FeatureFlags::get(js).getWorkerdExperimental(), DOMDataCloneError,
          "ServiceStub serialization requires the 'experimental' compat flag.");

      auto token = channel->getToken(IoChannelFactory::ChannelTokenUsage::RPC);
      rpcHandler.write([token = kj::mv(token)](rpc::JsValue::External::Builder builder) {
        builder.setSubrequestChannelToken(token);
      });
      return;
    }
    // TODO(someday): structuredClone() should have special handling that just reproduces the same
    //   local object. At present we have no way to recognize structuredClone() here though.
  }

  // The allow_irrevocable_stub_storage flag allows us to just embed the token inline. This format
  // is temporary, anyone using this will lose their data later.
  JSG_REQUIRE(FeatureFlags::get(js).getAllowIrrevocableStubStorage(), DOMDataCloneError,
      "ServiceStub cannot be serialized in this context.");
  serializer.writeLengthDelimited(channel->getToken(IoChannelFactory::ChannelTokenUsage::STORAGE));
}

jsg::Ref<Fetcher> Fetcher::deserialize(jsg::Lock& js,
    rpc::SerializationTag tag, jsg::Deserializer& deserializer) {
  KJ_IF_SOME(handler, deserializer.getExternalHandler()) {
    KJ_IF_SOME(frankenvalueHandler, kj::tryDowncast<Frankenvalue::CapTableReader>(handler)) {
      // Decoding a Frankenvalue (e.g. for dynamic loopback props or dynamic isolate env).
      auto& cap = KJ_REQUIRE_NONNULL(frankenvalueHandler.get(deserializer.readRawUint32()),
          "serialized ServiceStub had invalid cap table index");

      KJ_IF_SOME(channel, kj::tryDowncast<IoChannelFactory::SubrequestChannel>(cap)) {
        // Probably decoding dynamic ctx.props.
        return js.alloc<Fetcher>(IoContext::current().addObject(kj::addRef(channel)));
      } else KJ_IF_SOME(channel, kj::tryDowncast<IoChannelCapTableEntry>(cap)) {
        // Probably decoding dynamic isolate env.
        return js.alloc<Fetcher>(
            channel.getChannelNumber(IoChannelCapTableEntry::Type::SUBREQUEST),
            RequiresHostAndProtocol::YES, /*isInHouse=*/false);
      } else {
        KJ_FAIL_REQUIRE("ServiceStub capability in Frankenvalue is not a SubrequestChannel?");
      }
    } else KJ_IF_SOME(rpcHandler, kj::tryDowncast<RpcDeserializerExternalHandler>(handler)) {
      JSG_REQUIRE(FeatureFlags::get(js).getWorkerdExperimental(), DOMDataCloneError,
          "ServiceStub serialization requires the 'experimental' compat flag.");

      auto external = rpcHandler.read();
      KJ_REQUIRE(external.isSubrequestChannelToken());
      auto& ioctx = IoContext::current();
      auto channel = ioctx.getIoChannelFactory().subrequestChannelFromToken(
          IoChannelFactory::ChannelTokenUsage::RPC,
          external.getSubrequestChannelToken());
      return js.alloc<Fetcher>(ioctx.addObject(kj::mv(channel)));
    }
  }

  // The allow_irrevocable_stub_storage flag allows us to just embed the token inline. This format
  // is temporary, anyone using this will lose their data later.
  JSG_REQUIRE(FeatureFlags::get(js).getAllowIrrevocableStubStorage(), DOMDataCloneError,
      "ServiceStub cannot be deserialized in this context.");
  auto& ioctx = IoContext::current();
  auto channel = ioctx.getIoChannelFactory().subrequestChannelFromToken(
      IoChannelFactory::ChannelTokenUsage::STORAGE, deserializer.readLengthDelimitedBytes());
  return js.alloc<Fetcher>(ioctx.addObject(kj::mv(channel)));
}

static jsg::Promise<void> throwOnError(
    jsg::Lock& js, kj::StringPtr method, jsg::Promise<jsg::Ref<Response>> promise) {
  return promise.then(js, [method](jsg::Lock&, jsg::Ref<Response> response) {
    uint status = response->getStatus();
    // TODO(someday): Would be nice to attach the response to the JavaScript error, maybe? Or
    //   should people really use fetch() if they want to inspect error responses?
    JSG_REQUIRE(status >= 200 && status < 300, Error,
        kj::str("HTTP ", method, " request failed: ", response->getStatus(), " ",
            response->getStatusText()));
  });
}

static jsg::Promise<Fetcher::GetResult> parseResponse(
    jsg::Lock& js, jsg::Ref<Response> response, jsg::Optional<kj::String> type) {
  auto typeName =
      type.map([](const kj::String& s) -> kj::StringPtr { return s; }).orDefault("text");
  if (typeName == "stream") {
    KJ_IF_SOME(body, response->getBody()) {
      return js.resolvedPromise(Fetcher::GetResult(kj::mv(body)));
    } else {
      // Empty body.
      return js.resolvedPromise(Fetcher::GetResult(js.alloc<ReadableStream>(
          IoContext::current(), newSystemStream(newNullInputStream(), StreamEncoding::IDENTITY))));
    }
  }

  if (typeName == "text") {
    return response->text(js).then(js, [response = kj::mv(response)](jsg::Lock&, auto x) {
      return Fetcher::GetResult(kj::mv(x));
    });
  } else if (typeName == "arrayBuffer") {
    return response->arrayBuffer(js).then(js, [response = kj::mv(response)](jsg::Lock&, auto x) {
      return Fetcher::GetResult(kj::mv(x));
    });
  } else if (typeName == "json") {
    return response->json(js).then(js, [response = kj::mv(response)](jsg::Lock&, auto x) {
      return Fetcher::GetResult(kj::mv(x));
    });
  } else {
    JSG_FAIL_REQUIRE(TypeError,
        "Unknown response type. Possible types are \"text\", \"arrayBuffer\", "
        "\"json\", and \"stream\".");
  }
}

jsg::Promise<Fetcher::GetResult> Fetcher::get(
    jsg::Lock& js, kj::String url, jsg::Optional<kj::String> type) {
  RequestInitializerDict subInit;
  subInit.method = kj::str("GET");

  return fetchImpl(js, JSG_THIS, kj::mv(url), kj::mv(subInit))
      .then(js,
          [type = kj::mv(type)](
              jsg::Lock& js, jsg::Ref<Response> response) mutable -> jsg::Promise<GetResult> {
    uint status = response->getStatus();
    if (status == 404 || status == 410) {
      return js.resolvedPromise(GetResult(js.v8Ref(js.v8Null())));
    } else if (!response->getOk()) {
      // Manually construct exception so that we can incorporate method and status into the text
      // that JavaScript sees.
      // TODO(someday): Would be nice to attach the response to the JavaScript error, maybe? Or
      //   should people really use fetch() if they want to inspect error responses?
      JSG_FAIL_REQUIRE(Error,
          kj::str(
              "HTTP GET request failed: ", response->getStatus(), " ", response->getStatusText()));
    } else {
      return parseResponse(js, kj::mv(response), kj::mv(type));
    }
  });
}

jsg::Promise<void> Fetcher::put(jsg::Lock& js,
    kj::String url,
    Body::Initializer body,
    jsg::Optional<Fetcher::PutOptions> options) {
  // Note that this borrows liberally from fetchImpl(fetcher, request, init, isolate).
  // This use of evalNow() is obsoleted by the capture_async_api_throws compatibility flag, but
  // we need to keep it here for people who don't have that flag set.
  return throwOnError(js, "PUT", js.evalNow([&] {
    RequestInitializerDict subInit;
    subInit.method = kj::str("PUT");
    subInit.body = kj::mv(body);
    auto jsRequest = Request::constructor(js, kj::mv(url), kj::mv(subInit));
    auto urlList = kj::Vector<kj::Url>(1 + MAX_REDIRECT_COUNT);

    kj::Url parsedUrl = this->parseUrl(js, jsRequest->getUrl());

    // If any optional parameters were specified by the client, append them to
    // the URL's query parameters.
    KJ_IF_SOME(o, options) {
      KJ_IF_SOME(expiration, o.expiration) {
        parsedUrl.query.add(kj::Url::QueryParam{kj::str("expiration"), kj::str(expiration)});
      }
      KJ_IF_SOME(expirationTtl, o.expirationTtl) {
        parsedUrl.query.add(kj::Url::QueryParam{kj::str("expiration_ttl"), kj::str(expirationTtl)});
      }
    }

    urlList.add(kj::mv(parsedUrl));
    return fetchImpl(js, JSG_THIS, kj::mv(jsRequest), kj::mv(urlList));
  }));
}

jsg::Promise<void> Fetcher::delete_(jsg::Lock& js, kj::String url) {
  RequestInitializerDict subInit;
  subInit.method = kj::str("DELETE");
  return throwOnError(js, "DELETE", fetchImpl(js, JSG_THIS, kj::mv(url), kj::mv(subInit)));
}

jsg::Promise<Fetcher::QueueResult> Fetcher::queue(
    jsg::Lock& js, kj::String queueName, kj::Array<ServiceBindingQueueMessage> messages) {
  auto& ioContext = IoContext::current();

  auto encodedMessages = kj::heapArrayBuilder<IncomingQueueMessage>(messages.size());
  for (auto& msg: messages) {
    KJ_IF_SOME(b, msg.body) {
      JSG_REQUIRE(msg.serializedBody == kj::none, TypeError,
          "Expected one of body or serializedBody for each message");
      jsg::Serializer serializer(js,
          jsg::Serializer::Options{
            .version = 15,
            .omitHeader = false,
          });
      serializer.write(js, jsg::JsValue(b.getHandle(js)));
      encodedMessages.add(IncomingQueueMessage{.id = kj::mv(msg.id),
        .timestamp = msg.timestamp,
        .body = serializer.release().data,
        .attempts = msg.attempts});
    } else KJ_IF_SOME(b, msg.serializedBody) {
      encodedMessages.add(IncomingQueueMessage{.id = kj::mv(msg.id),
        .timestamp = msg.timestamp,
        .body = kj::mv(b),
        .attempts = msg.attempts});
    } else {
      JSG_FAIL_REQUIRE(TypeError, "Expected one of body or serializedBody for each message");
    }
  }

  // Only create worker interface after the error checks above to reduce overhead in case of errors.
  auto worker = getClient(ioContext, kj::none, "queue"_kjc);
  auto event = kj::refcounted<api::QueueCustomEvent>(QueueEvent::Params{
    .queueName = kj::mv(queueName),
    .messages = encodedMessages.finish(),
  });

  auto eventRef =
      kj::addRef(*event);  // attempt to work around windows-specific null pointer deref.
  return ioContext.awaitIo(js, worker->customEvent(kj::mv(eventRef)).attach(kj::mv(worker)),
      [event = kj::mv(event)](jsg::Lock& js, WorkerInterface::CustomEvent::Result result) {
    return Fetcher::QueueResult{
      .outcome = kj::str(result.outcome),
      .ackAll = event->getAckAll(),
      .retryBatch = event->getRetryBatch(),
      .explicitAcks = event->getExplicitAcks(),
      .retryMessages = event->getRetryMessages(),
    };
  });
}

jsg::Promise<Fetcher::ScheduledResult> Fetcher::scheduled(
    jsg::Lock& js, jsg::Optional<ScheduledOptions> options) {
  auto& ioContext = IoContext::current();
  auto worker = getClient(ioContext, kj::none, "scheduled"_kjc);

  auto scheduledTime = ioContext.now();
  auto cron = kj::String();
  KJ_IF_SOME(o, options) {
    KJ_IF_SOME(t, o.scheduledTime) {
      scheduledTime = t;
    }
    KJ_IF_SOME(c, o.cron) {
      cron = kj::mv(c);
    }
  }

  return ioContext.awaitIo(js,
      worker->runScheduled(scheduledTime, cron).attach(kj::mv(worker), kj::mv(cron)),
      [](jsg::Lock& js, WorkerInterface::ScheduledResult result) {
    return Fetcher::ScheduledResult{
      .outcome = kj::str(result.outcome),
      .noRetry = !result.retry,
    };
  });
}

kj::Own<WorkerInterface> Fetcher::getClient(
    IoContext& ioContext, kj::Maybe<kj::String> cfStr, kj::ConstString operationName) {
  auto clientWithTracing = getClientWithTracing(ioContext, kj::mv(cfStr), kj::mv(operationName));
  return clientWithTracing.client.attach(kj::mv(clientWithTracing.traceContext));
}

Fetcher::ClientWithTracing Fetcher::getClientWithTracing(
    IoContext& ioContext, kj::Maybe<kj::String> cfStr, kj::ConstString operationName) {
  KJ_SWITCH_ONEOF(channelOrClientFactory) {
    KJ_CASE_ONEOF(channel, uint) {
      // For channels, create trace context
      auto traceContext = ioContext.makeUserTraceSpan(kj::mv(operationName));
      auto client = ioContext.getSubrequestChannel(channel, isInHouse, kj::mv(cfStr), traceContext);
      return ClientWithTracing{kj::mv(client), kj::mv(traceContext)};
    }
    KJ_CASE_ONEOF(channel, IoOwn<IoChannelFactory::SubrequestChannel>) {
      auto traceContext = ioContext.makeUserTraceSpan(kj::mv(operationName));
      auto client = ioContext.getSubrequest(
          [&](TraceContext& tracing, IoChannelFactory& ioChannelFactory) {
        return channel->startRequest({.cfBlobJson = kj::mv(cfStr), .parentSpan = tracing.getInternalSpanParent()});
      }, {
        .inHouse = isInHouse,
        .wrapMetrics = !isInHouse,
        .existingTraceContext = traceContext,
      });
      return ClientWithTracing{kj::mv(client), kj::mv(traceContext)};
    }
    KJ_CASE_ONEOF(outgoingFactory, IoOwn<OutgoingFactory>) {
      // For outgoing factories, no trace context needed
      auto client = outgoingFactory->newSingleUseClient(kj::mv(cfStr));
      return ClientWithTracing{kj::mv(client), kj::none};
    }
    KJ_CASE_ONEOF(outgoingFactory, kj::Own<CrossContextOutgoingFactory>) {
      // For cross-context outgoing factories, no trace context needed
      auto client = outgoingFactory->newSingleUseClient(ioContext, kj::mv(cfStr));
      return ClientWithTracing{kj::mv(client), kj::none};
    }
  }
  KJ_UNREACHABLE;
}

kj::Own<IoChannelFactory::SubrequestChannel> Fetcher::getSubrequestChannel(IoContext& ioContext) {
  KJ_SWITCH_ONEOF(channelOrClientFactory) {
    KJ_CASE_ONEOF(channel, uint) {
      return ioContext.getIoChannelFactory().getSubrequestChannel(channel);
    }
    KJ_CASE_ONEOF(channel, IoOwn<IoChannelFactory::SubrequestChannel>) {
      return kj::addRef(*channel);
    }
    KJ_CASE_ONEOF(outgoingFactory, IoOwn<OutgoingFactory>) {
      return outgoingFactory->getSubrequestChannel();
    }
    KJ_CASE_ONEOF(outgoingFactory, kj::Own<CrossContextOutgoingFactory>) {
      return outgoingFactory->getSubrequestChannel(ioContext);
    }
  }
  KJ_UNREACHABLE;
}

kj::Url Fetcher::parseUrl(jsg::Lock& js, kj::StringPtr url) {
  // We need to prep the request's URL for transmission over HTTP. fetch() accepts URLs that have
  // "." and ".." components as well as fragments (stuff after '#'), all of which needs to be
  // removed/collapsed before the URL is HTTP-ready. Luckily our URL parser does all this if we
  // tell it the context is REMOTE_HREF.
  constexpr auto urlOptions = kj::Url::Options{.percentDecode = false, .allowEmpty = true};
  kj::Maybe<kj::Url> maybeParsed;
  if (this->requiresHost == RequiresHostAndProtocol::YES) {
    maybeParsed = kj::Url::tryParse(url, kj::Url::REMOTE_HREF, urlOptions);
  } else {
    // We don't require a protocol nor hostname, but we accept them. The easiest way to implement
    // this is to parse relative to a dummy URL.
    static const kj::Url FAKE =
        kj::Url::parse("https://fake-host/", kj::Url::REMOTE_HREF, urlOptions);
    maybeParsed = FAKE.tryParseRelative(url);
  }

  KJ_IF_SOME(p, maybeParsed) {
    if (p.scheme != "http" && p.scheme != "https") {
      // A non-HTTP scheme was requested. We should probably throw an exception, but historically
      // we actually went ahead and passed `X-Forwarded-Proto: whatever` to FL, which it happily
      // ignored if the protocol specified was not "https". Whoops. Unfortunately, some workers
      // in production have grown dependent on the bug. We'll have to use a runtime versioning flag
      // to fix this.

      if (FeatureFlags::get(js).getFetchRefusesUnknownProtocols()) {
        // Backwards-compatibility flag not enabled, so just fail.
        JSG_FAIL_REQUIRE(TypeError, kj::str("Fetch API cannot load: ", url));
      }

      if (p.scheme != nullptr && '0' <= p.scheme[0] && p.scheme[0] <= '9') {
        // First character of the scheme is a digit. This is a weird case: Normally the KJ URL
        // parser would treat a scheme starting with a digit as invalid. But, due to a bug,
        // `tryParseRelative()` does NOT treat it as invalid. So, we know we took the branch above
        // that used `tryParseRelative()` above. In any case, later stages of the runtime will
        // definitely try to parse this URL again and will reject it at that time, producing an
        // internal error. We might as well throw a transparent error here instead so that we don't
        // log a garbage sentry alert.
        JSG_FAIL_REQUIRE(TypeError, kj::str("Fetch API cannot load: ", url));
      }

      // In preview, log a warning in hopes that people fix this.
      kj::StringPtr more = nullptr;
      if (p.scheme == "ws" || p.scheme == "wss") {
        // Include some extra text for ws:// and wss:// specifically, since this is the most common
        // mistake.
        more = " Note that fetch() treats WebSockets as a special kind of HTTP request, "
               "therefore WebSockets should use 'http:'/'https:', not 'ws:'/'wss:'.";
      } else if (p.scheme == "ftp") {
        // Include some extra text for ftp://, since we see this sometimes.
        more = " fetch() does not support the FTP protocol.";
      }
      IoContext::current().logWarning(
          kj::str("Worker passed an invalid URL to fetch(). URLs passed to fetch() must begin with "
                  "either 'http:' or 'https:', not '",
              p.scheme,
              ":'. Due to a historical bug, any "
              "other protocol used here will be treated the same as 'http:'. We plan to correct "
              "this bug in the future, so please update your Worker to use 'http:' or 'https:' for "
              "all fetch() URLs.",
              more));
    }

    return kj::mv(p);
  } else {
    JSG_FAIL_REQUIRE(TypeError, kj::str("Fetch API cannot load: ", url));
  }
}
}  // namespace workerd::api
