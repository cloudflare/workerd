// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/util/abortable.h>
#include <kj/compat/http.h>
#include <map>
#include "basics.h"
#include "streams.h"
#include "form-data.h"
#include "web-socket.h"
#include "url.h"
#include "blob.h"
#include <workerd/io/compatibility-date.capnp.h>

namespace workerd::api {

class Headers: public jsg::Object {
private:
  using EntryIteratorType = kj::Array<jsg::ByteString>;
  using KeyIteratorType = jsg::ByteString;
  using ValueIteratorType = jsg::ByteString;

  template <typename T>
  struct IteratorState {
    kj::Array<T> copy;
    decltype(copy.begin()) cursor = copy.begin();
  };

public:
  enum class Guard {
    IMMUTABLE,
    REQUEST,
    // REQUEST_NO_CORS,  // CORS not relevant on server side
    RESPONSE,
    NONE
  };

  struct DisplayedHeader {
    jsg::ByteString key;   // lower-cased name
    jsg::ByteString value; // comma-concatenation of all values seen
  };

  Headers(): guard(Guard::NONE) {}
  explicit Headers(jsg::Dict<jsg::ByteString, jsg::ByteString> dict);
  explicit Headers(const Headers& other);
  explicit Headers(const kj::HttpHeaders& other, Guard guard);

  Headers(Headers&&) = delete;
  Headers& operator=(Headers&&) = delete;

  jsg::Ref<Headers> clone() const;
  // Make a copy of this Headers object, and preserve the guard. The normal copy constructor sets
  // the copy's guard to NONE.

  void shallowCopyTo(kj::HttpHeaders& out);
  // Fill in the given HttpHeaders with these headers. Note that strings are inserted by
  // reference, so the output must be consumed immediately.

  bool hasLowerCase(kj::StringPtr name);
  // Like has(), but only call this with an already-lower-case `name`. Useful to avoid an
  // unnecessary string allocation. Not part of the JS interface.

  kj::Array<DisplayedHeader> getDisplayedHeaders();
  // Returns headers with lower-case name and comma-concatenated duplicates.

  using ByteStringPair = jsg::Sequence<jsg::ByteString>;
  using ByteStringPairs = jsg::Sequence<ByteStringPair>;

  using Initializer = kj::OneOf<jsg::Ref<Headers>,
                                ByteStringPairs,
                                jsg::Dict<jsg::ByteString, jsg::ByteString>>;
  // Per the fetch specification, it is possible to initialize a Headers object
  // from any other object that has a Symbol.iterator implementation. Those are
  // handled in this Initializer definition using the ByteStringPairs definition
  // that aliases jsg::Sequence<jsg::Sequence<jsg::ByteString>>. Technically,
  // the Headers object itself falls under that definition as well. However, treating
  // a Headers object as a jsg::Sequence<jsg::Sequence<T>> is nowhere near as
  // performant and has the side effect of forcing all header names to be lower-cased
  // rather than case-preserved. Instead of following the spec exactly here, we
  // choose to special case creating a Header object from another Header object.
  // This is an intentional departure from the spec.

  static jsg::Ref<Headers> constructor(jsg::Lock& js, jsg::Optional<Initializer> init);
  kj::Maybe<jsg::ByteString> get(jsg::ByteString name);
  kj::ArrayPtr<jsg::ByteString> getAll(jsg::ByteString name);
  bool has(jsg::ByteString name);
  void set(jsg::ByteString name, jsg::ByteString value);
  void append(jsg::ByteString name, jsg::ByteString value);
  void delete_(jsg::ByteString name);
  void forEach(jsg::Lock& js,
      jsg::V8Ref<v8::Function>,
      jsg::Optional<jsg::Value>);

  JSG_ITERATOR(EntryIterator, entries,
                EntryIteratorType,
                IteratorState<DisplayedHeader>,
                iteratorNext<EntryIteratorType>)
  JSG_ITERATOR(KeyIterator, keys,
                KeyIteratorType,
                IteratorState<jsg::ByteString>,
                iteratorNext<KeyIteratorType>)
  JSG_ITERATOR(ValueIterator, values,
                ValueIteratorType,
                IteratorState<jsg::ByteString>,
                iteratorNext<KeyIteratorType>)

  // JavaScript API.

  JSG_RESOURCE_TYPE(Headers) {
    JSG_METHOD(get);
    JSG_METHOD(getAll);
    JSG_METHOD(has);
    JSG_METHOD(set);
    JSG_METHOD(append);
    JSG_METHOD_NAMED(delete, delete_);
    JSG_METHOD(forEach);
    JSG_METHOD(entries);
    JSG_METHOD(keys);
    JSG_METHOD(values);

    JSG_ITERABLE(entries);

    JSG_TS_DEFINE(type HeadersInit = Headers | Iterable<Iterable<string>> | Record<string, string>);
    // All type aliases get inlined when exporting RTTI, but this type alias is included by
    // the official TypeScript types, so users might be depending on it.

    JSG_TS_OVERRIDE({
      constructor(init?: HeadersInit);

      entries(): IterableIterator<[key: string, value: string]>;
      [Symbol.iterator](): IterableIterator<[key: string, value: string]>;

      forEach<This = unknown>(callback: (this: This, value: string, key: string, parent: Headers) => void, thisArg?: This): void;
    });
  }

private:
  struct Header {
    jsg::ByteString key;   // lower-cased name
    jsg::ByteString name;
    kj::Vector<jsg::ByteString> values;
    // We intentionally do not comma-concatenate header values of the same name, as we need to be
    // able to re-serialize them separately. This is particularly important for the Set-Cookie
    // header, which uses a date format that requires a comma. This would normally suggest using a
    // std::multimap, but we also need to be able to display the values in comma-concatenated form
    // via Headers.entries()[1] in order to be Fetch-conformant. Storing a vector of strings in a
    // std::map makes this easier, and also makes it easy to honor the "first header name casing is
    // used for all duplicate header names" rule[2] that the Fetch spec mandates.
    //
    // See: 1: https://fetch.spec.whatwg.org/#concept-header-list-sort-and-combine
    //      2: https://fetch.spec.whatwg.org/#concept-header-list-append

    explicit Header(jsg::ByteString key, jsg::ByteString name,
                    kj::Vector<jsg::ByteString> values)
        : key(kj::mv(key)), name(kj::mv(name)), values(kj::mv(values)) {}
    explicit Header(jsg::ByteString key, jsg::ByteString name, jsg::ByteString value)
        : key(kj::mv(key)), name(kj::mv(name)), values(1) {
      values.add(kj::mv(value));
    }
  };

  Guard guard;
  std::map<kj::StringPtr, Header> headers;

  void checkGuard() {
    JSG_REQUIRE(guard == Guard::NONE, TypeError, "Can't modify immutable headers.");
  }

  template <typename Type>
  static kj::Maybe<Type> iteratorNext(jsg::Lock& js, auto& state) {
    if (state.cursor == state.copy.end()) {
      return nullptr;
    }
    auto& ret = *state.cursor++;
    if constexpr (kj::isSameType<Type, EntryIteratorType>()) {
      return kj::arr(kj::mv(ret.key), kj::mv(ret.value));
    } else if constexpr (kj::isSameType<Type, KeyIteratorType>()) {
      return kj::mv(ret);
    } else if constexpr (kj::isSameType<Type, ValueIteratorType>()) {
      return kj::mv(ret);
    } else {
      KJ_UNREACHABLE;
    }
  }
};

class Body: public jsg::Object {
  // Base class for Request and Response. In JavaScript, this class is a mixin, meaning no one will
  // be instantiating objects of this type -- it exists solely to house body-related functionality
  // common to both Requests and Responses.

public:
  using Initializer = kj::OneOf<jsg::Ref<ReadableStream>, kj::String, kj::Array<byte>,
                                jsg::Ref<Blob>, jsg::Ref<URLSearchParams>, jsg::Ref<FormData>>;
  // The types of objects from which a Body can be created.
  //
  // If the object is a ReadableStream, Body will adopt it directly; otherwise the object is some
  // sort of buffer-like source. In this case, Body will store its own ReadableStream that wraps the
  // source, and it keeps a reference to the source object around. This allows Requests and
  // Responses created from Strings, ArrayBuffers, FormDatas, Blobs, or URLSearchParams to be
  // retransmitted.
  //
  // For an example of where this is important, consider a POST Request in redirect-follow mode and
  // containing a body: if passed to a fetch() call that results in a 307 or 308 response, fetch()
  // will re-POST to the new URL. If the body was constructed from a ReadableStream, this re-POST
  // will fail, because there is no body source left. On the other hand, if the body was constructed
  // from any of the other source types, Body can create a new ReadableStream from the source, and
  // the POST will successfully retransmit.

  struct RefcountedBytes final: public kj::Refcounted {
    kj::Array<kj::byte> bytes;
    RefcountedBytes(kj::Array<kj::byte>&& bytes): bytes(kj::mv(bytes)) {}
  };

  struct Buffer {
    // The Fetch spec calls this type the body's "source", even though it really is a buffer. I end
    // talking about things like "a buffer-backed body", whereas in standardese I should say
    // "a body with a non-null source".
    //
    // I find that confusing, so let's just call it what it is: a Body::Buffer.

    kj::OneOf<kj::Own<RefcountedBytes>, jsg::Ref<Blob>> ownBytes;
    // In order to reconstruct buffer-backed ReadableStreams without gratuitous array copying, we
    // need to be able to tie the lifetime of the source buffer to the lifetime of the
    // ReadableStream's native stream, AND the lifetime of the Body itself. Thus we need
    // refcounting.
    //
    // NOTE: ownBytes may contain a v8::Global reference, hence instances of `Buffer` must exist
    //   only within the V8 heap space.
    // TODO(cleanup): When we integrate with V8's garbage collection APIs, we need to account for
    //   that here.

    kj::ArrayPtr<const kj::byte> view;
    // Bodies constructed from buffers rather than ReadableStreams can be retransmitted if necessary
    // (e.g. for redirects, authentication). In these cases, we need to keep an ArrayPtr view onto
    // the Array source itself, because the source may be a string, and thus have a trailing nul
    // byte.

    Buffer() = default;
    Buffer(kj::Array<kj::byte> array)
        : ownBytes(kj::refcounted<RefcountedBytes>(kj::mv(array))),
          view(ownBytes.get<kj::Own<RefcountedBytes>>()->bytes) {}
    Buffer(kj::String string)
        : ownBytes(kj::refcounted<RefcountedBytes>(string.releaseArray().releaseAsBytes())),
          view([this] {
            auto bytesIncludingNull = ownBytes.get<kj::Own<RefcountedBytes>>()->bytes.asPtr();
            return bytesIncludingNull.slice(0, bytesIncludingNull.size() - 1);
          }()) {}
    Buffer(jsg::Ref<Blob> blob)
        : ownBytes(kj::mv(blob)),
          view(ownBytes.get<jsg::Ref<Blob>>()->getData()) {}

    Buffer clone(jsg::Lock& js);
  };

  struct Impl {
    jsg::Ref<ReadableStream> stream;
    kj::Maybe<Buffer> buffer;
  };

  struct ExtractedBody {
    ExtractedBody(jsg::Ref<ReadableStream> stream,
                  kj::Maybe<Buffer> source = nullptr,
                  kj::Maybe<kj::String> contentType = nullptr);

    Impl impl;
    kj::Maybe<kj::String> contentType;
  };
  static ExtractedBody extractBody(jsg::Lock& js, Initializer init);
  // Implements the "extract a body" algorithm from the Fetch spec.
  // https://fetch.spec.whatwg.org/#concept-bodyinit-extract

  explicit Body(kj::Maybe<ExtractedBody> init, Headers& headers);

  kj::Maybe<Buffer> getBodyBuffer(jsg::Lock& js);

  // The following body rewind/nullification functions are helpers for implementing fetch() redirect
  // handling.

  bool canRewindBody();
  // True if this body is null or buffer-backed, false if this body is a ReadableStream.

  void rewindBody(jsg::Lock& js);
  // Reconstruct this body from its backing buffer. Precondition: `canRewindBody() == true`.

  void nullifyBody();
  // Convert this body into a null body.

  // ---------------------------------------------------------------------------
  // JS API

  kj::Maybe<jsg::Ref<ReadableStream>> getBody();
  bool getBodyUsed();
  jsg::Promise<kj::Array<byte>> arrayBuffer(jsg::Lock& js);
  jsg::Promise<kj::String> text(jsg::Lock& js);
  jsg::Promise<jsg::Ref<FormData>> formData(jsg::Lock& js,
      CompatibilityFlags::Reader featureFlags);
  jsg::Promise<jsg::Value> json(jsg::Lock& js);
  jsg::Promise<jsg::Ref<Blob>> blob(jsg::Lock& js);

  JSG_RESOURCE_TYPE(Body, CompatibilityFlags::Reader flags) {
    if (flags.getJsgPropertyOnPrototypeTemplate()) {
      JSG_READONLY_PROTOTYPE_PROPERTY(body, getBody);
      JSG_READONLY_PROTOTYPE_PROPERTY(bodyUsed, getBodyUsed);
    } else {
      JSG_READONLY_INSTANCE_PROPERTY(body, getBody);
      JSG_READONLY_INSTANCE_PROPERTY(bodyUsed, getBodyUsed);
    }
    JSG_METHOD(arrayBuffer);
    JSG_METHOD(text);
    JSG_METHOD(json);
    JSG_METHOD(formData);
    JSG_METHOD(blob);

    JSG_TS_DEFINE(type BodyInit = ReadableStream<Uint8Array> | string | ArrayBuffer | ArrayBufferView | Blob | URLSearchParams | FormData);
    // All type aliases get inlined when exporting RTTI, but this type alias is included by
    // the official TypeScript types, so users might be depending on it.
    JSG_TS_OVERRIDE({ json<T>(): Promise<T>; });
    // Allow JSON body type to be specified
  }

protected:
  kj::Maybe<ExtractedBody> clone(jsg::Lock& js);
  // Helper to implement Request/Response::clone().

private:
  kj::Maybe<Impl> impl;
  Headers& headersRef;
  // HACK: This `headersRef` variable refers to a Headers object in the Request/Response subclass.
  //   As such, it will briefly dangle during object destruction. While unlikely to be an issue,
  //   it's worth being aware of.

  void visitForGc(jsg::GcVisitor& visitor) {
    KJ_IF_MAYBE(i, impl) {
      visitor.visit(i->stream);
    }
  }
};

class Request;
class Response;
struct RequestInitializerDict;

class Socket;
struct SocketOptions;

class Fetcher: public jsg::Object {
  // A capability to send HTTP requests to some destination other than the public internet.
  // This is the type of `request.fetcher` (if it is not null).
  //
  // Actually, this interface could support more than just HTTP. This interface is really the
  // JavaScript wapper around `WorkerInterface`. It is the type used by worker-to-worker bindings.
  // As we add support for non-HTTP event types that can be invoked remotely, they should be added
  // here.

public:
  enum class RequiresHostAndProtocol {
    // Should we use a fake https base url if we lack a scheme+authority?
    YES,
    NO
  };

  explicit Fetcher(uint channel, RequiresHostAndProtocol requiresHost, bool isInHouse = false)
      : channelOrClientFactory(channel), requiresHost(requiresHost), isInHouse(isInHouse) {}
  // `channel` is what to pass to IoContext::getSubrequestChannel() to get a WorkerInterface
  // representing this Fetcher. Note that different requests potentially have different client
  // objects because a WorkerInterface is a KJ I/O object and therefore tied to a thread.
  // Abstractly, within a worker instance, the same channel always refers to the same Fetcher, even
  // though the WorkerInterface object changes from request to request.
  //
  // If `requiresHost` is false, then requests using this Fetcher are allowed to specify a
  // URL that has no protocol or host.
  //
  // See pipeline.capnp or request-context.h for an explanation of `isInHouse`.

  class OutgoingFactory {
    // Used by Fetchers that use ad-hoc, single-use WorkerInterface instances, such as ones
    // created for Actors.
  public:
    virtual kj::Own<WorkerInterface> newSingleUseClient(kj::Maybe<kj::String> cfStr) = 0;
  };

  class CrossContextOutgoingFactory {
    // Used by Fetchers that obtain their HttpClient in a custom way, but which aren't tied
    // to a specific I/O context. The factory object moves with the isolate across threads and
    // contexts, and must work from any context.
  public:
    virtual kj::Own<WorkerInterface> newSingleUseClient(IoContext& context, kj::Maybe<kj::String> cfStr) = 0;
  };

  Fetcher(IoOwn<OutgoingFactory> outgoingFactory,
          RequiresHostAndProtocol requiresHost,
          bool isInHouse = false)
      : channelOrClientFactory(kj::mv(outgoingFactory)),
        requiresHost(requiresHost),
        isInHouse(isInHouse) {}
  // `outgoingFactory` is used for Fetchers that use ad-hoc WorkerInterface instances, such as ones
  // created for Actors.

  Fetcher(kj::Own<CrossContextOutgoingFactory> outgoingFactory,
          RequiresHostAndProtocol requiresHost,
          bool isInHouse = false)
      : channelOrClientFactory(kj::mv(outgoingFactory)),
        requiresHost(requiresHost),
        isInHouse(isInHouse) {}
  // `outgoingFactory` is used for Fetchers that use ad-hoc WorkerInterface instances, but doesn't
  // require an IoContext

  kj::Own<WorkerInterface> getClient(
      IoContext& ioContext,
      kj::Maybe<kj::String> cfStr,
      kj::StringPtr operationName);
  // Returns an `WorkerInterface` that is only valid for the lifetime of the current
  // `IoContext`.

  kj::Url parseUrl(jsg::Lock& js, kj::StringPtr url,
                   CompatibilityFlags::Reader featureFlags);
  // Wraps kj::Url::parse to take into account whether the Fetcher requires a host to be
  // specified on URLs, Fetcher-specific URL decoding options, and error handling.

  jsg::Ref<Socket> connect(
      jsg::Lock& js, kj::String address, jsg::Optional<SocketOptions> options,
      CompatibilityFlags::Reader featureFlags);

  jsg::Promise<jsg::Ref<Response>> fetch(
      jsg::Lock& js, kj::OneOf<jsg::Ref<Request>, kj::String> requestOrUrl,
      jsg::Optional<kj::OneOf<RequestInitializerDict, jsg::Ref<Request>>> requestInit,
      CompatibilityFlags::Reader featureFlags);

  using GetResult = kj::OneOf<jsg::Ref<ReadableStream>, kj::Array<byte>, kj::String, jsg::Value>;

  jsg::Promise<GetResult> get(
      jsg::Lock& js, kj::String url, jsg::Optional<kj::String> type,
      CompatibilityFlags::Reader featureFlags);

  struct PutOptions {
    // Optional parameter for passing options into a Fetcher::put. Initially
    // intended for supporting expiration times in KV bindings.

    jsg::Optional<int> expiration;
    jsg::Optional<int> expirationTtl;

    JSG_STRUCT(expiration, expirationTtl);
  };

  jsg::Promise<void> put(
      jsg::Lock& js, kj::String url, Body::Initializer body, jsg::Optional<PutOptions> options,
      CompatibilityFlags::Reader featureFlags);

  jsg::Promise<void> delete_(jsg::Lock& js, kj::String url,
      CompatibilityFlags::Reader featureFlags);

  JSG_RESOURCE_TYPE(Fetcher, CompatibilityFlags::Reader flags) {
    JSG_METHOD(fetch);
    if (flags.getTcpSocketsSupport()) {
      JSG_METHOD(connect);
    }

    JSG_METHOD(get);
    JSG_METHOD(put);
    JSG_METHOD_NAMED(delete, delete_);

    JSG_TS_OVERRIDE({
      fetch(input: RequestInfo, init?: RequestInit<RequestInitCfProperties>): Promise<Response>;
      get: never;
      put: never;
      delete: never;
    });
    // Add URL to `fetch` input, and omit method helpers from definition
  }

private:
  kj::OneOf<uint, kj::Own<CrossContextOutgoingFactory>, IoOwn<OutgoingFactory>> channelOrClientFactory;
  RequiresHostAndProtocol requiresHost;
  bool isInHouse;
};

struct RequestInitializerDict {
  // Type of the second parameter to Request's constructor. Also the type of the second parameter
  // to fetch().

  jsg::Optional<kj::String> method;
  jsg::Optional<Headers::Initializer> headers;

  jsg::Optional<kj::Maybe<Body::Initializer>> body;
  // The script author may specify an empty body either implicitly, by allowing this property to
  // be undefined, or explicitly, by setting this property to null. To support both cases, this
  // body initializer must be Optional<Maybe<Body::Initializer>>.

  jsg::Optional<kj::String> redirect;
  // follow, error, manual (default follow)

  jsg::Optional<kj::Maybe<jsg::Ref<Fetcher>>> fetcher;

  jsg::Optional<jsg::V8Ref<v8::Object>> cf;
  // Cloudflare-specific feature flags.
  //
  // TODO(someday): We should generalize this concept to sending control information to
  //   downstream workers in the pipeline. That is, when multiple workers apply to the same
  //   request (with the first worker's subrequests being passed to the next worker), then
  //   first worker should be able to set flags on the request that the second worker can see.
  //   Perhaps we should say that any field you set on a Request object will be JSON-serialized
  //   and passed on to the next worker? Then `cf` is just one such field: it's not special,
  //   it's only named `cf` because the consumer is Cloudflare code.

  jsg::WontImplement mode;
  jsg::WontImplement credentials;
  // These control CORS policy. This doesn't matter on the edge because CSRF is not possible
  // here:
  // 1. We don't have the user's credentials (e.g. cookies) for any other origin, so we couldn't
  //    forge a request from the user even if we wanted to.
  // 2. We aren't behind the user's firewall, so we also can't forge requests to unauthenticated
  //    internal network services.

  jsg::Unimplemented cache;
  // In browsers this controls the local browser cache. For Cloudflare Workers it could control the
  // Cloudflare edge cache. Note that this setting is different from using the `Cache-Control`
  // header since `Cache-Control` would be forwarded to the origin.

  jsg::WontImplement referrer;
  jsg::WontImplement referrerPolicy;
  // These control how the `Referer` and `Origin` headers are initialized by the browser.
  // Browser-side JavaScript is normally not permitted to set these headers, because servers
  // sometimes use the headers to defend against CSRF. On the edge, CSRF is not a risk (see
  // comments about `mode` and `credentials`, above), hence protecting the Referer and Origin
  // headers is not necessary, so we treat them as regular-old headers instead.

  jsg::Unimplemented integrity;
  // Subresource integrity (check response against a given hash).

  jsg::Optional<kj::Maybe<jsg::Ref<AbortSignal>>> signal;
  // The spec declares this optional, but is unclear on whether it is nullable. The spec is also
  // unclear on whether the `Request.signal` property is nullable. If `Request.signal` is nullable,
  // then we definitely have to accept `null` as an input here, otherwise
  // `new Request(url, {...request})` will fail when `request.signal` is null. However, it's also
  // possible that neither property should be nullable. Indeed, it appears that Chrome always
  // constructs a dummy signal even if none was provided, and uses that. But Chrome is also happy
  // to accept `null` as an input, so if we're doing what Chrome does, then we should accept
  // `null`.

  jsg::Unimplemented observe;
  // Functionality to exert fine-grained control over the fetch, including the ability to cancel
  // it.

  JSG_STRUCT(method, headers, body, redirect, fetcher, cf, mode, credentials, cache,
              referrer, referrerPolicy, integrity, signal, observe);
  JSG_STRUCT_TS_OVERRIDE(RequestInit<CfType = IncomingRequestCfProperties | RequestInitCfProperties> {
    headers?: HeadersInit;
    body?: BodyInit | null;
    cf?: CfType;
  });
};

class Request: public Body {
public:
  enum class Redirect {
    FOLLOW,
    MANUAL
    // Note: error mode doesn't make sense for us.
  };
  static kj::Maybe<Redirect> tryParseRedirect(kj::StringPtr redirect);

  Request(kj::HttpMethod method, kj::StringPtr url, Redirect redirect,
          jsg::Ref<Headers> headers, kj::Maybe<jsg::Ref<Fetcher>> fetcher,
          kj::Maybe<jsg::Ref<AbortSignal>> signal, kj::Maybe<jsg::V8Ref<v8::Object>> cf,
          kj::Maybe<Body::ExtractedBody> body)
    : Body(kj::mv(body), *headers), method(method), url(kj::str(url)),
      redirect(redirect), headers(kj::mv(headers)), fetcher(kj::mv(fetcher)),
      cf(kj::mv(cf)) {
    KJ_IF_MAYBE(s, signal) {
      // If the AbortSignal will never abort, assigning it to thisSignal instead ensures
      // that the cancel machinery is not used but the request.signal accessor will still
      // do the right thing.
      if ((*s)->getNeverAborts()) {
        this->thisSignal = kj::mv(*s);
      } else {
        this->signal = kj::mv(*s);
      }
    }
  }
  // TODO(conform): Technically, the request's URL should be parsed immediately upon Request
  //   construction, and any errors encountered should be thrown. Instead, we defer parsing until
  //   fetch()-time. This sidesteps an awkward issue: The request URL should be parsed relative to
  //   the service worker script's URL (e.g. https://capnproto.org/sw.js), but edge worker scripts
  //   don't have a script URL, so we have no choice but to parse it as an absolute URL. This means
  //   constructs like `new Request("")` should actually throw TypeError, but constructing Requests
  //   with empty URLs is useful in testing.

  kj::HttpMethod getMethodEnum() { return method; }
  void setMethodEnum(kj::HttpMethod newMethod) { method = newMethod; }
  Redirect getRedirectEnum() { return redirect; }
  void shallowCopyHeadersTo(kj::HttpHeaders& out);
  kj::Maybe<kj::String> serializeCfBlobJson(jsg::Lock& js);

  // ---------------------------------------------------------------------------
  // JS API

  typedef RequestInitializerDict InitializerDict;

  using Info = kj::OneOf<jsg::Ref<Request>, kj::String>;
  using Initializer = kj::OneOf<InitializerDict, jsg::Ref<Request>>;

  static jsg::Ref<Request> coerce(
      jsg::Lock& js,
      Request::Info input,
      jsg::Optional<Request::Initializer> init);
  // Wrapper around Request::constructor that calls it only if necessary, and returns a
  // jsg::Ref<Request>.
  //
  // C++ API, but declared down here because we need the InitializerDict type.

  static jsg::Ref<Request> constructor(
      jsg::Lock& js,
      Request::Info input,
      jsg::Optional<Request::Initializer> init);

  jsg::Ref<Request> clone(jsg::Lock& js);

  kj::StringPtr getMethod();
  kj::StringPtr getUrl();
  jsg::Ref<Headers> getHeaders(jsg::Lock& js);
  kj::StringPtr getRedirect();
  kj::Maybe<jsg::Ref<Fetcher>> getFetcher();

  kj::Maybe<jsg::Ref<AbortSignal>> getSignal();
  // getSignal() is the one that we used internally to determine if there's actually
  // an AbortSignal that can be triggered to cancel things. The getThisSignal() is
  // used only on the JavaScript side to conform to the spec, which requires
  // request.signal to always return an AbortSignal even if one is not actively
  // used on this request.

  jsg::Ref<AbortSignal> getThisSignal(jsg::Lock& js);

  jsg::Optional<v8::Local<v8::Object>> getCf(jsg::Lock& js);
  // Returns the `cf` field containing Cloudflare feature flags.

  jsg::WontImplement getContext() { return jsg::WontImplement(); }
  // This is deprecated in the spec.

  jsg::WontImplement getMode()        { return jsg::WontImplement(); }
  jsg::WontImplement getCredentials() { return jsg::WontImplement(); }
  jsg::Unimplemented getIntegrity()   { return jsg::Unimplemented(); }
  jsg::Unimplemented getCache()       { return jsg::Unimplemented(); }
  // See members of Initializer for commentary on unimplemented APIs.

  JSG_RESOURCE_TYPE(Request, CompatibilityFlags::Reader flags) {
    JSG_INHERIT(Body);

    JSG_METHOD(clone);

    JSG_TS_DEFINE(type RequestInfo = Request | string | URL);
    // All type aliases get inlined when exporting RTTI, but this type alias is included by
    // the official TypeScript types, so users might be depending on it.

    if (flags.getJsgPropertyOnPrototypeTemplate()) {
      JSG_READONLY_PROTOTYPE_PROPERTY(method, getMethod);
      JSG_READONLY_PROTOTYPE_PROPERTY(url, getUrl);
      JSG_READONLY_PROTOTYPE_PROPERTY(headers, getHeaders);
      JSG_READONLY_PROTOTYPE_PROPERTY(redirect, getRedirect);
      JSG_READONLY_PROTOTYPE_PROPERTY(fetcher, getFetcher);
      JSG_READONLY_PROTOTYPE_PROPERTY(signal, getThisSignal);
      JSG_READONLY_PROTOTYPE_PROPERTY(cf, getCf);

      JSG_READONLY_PROTOTYPE_PROPERTY(context, getContext);
      JSG_READONLY_PROTOTYPE_PROPERTY(mode, getMode);
      JSG_READONLY_PROTOTYPE_PROPERTY(credentials, getCredentials);
      JSG_READONLY_PROTOTYPE_PROPERTY(integrity, getIntegrity);
      JSG_READONLY_PROTOTYPE_PROPERTY(cache, getCache);

      JSG_TS_OVERRIDE(<CfHostMetadata = unknown> {
        constructor(input: RequestInfo, init?: RequestInit);
        clone(): Request<CfHostMetadata>;
        get cf(): IncomingRequestCfProperties<CfHostMetadata> | undefined;
      });
      // Use `RequestInfo` and `RequestInit` type aliases in constructor instead of inlining.
      // `IncomingRequestCfProperties` is defined in `/types/defines/cf.d.ts`.
    } else {
      JSG_READONLY_INSTANCE_PROPERTY(method, getMethod);
      JSG_READONLY_INSTANCE_PROPERTY(url, getUrl);
      JSG_READONLY_INSTANCE_PROPERTY(headers, getHeaders);
      JSG_READONLY_INSTANCE_PROPERTY(redirect, getRedirect);
      JSG_READONLY_INSTANCE_PROPERTY(fetcher, getFetcher);
      JSG_READONLY_INSTANCE_PROPERTY(signal, getThisSignal);
      JSG_READONLY_INSTANCE_PROPERTY(cf, getCf);

      JSG_READONLY_INSTANCE_PROPERTY(context, getContext);
      JSG_READONLY_INSTANCE_PROPERTY(mode, getMode);
      JSG_READONLY_INSTANCE_PROPERTY(credentials, getCredentials);
      JSG_READONLY_INSTANCE_PROPERTY(integrity, getIntegrity);
      JSG_READONLY_INSTANCE_PROPERTY(cache, getCache);

      JSG_TS_OVERRIDE(<CfHostMetadata = unknown> {
        constructor(input: RequestInfo, init?: RequestInit);
        clone(): Request<CfHostMetadata>;
        readonly cf?: IncomingRequestCfProperties<CfHostMetadata>;
      });
      // Use `RequestInfo` and `RequestInit` type aliases in constructor instead of inlining.
      // `IncomingRequestCfProperties` is defined in `/types/defines/cf.d.ts`.
    }
  }

private:
  kj::HttpMethod method;
  kj::String url;
  Redirect redirect;
  jsg::Ref<Headers> headers;
  kj::Maybe<jsg::Ref<Fetcher>> fetcher;
  kj::Maybe<jsg::Ref<AbortSignal>> signal;
  kj::Maybe<jsg::Ref<AbortSignal>> thisSignal;
  // The fetch spec definition of Request has a distinction between the "signal" (which is
  // an optional AbortSignal passed in with the options), and "this' signal", which is an
  // AbortSignal that is always available via the request.signal accessor. When signal is
  // used explicity, thisSignal will not be.
  kj::Maybe<jsg::V8Ref<v8::Object>> cf;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(headers, fetcher, signal, thisSignal, cf);
  }
};

class Response: public Body {
public:
  enum class BodyEncoding {
    AUTO,
    MANUAL
  };

  Response(int statusCode, kj::String statusText, jsg::Ref<Headers> headers,
           kj::Maybe<jsg::V8Ref<v8::Object>> cf, kj::Maybe<Body::ExtractedBody> body,
           CompatibilityFlags::Reader reader,
           kj::Array<kj::String> urlList = {},
           kj::Maybe<jsg::Ref<WebSocket>> webSocket = nullptr,
           Response::BodyEncoding bodyEncoding = Response::BodyEncoding::AUTO)
      : Body(kj::mv(body), *headers),
        statusCode(statusCode),
        statusText(kj::mv(statusText)),
        headers(kj::mv(headers)),
        cf(kj::mv(cf)),
        urlList(kj::mv(urlList)),
        webSocket(kj::mv(webSocket)),
        bodyEncoding(bodyEncoding),
        hasEnabledWebSocketCompression(reader.getWebSocketCompression()) {}

  // ---------------------------------------------------------------------------
  // JS API

  struct InitializerDict {
    jsg::Optional<int> status;
    jsg::Optional<kj::String> statusText;
    jsg::Optional<Headers::Initializer> headers;

    jsg::Optional<jsg::V8Ref<v8::Object>> cf;
    // Cloudflare-specific feature flags.

    jsg::Optional<kj::Maybe<jsg::Ref<WebSocket>>> webSocket;

    jsg::Optional<kj::String> encodeBody;

    JSG_STRUCT(status, statusText, headers, cf, webSocket, encodeBody);
    JSG_STRUCT_TS_OVERRIDE(ResponseInit {
      headers?: HeadersInit;
      encodeBody?: "automatic" | "manual";
    });
  };

  using Initializer = kj::OneOf<InitializerDict, jsg::Ref<Response>>;

  static jsg::Ref<Response> constructor(
      jsg::Lock& js,
      jsg::Optional<kj::Maybe<Body::Initializer>> bodyInit,
      jsg::Optional<Initializer> maybeInit,
      CompatibilityFlags::Reader flags);
  // Response's constructor has two arguments: an optional, nullable body that defaults to null, and
  // an optional initializer property bag. Tragically, the only way to express the "optional,
  // nullable body that defaults to null" is with an Optional<Maybe<Body::Initializer>>. The reason
  // for this is because:
  //
  //   - We need to be able to call `new Response()`, meaning the body initializer MUST be Optional.
  //   - We need to be able to call `new Response(null)`, but `null` cannot implicitly convert to
  //     an Optional, so we need an inner Maybe to inhibit string coercion to Body::Initializer.

  static jsg::Ref<Response> redirect(
      jsg::Lock& js, kj::String url, jsg::Optional<int> status, CompatibilityFlags::Reader flags);
  // Constructs a redirection response. `status` must be a redirect status if given, otherwise it
  // defaults to 302 (technically a non-conformity, but both Chrome and Firefox use this default).
  //
  // It's worth noting a couple property quirks of Responses constructed using this method:
  //   1. `url` will be the empty string, because the response didn't actually come from any
  //      particular URL.
  //   2. `redirected` will equal false, for the same reason as (1).
  //   3. `body` will be empty -- we don't even provide a default courtesy body. If you need one,
  //      you'll need to use the regular constructor, which is more flexible.
  //
  // These behaviors surprised me, but they match both the spec and Chrome/Firefox behavior.

  static jsg::Unimplemented error() { return {}; };
  // Constructs a `network error` response.
  //
  // A network error is a response whose status is always 0, status message is always the empty
  // byte sequence, header list is always empty, body is always null, and trailer is always empty.
  //
  // TODO(conform): implementation is missing; two approaches where tested:
  //  - returning a HTTP 5xx response but that doesn't match the spec and we didn't
  //    find it useful.
  //  - throwing/propaging a DISCONNECTED kj::Exception to actually disconnect the
  //    client. However, we were conserned about possible side-effects and incorrect
  //    error reporting.

  jsg::Ref<Response> clone(jsg::Lock& js, CompatibilityFlags::Reader flags);

  static jsg::Ref<Response> json_(
      jsg::Lock& js,
      v8::Local<v8::Value> any,
      jsg::Optional<Initializer> maybeInit,
      CompatibilityFlags::Reader flags);

  struct SendOptions {
    bool allowWebSocket = false;
  };
  kj::Promise<DeferredProxy<void>> send(
      jsg::Lock& js, kj::HttpService::Response& outer, SendOptions options,
      kj::Maybe<const kj::HttpHeaders&> maybeReqHeaders);
  // Helper not exposed to JavaScript.

  int getStatus();
  kj::StringPtr getStatusText();
  jsg::Ref<Headers> getHeaders(jsg::Lock& js);

  bool getOk();
  bool getRedirected();
  kj::StringPtr getUrl();

  kj::Maybe<jsg::Ref<WebSocket>> getWebSocket(jsg::Lock& js);

  jsg::Optional<v8::Local<v8::Object>> getCf(const v8::PropertyCallbackInfo<v8::Value>& info);
  // Returns the `cf` field containing Cloudflare feature flags.

  jsg::WontImplement getType() { return jsg::WontImplement(); }
  // This relates to CORS, which doesn't apply on the edge -- see Request::Initializer::mode.

  jsg::WontImplement getUseFinalUrl() { return jsg::WontImplement(); }
  // This is deprecated in the spec.

  JSG_RESOURCE_TYPE(Response, CompatibilityFlags::Reader flags) {
    JSG_INHERIT(Body);

    JSG_STATIC_METHOD(error);
    JSG_STATIC_METHOD(redirect);
    JSG_STATIC_METHOD_NAMED(json, json_);
    JSG_METHOD(clone);

    if (flags.getJsgPropertyOnPrototypeTemplate()) {
      JSG_READONLY_PROTOTYPE_PROPERTY(status, getStatus);
      JSG_READONLY_PROTOTYPE_PROPERTY(statusText, getStatusText);
      JSG_READONLY_PROTOTYPE_PROPERTY(headers, getHeaders);

      JSG_READONLY_PROTOTYPE_PROPERTY(ok, getOk);
      JSG_READONLY_PROTOTYPE_PROPERTY(redirected, getRedirected);
      JSG_READONLY_PROTOTYPE_PROPERTY(url, getUrl);

      JSG_READONLY_PROTOTYPE_PROPERTY(webSocket, getWebSocket);

      JSG_READONLY_PROTOTYPE_PROPERTY(cf, getCf);

      JSG_READONLY_PROTOTYPE_PROPERTY(type, getType);
      JSG_READONLY_PROTOTYPE_PROPERTY(useFinalUrl, getUseFinalUrl);
    } else {
      JSG_READONLY_INSTANCE_PROPERTY(status, getStatus);
      JSG_READONLY_INSTANCE_PROPERTY(statusText, getStatusText);
      JSG_READONLY_INSTANCE_PROPERTY(headers, getHeaders);

      JSG_READONLY_INSTANCE_PROPERTY(ok, getOk);
      JSG_READONLY_INSTANCE_PROPERTY(redirected, getRedirected);
      JSG_READONLY_INSTANCE_PROPERTY(url, getUrl);

      JSG_READONLY_INSTANCE_PROPERTY(webSocket, getWebSocket);

      JSG_READONLY_INSTANCE_PROPERTY(cf, getCf);

      JSG_READONLY_INSTANCE_PROPERTY(type, getType);
      JSG_READONLY_INSTANCE_PROPERTY(useFinalUrl, getUseFinalUrl);
    }

    JSG_TS_OVERRIDE({ constructor(body?: BodyInit | null, init?: ResponseInit); });
    // Use `BodyInit` and `ResponseInit` type aliases in constructor instead of inlining
  }

private:
  int statusCode;
  kj::String statusText;
  jsg::Ref<Headers> headers;
  kj::Maybe<jsg::V8Ref<v8::Object>> cf;

  kj::Array<kj::String> urlList;
  // The URL list, per the Fetch spec. Only Responses actually created by fetch() have a non-empty
  // URL list; for responses created from JavaScript this is empty. The list is filled in with the
  // sequence of URLs that fetch() requested. In redirect manual mode, this will be one element,
  // and just be a copy of the corresponding request's URL; in redirect follow mode the length of
  // the list will be one plus the number of redirects followed.
  //
  // The last URL is typically the only one that the user will care about, and is the one exposed
  // by getUrl().

  kj::Maybe<jsg::Ref<WebSocket>> webSocket;
  // If this response represents a successful WebSocket handshake, this is the socket, and the body
  // is empty.

  Response::BodyEncoding bodyEncoding;
  // If this response is already encoded and the user don't want to encode the
  // body twice, they can specify encodeBody: "manual".

  bool hasEnabledWebSocketCompression = false;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(headers, webSocket, cf);
  }
};

class FetchEvent: public ExtendableEvent {
public:
  FetchEvent(jsg::Ref<Request> request)
      : ExtendableEvent("fetch"), request(kj::mv(request)),
        state(AwaitingRespondWith()) {}

  kj::Maybe<jsg::Promise<jsg::Ref<Response>>> getResponsePromise(jsg::Lock& js);

  static jsg::Ref<FetchEvent> constructor(kj::String type) = delete;
  // TODO(soon): constructor

  jsg::Ref<Request> getRequest();
  void respondWith(jsg::Lock& js, jsg::Promise<jsg::Ref<Response>> promise);

  void passThroughOnException();

  // TODO(someday): Do any other FetchEvent members make sense on the edge?

  JSG_RESOURCE_TYPE(FetchEvent) {
    JSG_INHERIT(ExtendableEvent);

    JSG_READONLY_INSTANCE_PROPERTY(request, getRequest);
    JSG_METHOD(respondWith);
    JSG_METHOD(passThroughOnException);
  }

private:
  jsg::Ref<Request> request;

  struct AwaitingRespondWith {};
  struct RespondWithCalled {
    jsg::Promise<jsg::Ref<Response>> promise;
  };
  struct ResponseSent {};

  kj::OneOf<AwaitingRespondWith, RespondWithCalled, ResponseSent> state;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(request);
  }
};

jsg::Promise<jsg::Ref<Response>> fetchImpl(
    jsg::Lock& js,
    kj::Maybe<jsg::Ref<Fetcher>> fetcher,  // if null, use fetcher from request object
    Request::Info requestOrUrl,
    jsg::Optional<Request::Initializer> requestInit,
    CompatibilityFlags::Reader featureFlags);

jsg::Ref<Response> makeHttpResponse(
    jsg::Lock& js, kj::HttpMethod method, kj::Vector<kj::Url> urlList,
    uint statusCode, kj::StringPtr statusText, const kj::HttpHeaders& headers,
    kj::Own<kj::AsyncInputStream> body, kj::Maybe<jsg::Ref<WebSocket>> webSocket,
    CompatibilityFlags::Reader flags,
    Response::BodyEncoding bodyEncoding = Response::BodyEncoding::AUTO,
    kj::Maybe<jsg::Ref<AbortSignal>> signal = nullptr);

kj::Maybe<kj::StringPtr> defaultStatusText(uint statusCode);
// Return the RFC-recommended default status text for `statusCode`.

bool isNullBodyStatusCode(uint statusCode);
bool isRedirectStatusCode(uint statusCode);

kj::String makeRandomBoundaryCharacters();
// Make a boundary string for FormData serialization.
// TODO(cleanup): Move to form-data.{h,c++}?

#define EW_HTTP_ISOLATE_TYPES         \
  api::FetchEvent,                    \
  api::Headers,                       \
  api::Headers::EntryIterator,        \
  api::Headers::EntryIterator::Next,  \
  api::Headers::KeyIterator,          \
  api::Headers::KeyIterator::Next,    \
  api::Headers::ValueIterator,        \
  api::Headers::ValueIterator::Next,  \
  api::Body,                          \
  api::Response,                      \
  api::Response::InitializerDict,     \
  api::Request,                       \
  api::Request::InitializerDict,      \
  api::Fetcher,                       \
  api::Fetcher::PutOptions

// The list of http.h types that are added to worker.c++'s JSG_DECLARE_ISOLATE_TYPE
}  // namespace workerd::api
