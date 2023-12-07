// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/async-context.h>
#include <workerd/util/abortable.h>
#include <kj/compat/http.h>
#include <map>
#include "basics.h"
#include "cf-property.h"
#include "streams.h"
#include "form-data.h"
#include "web-socket.h"
#include "url.h"
#include "url-standard.h"
#include "blob.h"
#include <workerd/io/compatibility-date.capnp.h>

namespace workerd::api {

struct QueueResponse;

class Headers: public jsg::Object {
private:
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

  // Make a copy of this Headers object, and preserve the guard. The normal copy constructor sets
  // the copy's guard to NONE.
  jsg::Ref<Headers> clone() const;

  // Fill in the given HttpHeaders with these headers. Note that strings are inserted by
  // reference, so the output must be consumed immediately.
  void shallowCopyTo(kj::HttpHeaders& out);

  // Like has(), but only call this with an already-lower-case `name`. Useful to avoid an
  // unnecessary string allocation. Not part of the JS interface.
  bool hasLowerCase(kj::StringPtr name);

  // Returns headers with lower-case name and comma-concatenated duplicates.
  kj::Array<DisplayedHeader> getDisplayedHeaders(jsg::Lock& js);

  using ByteStringPair = jsg::Sequence<jsg::ByteString>;
  using ByteStringPairs = jsg::Sequence<ByteStringPair>;

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
  using Initializer = kj::OneOf<jsg::Ref<Headers>,
                                ByteStringPairs,
                                jsg::Dict<jsg::ByteString, jsg::ByteString>>;

  static jsg::Ref<Headers> constructor(jsg::Lock& js, jsg::Optional<Initializer> init);
  kj::Maybe<jsg::ByteString> get(jsg::ByteString name);

  // getAll is a legacy non-standard extension API that we introduced before
  // getSetCookie() was defined. We continue to support it for backwards
  // compatibility but users really ought to be using getSetCookie() now.
  kj::ArrayPtr<jsg::ByteString> getAll(jsg::ByteString name);

  // The Set-Cookie header is special in that it is the only HTTP header that
  // is not permitted to be combined into a single instance.
  kj::ArrayPtr<jsg::ByteString> getSetCookie();

  bool has(jsg::ByteString name);

  void set(jsg::ByteString name, jsg::ByteString value);

  // Like set(), but ignores the header guard if set. This can only be called from C++, and may be
  // used to mutate headers before dispatching a request.
  void setUnguarded(jsg::ByteString name, jsg::ByteString value);

  void append(jsg::ByteString name, jsg::ByteString value);

  void delete_(jsg::ByteString name);

  void forEach(jsg::Lock& js,
               jsg::Function<void(kj::StringPtr, kj::StringPtr, jsg::Ref<Headers>)>,
               jsg::Optional<jsg::Value>);

  bool inspectImmutable();

  JSG_ITERATOR(EntryIterator, entries,
                kj::Array<jsg::ByteString>,
                IteratorState<DisplayedHeader>,
                entryIteratorNext)
  JSG_ITERATOR(KeyIterator, keys,
                jsg::ByteString,
                IteratorState<jsg::ByteString>,
                keyOrValueIteratorNext)
  JSG_ITERATOR(ValueIterator, values,
                jsg::ByteString,
                IteratorState<jsg::ByteString>,
                keyOrValueIteratorNext)

  // JavaScript API.

  JSG_RESOURCE_TYPE(Headers, CompatibilityFlags::Reader flags) {
    JSG_METHOD(get);
    JSG_METHOD(getAll);
    if (flags.getHttpHeadersGetSetCookie()) {
      JSG_METHOD(getSetCookie);
    }
    JSG_METHOD(has);
    JSG_METHOD(set);
    JSG_METHOD(append);
    JSG_METHOD_NAMED(delete, delete_);
    JSG_METHOD(forEach);
    JSG_METHOD(entries);
    JSG_METHOD(keys);
    JSG_METHOD(values);

    JSG_INSPECT_PROPERTY(immutable, inspectImmutable);

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
    kj::Vector<jsg::ByteString> values;

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

  static kj::Maybe<kj::Array<jsg::ByteString>> entryIteratorNext(jsg::Lock& js, auto& state) {
    if (state.cursor == state.copy.end()) {
      return kj::none;
    }
    auto& ret = *state.cursor++;
    return kj::arr(kj::mv(ret.key), kj::mv(ret.value));
  }

  static kj::Maybe<jsg::ByteString> keyOrValueIteratorNext(jsg::Lock& js, auto& state) {
    if (state.cursor == state.copy.end()) {
      return kj::none;
    }
    auto& ret = *state.cursor++;
    return kj::mv(ret);
  }
};

// Base class for Request and Response. In JavaScript, this class is a mixin, meaning no one will
// be instantiating objects of this type -- it exists solely to house body-related functionality
// common to both Requests and Responses.
class Body: public jsg::Object {
public:
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
  using Initializer = kj::OneOf<jsg::Ref<ReadableStream>, kj::String, kj::Array<byte>,
                                jsg::Ref<Blob>, jsg::Ref<URLSearchParams>, jsg::Ref<FormData>>;

  struct RefcountedBytes final: public kj::Refcounted {
    kj::Array<kj::byte> bytes;
    RefcountedBytes(kj::Array<kj::byte>&& bytes): bytes(kj::mv(bytes)) {}
  };

  // The Fetch spec calls this type the body's "source", even though it really is a buffer. I end
  // talking about things like "a buffer-backed body", whereas in standardese I should say
  // "a body with a non-null source".
  //
  // I find that confusing, so let's just call it what it is: a Body::Buffer.
  struct Buffer {
    // In order to reconstruct buffer-backed ReadableStreams without gratuitous array copying, we
    // need to be able to tie the lifetime of the source buffer to the lifetime of the
    // ReadableStream's native stream, AND the lifetime of the Body itself. Thus we need
    // refcounting.
    //
    // NOTE: ownBytes may contain a v8::Global reference, hence instances of `Buffer` must exist
    //   only within the V8 heap space.
    kj::OneOf<kj::Own<RefcountedBytes>, jsg::Ref<Blob>> ownBytes;
    // TODO(cleanup): When we integrate with V8's garbage collection APIs, we need to account for
    //   that here.

    // Bodies constructed from buffers rather than ReadableStreams can be retransmitted if necessary
    // (e.g. for redirects, authentication). In these cases, we need to keep an ArrayPtr view onto
    // the Array source itself, because the source may be a string, and thus have a trailing nul
    // byte.
    kj::ArrayPtr<const kj::byte> view;

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
                  kj::Maybe<Buffer> source = kj::none,
                  kj::Maybe<kj::String> contentType = kj::none);

    Impl impl;
    kj::Maybe<kj::String> contentType;
  };

  // Implements the "extract a body" algorithm from the Fetch spec.
  // https://fetch.spec.whatwg.org/#concept-bodyinit-extract
  static ExtractedBody extractBody(jsg::Lock& js, Initializer init);

  explicit Body(kj::Maybe<ExtractedBody> init, Headers& headers);

  kj::Maybe<Buffer> getBodyBuffer(jsg::Lock& js);

  // The following body rewind/nullification functions are helpers for implementing fetch() redirect
  // handling.

  // True if this body is null or buffer-backed, false if this body is a ReadableStream.
  bool canRewindBody();

  // Reconstruct this body from its backing buffer. Precondition: `canRewindBody() == true`.
  void rewindBody(jsg::Lock& js);

  // Convert this body into a null body.
  void nullifyBody();

  // ---------------------------------------------------------------------------
  // JS API

  kj::Maybe<jsg::Ref<ReadableStream>> getBody();
  bool getBodyUsed();
  jsg::Promise<kj::Array<byte>> arrayBuffer(jsg::Lock& js);
  jsg::Promise<kj::String> text(jsg::Lock& js);
  jsg::Promise<jsg::Ref<FormData>> formData(jsg::Lock& js);
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
  // Helper to implement Request/Response::clone().
  kj::Maybe<ExtractedBody> clone(jsg::Lock& js);

private:
  kj::Maybe<Impl> impl;

  // HACK: This `headersRef` variable refers to a Headers object in the Request/Response subclass.
  //   As such, it will briefly dangle during object destruction. While unlikely to be an issue,
  //   it's worth being aware of.
  Headers& headersRef;

  void visitForGc(jsg::GcVisitor& visitor) {
    KJ_IF_SOME(i, impl) {
      visitor.visit(i.stream);
    }
  }
};

class Request;
class Response;
struct RequestInitializerDict;

class Socket;
struct SocketOptions;
struct SocketAddress;
typedef kj::OneOf<SocketAddress, kj::String> AnySocketAddress;

// A capability to send HTTP requests to some destination other than the public internet.
// This is the type of `request.fetcher` (if it is not null).
//
// Actually, this interface could support more than just HTTP. This interface is really the
// JavaScript wapper around `WorkerInterface`. It is the type used by worker-to-worker bindings.
// As we add support for non-HTTP event types that can be invoked remotely, they should be added
// here.
class Fetcher: public jsg::Object {
public:
  // Should we use a fake https base url if we lack a scheme+authority?
  enum class RequiresHostAndProtocol {
    YES,
    NO
  };

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
  explicit Fetcher(uint channel, RequiresHostAndProtocol requiresHost, bool isInHouse = false)
      : channelOrClientFactory(channel), requiresHost(requiresHost), isInHouse(isInHouse) {}

  // Used by Fetchers that use ad-hoc, single-use WorkerInterface instances, such as ones
  // created for Actors.
  class OutgoingFactory {
  public:
    virtual kj::Own<WorkerInterface> newSingleUseClient(kj::Maybe<kj::String> cfStr) = 0;
  };

  // Used by Fetchers that obtain their HttpClient in a custom way, but which aren't tied
  // to a specific I/O context. The factory object moves with the isolate across threads and
  // contexts, and must work from any context.
  class CrossContextOutgoingFactory {
  public:
    virtual kj::Own<WorkerInterface> newSingleUseClient(IoContext& context, kj::Maybe<kj::String> cfStr) = 0;
  };

  // `outgoingFactory` is used for Fetchers that use ad-hoc WorkerInterface instances, such as ones
  // created for Actors.
  Fetcher(IoOwn<OutgoingFactory> outgoingFactory,
          RequiresHostAndProtocol requiresHost,
          bool isInHouse = false)
      : channelOrClientFactory(kj::mv(outgoingFactory)),
        requiresHost(requiresHost),
        isInHouse(isInHouse) {}

  // `outgoingFactory` is used for Fetchers that use ad-hoc WorkerInterface instances, but doesn't
  // require an IoContext
  Fetcher(kj::Own<CrossContextOutgoingFactory> outgoingFactory,
          RequiresHostAndProtocol requiresHost,
          bool isInHouse = false)
      : channelOrClientFactory(kj::mv(outgoingFactory)),
        requiresHost(requiresHost),
        isInHouse(isInHouse) {}

  // Returns an `WorkerInterface` that is only valid for the lifetime of the current
  // `IoContext`.
  kj::Own<WorkerInterface> getClient(
      IoContext& ioContext,
      kj::Maybe<kj::String> cfStr,
      kj::ConstString operationName);

  // Wraps kj::Url::parse to take into account whether the Fetcher requires a host to be
  // specified on URLs, Fetcher-specific URL decoding options, and error handling.
  kj::Url parseUrl(jsg::Lock& js, kj::StringPtr url);

  jsg::Ref<Socket> connect(
      jsg::Lock& js, AnySocketAddress address, jsg::Optional<SocketOptions> options);

  jsg::Promise<jsg::Ref<Response>> fetch(
      jsg::Lock& js, kj::OneOf<jsg::Ref<Request>, kj::String> requestOrUrl,
      jsg::Optional<kj::OneOf<RequestInitializerDict, jsg::Ref<Request>>> requestInit);

  using GetResult = kj::OneOf<jsg::Ref<ReadableStream>, kj::Array<byte>, kj::String, jsg::Value>;

  jsg::Promise<GetResult> get(jsg::Lock& js, kj::String url, jsg::Optional<kj::String> type);

  // Optional parameter for passing options into a Fetcher::put. Initially
  // intended for supporting expiration times in KV bindings.
  struct PutOptions {
    jsg::Optional<int> expiration;
    jsg::Optional<int> expirationTtl;

    JSG_STRUCT(expiration, expirationTtl);
  };

  jsg::Promise<void> put(
      jsg::Lock& js, kj::String url, Body::Initializer body, jsg::Optional<PutOptions> options);

  jsg::Promise<void> delete_(jsg::Lock& js, kj::String url);

  // Representation of a queue message for use when invoking the queue() event handler on another
  // worker via a service binding.
  struct ServiceBindingQueueMessage {
    kj::String id;
    kj::Date timestamp;
    jsg::Optional<jsg::Value> body;
    jsg::Optional<kj::Array<kj::byte>> serializedBody;

    JSG_STRUCT(id, timestamp, body, serializedBody);
    JSG_STRUCT_TS_OVERRIDE(type ServiceBindingQueueMessage<Body = unknown> = {
      id: string;
      timestamp: Date;
    } & (
      | { body: Body }
      | { serializedBody: ArrayBuffer | ArrayBufferView }
    ));
  };

  struct QueueResult {
    kj::String outcome;
    bool retryAll;
    bool ackAll;
    kj::Array<kj::String> explicitRetries;
    kj::Array<kj::String> explicitAcks;

    JSG_STRUCT(outcome, retryAll, ackAll, explicitRetries, explicitAcks);
  };

  jsg::Promise<QueueResult> queue(
      jsg::Lock& js, kj::String queueName, kj::Array<ServiceBindingQueueMessage> messages);

  struct ScheduledOptions {
    jsg::Optional<kj::Date> scheduledTime;
    jsg::Optional<kj::String> cron;

    JSG_STRUCT(scheduledTime, cron);
  };

  struct ScheduledResult {
    kj::String outcome;
    bool noRetry;

    JSG_STRUCT(outcome, noRetry);
  };

  jsg::Promise<ScheduledResult> scheduled(jsg::Lock& js, jsg::Optional<ScheduledOptions> options);

  JSG_RESOURCE_TYPE(Fetcher, CompatibilityFlags::Reader flags) {
    JSG_METHOD(fetch);
    JSG_METHOD(connect);

    if (flags.getServiceBindingExtraHandlers()) {
      JSG_METHOD(queue);
      JSG_METHOD(scheduled);
    }

    JSG_METHOD(get);
    JSG_METHOD(put);
    JSG_METHOD_NAMED(delete, delete_);

    JSG_TS_OVERRIDE({
      fetch(input: RequestInfo, init?: RequestInit): Promise<Response>;
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

// Type of the second parameter to Request's constructor. Also the type of the second parameter
// to fetch().
struct RequestInitializerDict {
  jsg::Optional<kj::String> method;
  jsg::Optional<Headers::Initializer> headers;

  // The script author may specify an empty body either implicitly, by allowing this property to
  // be undefined, or explicitly, by setting this property to null. To support both cases, this
  // body initializer must be Optional<Maybe<Body::Initializer>>.
  jsg::Optional<kj::Maybe<Body::Initializer>> body;

  // follow, error, manual (default follow)
  jsg::Optional<kj::String> redirect;

  jsg::Optional<kj::Maybe<jsg::Ref<Fetcher>>> fetcher;

  // Cloudflare-specific feature flags.
  jsg::Optional<jsg::V8Ref<v8::Object>> cf;
  // TODO(someday): We should generalize this concept to sending control information to
  //   downstream workers in the pipeline. That is, when multiple workers apply to the same
  //   request (with the first worker's subrequests being passed to the next worker), then
  //   first worker should be able to set flags on the request that the second worker can see.
  //   Perhaps we should say that any field you set on a Request object will be JSON-serialized
  //   and passed on to the next worker? Then `cf` is just one such field: it's not special,
  //   it's only named `cf` because the consumer is Cloudflare code.

  // These control CORS policy. This doesn't matter on the edge because CSRF is not possible
  // here:
  // 1. We don't have the user's credentials (e.g. cookies) for any other origin, so we couldn't
  //    forge a request from the user even if we wanted to.
  // 2. We aren't behind the user's firewall, so we also can't forge requests to unauthenticated
  //    internal network services.
  jsg::WontImplement mode;

  // These control CORS policy. This doesn't matter on the edge because CSRF is not possible
  // here:
  // 1. We don't have the user's credentials (e.g. cookies) for any other origin, so we couldn't
  //    forge a request from the user even if we wanted to.
  // 2. We aren't behind the user's firewall, so we also can't forge requests to unauthenticated
  //    internal network services.
  jsg::WontImplement credentials;

  // In browsers this controls the local browser cache. For Cloudflare Workers it could control the
  // Cloudflare edge cache. Note that this setting is different from using the `Cache-Control`
  // header since `Cache-Control` would be forwarded to the origin.
  jsg::Unimplemented cache;

  // These control how the `Referer` and `Origin` headers are initialized by the browser.
  // Browser-side JavaScript is normally not permitted to set these headers, because servers
  // sometimes use the headers to defend against CSRF. On the edge, CSRF is not a risk (see
  // comments about `mode` and `credentials`, above), hence protecting the Referer and Origin
  // headers is not necessary, so we treat them as regular-old headers instead.
  jsg::WontImplement referrer;

  // These control how the `Referer` and `Origin` headers are initialized by the browser.
  // Browser-side JavaScript is normally not permitted to set these headers, because servers
  // sometimes use the headers to defend against CSRF. On the edge, CSRF is not a risk (see
  // comments about `mode` and `credentials`, above), hence protecting the Referer and Origin
  // headers is not necessary, so we treat them as regular-old headers instead.
  jsg::WontImplement referrerPolicy;

  // Subresource integrity (check response against a given hash).
  // We do not implement integrity checking, however, we will accept either an undefined
  // or empty string value for the property. If any other value is given we will throw.
  jsg::Optional<kj::String> integrity;

  // The spec declares this optional, but is unclear on whether it is nullable. The spec is also
  // unclear on whether the `Request.signal` property is nullable. If `Request.signal` is nullable,
  // then we definitely have to accept `null` as an input here, otherwise
  // `new Request(url, {...request})` will fail when `request.signal` is null. However, it's also
  // possible that neither property should be nullable. Indeed, it appears that Chrome always
  // constructs a dummy signal even if none was provided, and uses that. But Chrome is also happy
  // to accept `null` as an input, so if we're doing what Chrome does, then we should accept
  // `null`.
  jsg::Optional<kj::Maybe<jsg::Ref<AbortSignal>>> signal;

  // We do not support keepalive currently and may never?
  // Per the spec, keepalive is "a boolean indicating whether or not request can
  // outlive the global in which it was created." We could choose to explicitly indicate
  // that we do not support this option but for now we'll just ignore it.
  // jsg::Optional<bool> keepalive;
  // TODO(conform): Won't support?

  // The duplex option controls whether or not a fetch is expected to send the entire request
  // before processing the response. The default value ("half"), which is currently the only
  // option supported by the standard, dictates that the request is fully sent before handling
  // the response. There are currently a proposal to add a "full" option which is the model
  // we support. Once "full" is added, we need to update this to accept either undefined or
  // "full", and possibly decide if we want to support the "half" option. For now we'll just
  // ignore this option. Enabling this option later might require a compatibility flag.
  // jsg::Optional<kj::String> duplex;
  // TODO(conform): Might support later?

  // Specifies the relative priority of the request. We currently do not make use of this
  // information. Per the spec, the only values acceptable for the priority option are
  // "high", "low", and "auto", with "auto" being considered the default. For now we'll just
  // ignore this option. Enabling this option later might require a compatibility flag.
  // jsg::Optional<kj::String> priority;
  // TODO(conform): Might support later?

  JSG_STRUCT(method, headers, body, redirect, fetcher, cf, mode, credentials, cache,
             referrer, referrerPolicy, integrity, signal);
  JSG_STRUCT_TS_OVERRIDE(RequestInit<Cf = CfProperties> {
    headers?: HeadersInit;
    body?: BodyInit | null;
    cf?: Cf;
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
          kj::Maybe<jsg::Ref<AbortSignal>> signal, CfProperty&& cf,
          kj::Maybe<Body::ExtractedBody> body)
    : Body(kj::mv(body), *headers), method(method), url(kj::str(url)),
      redirect(redirect), headers(kj::mv(headers)), fetcher(kj::mv(fetcher)),
      cf(kj::mv(cf)) {
    KJ_IF_SOME(s, signal) {
      // If the AbortSignal will never abort, assigning it to thisSignal instead ensures
      // that the cancel machinery is not used but the request.signal accessor will still
      // do the right thing.
      if (s->getNeverAborts()) {
        this->thisSignal = kj::mv(s);
      } else {
        this->signal = kj::mv(s);
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

  // Wrapper around Request::constructor that calls it only if necessary, and returns a
  // jsg::Ref<Request>.
  //
  // C++ API, but declared down here because we need the InitializerDict type.
  static jsg::Ref<Request> coerce(
      jsg::Lock& js,
      Request::Info input,
      jsg::Optional<Request::Initializer> init);

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

  // getSignal() is the one that we used internally to determine if there's actually
  // an AbortSignal that can be triggered to cancel things. The getThisSignal() is
  // used only on the JavaScript side to conform to the spec, which requires
  // request.signal to always return an AbortSignal even if one is not actively
  // used on this request.
  kj::Maybe<jsg::Ref<AbortSignal>> getSignal();

  jsg::Ref<AbortSignal> getThisSignal(jsg::Lock& js);

  // Returns the `cf` field containing Cloudflare feature flags.
  jsg::Optional<jsg::JsObject> getCf(jsg::Lock& js);

  // We do not implement support for the keepalive option but we do want to at least provide
  // the standard property, hard-coded to always be false.
  bool getKeepalive() { return false; }

  // The duplex option controls whether or not a fetch is expected to send the entire request
  // before processing the response. The default value ("half"), which is currently the only
  // option supported by the standard, dictates that the request is fully sent before handling
  // the response. There are currently a proposal to add a "full" option which is the model
  // we support. Once "full" is added, we need to update this to accept either undefined or
  // "full", and possibly decide if we want to support the "half" option.
  // jsg::JsValue getDuplex(jsg::Lock& js) { return js.v8Undefined(); }
  // TODO(conform): Might implement?

  // These relate to CORS support, which we do not implement. In the
  // Request initializer we will explicitly throw if any attempt is
  // made to specify these. For the accessors tho, we want it to always
  // just return undefined rather than throw, which helps with code
  // portability across multiple runtimes. The spec says that the default
  // value for mode when not specified *should* be 'no-cors`, but that
  // value implies strict limitations that we do not follow. In discussion
  // with other implementers with the same issues, it was decided that
  // simply returning undefined for these was the best option.
  // jsg::JsValue getMode(jsg::Lock& js) { return js.v8Undefined(); }
  // jsg::JsValue getCredentials(jsg::Lock& js) { return js.v8Undefined(); }
  // TODO(conform): Won't implement?

  // The cache mode determines how HTTP cache is used with the request.
  // We currently do not fully implement this. Currently we will explicitly
  // throw in the Request constructor if the option is set. For the accessor
  // we want it to always just return undefined while it is not implemented.
  // The spec does not provide a value to indicate "unimplemented" and all
  // of the other values would imply semantics we do not follow. In discussion
  // with other implementers with the same issues, it was decided that
  // simply returning undefined for these was the best option.
  // jsg::JsValue getCache(jsg::Lock& js) { return js.v8Undefined(); }
  // TODO(conform): Won't implement?

  // We do not implement integrity checking at all. However, the spec says that
  // the default value should be an empty string. When the Request object is
  // created we verify that the given value is undefined or empty.
  kj::String getIntegrity() { return kj::String(); }

  JSG_RESOURCE_TYPE(Request, CompatibilityFlags::Reader flags) {
    JSG_INHERIT(Body);

    JSG_METHOD(clone);

    JSG_TS_DEFINE(type RequestInfo<CfHostMetadata = unknown, Cf = CfProperties<CfHostMetadata>> = Request<CfHostMetadata, Cf> | string | URL);
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

      // TODO(conform): These are standard properties that we do not implement (see descriptions
      // above).
      // JSG_READONLY_PROTOTYPE_PROPERTY(duplex, getDuplex);
      // JSG_READONLY_PROTOTYPE_PROPERTY(mode, getMode);
      // JSG_READONLY_PROTOTYPE_PROPERTY(credentials, getCredentials);
      // JSG_READONLY_PROTOTYPE_PROPERTY(cache, getCache);
      JSG_READONLY_PROTOTYPE_PROPERTY(integrity, getIntegrity);
      JSG_READONLY_PROTOTYPE_PROPERTY(keepalive, getKeepalive);

      JSG_TS_OVERRIDE(<CfHostMetadata = unknown, Cf = CfProperties<CfHostMetadata>> {
        constructor(input: RequestInfo<CfProperties>, init?: RequestInit<Cf>);
        clone(): Request<CfHostMetadata, Cf>;
        get cf(): Cf | undefined;
      });
      // Use `RequestInfo` and `RequestInit` type aliases in constructor instead of inlining.
      // `CfProperties` is defined in `/types/defines/cf.d.ts`. We only really need a single `Cf`
      // type parameter here, but it would be a breaking type change to remove `CfHostMetadata`.
    } else {
      JSG_READONLY_INSTANCE_PROPERTY(method, getMethod);
      JSG_READONLY_INSTANCE_PROPERTY(url, getUrl);
      JSG_READONLY_INSTANCE_PROPERTY(headers, getHeaders);
      JSG_READONLY_INSTANCE_PROPERTY(redirect, getRedirect);
      JSG_READONLY_INSTANCE_PROPERTY(fetcher, getFetcher);
      JSG_READONLY_INSTANCE_PROPERTY(signal, getThisSignal);
      JSG_READONLY_INSTANCE_PROPERTY(cf, getCf);

      // TODO(conform): These are standard properties that we do not implement (see descriptions
      // above).
      // JSG_READONLY_INSTANCE_PROPERTY(duplex, getDuplex);
      // JSG_READONLY_INSTANCE_PROPERTY(mode, getMode);
      // JSG_READONLY_INSTANCE_PROPERTY(credentials, getCredentials);
      // JSG_READONLY_INSTANCE_PROPERTY(cache, getCache);
      JSG_READONLY_INSTANCE_PROPERTY(integrity, getIntegrity);
      JSG_READONLY_INSTANCE_PROPERTY(keepalive, getKeepalive);

      JSG_TS_OVERRIDE(<CfHostMetadata = unknown, Cf = CfProperties<CfHostMetadata>> {
        constructor(input: RequestInfo<CfProperties>, init?: RequestInit<Cf>);
        clone(): Request<CfHostMetadata, Cf>;
        readonly cf?: Cf;
      });
    }
  }

private:
  kj::HttpMethod method;
  kj::String url;
  Redirect redirect;
  jsg::Ref<Headers> headers;
  kj::Maybe<jsg::Ref<Fetcher>> fetcher;
  kj::Maybe<jsg::Ref<AbortSignal>> signal;

  // The fetch spec definition of Request has a distinction between the "signal" (which is
  // an optional AbortSignal passed in with the options), and "this' signal", which is an
  // AbortSignal that is always available via the request.signal accessor. When signal is
  // used explicitly, thisSignal will not be.
  kj::Maybe<jsg::Ref<AbortSignal>> thisSignal;

  CfProperty cf;

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

  Response(jsg::Lock& js, int statusCode, kj::String statusText, jsg::Ref<Headers> headers,
           CfProperty&& cf, kj::Maybe<Body::ExtractedBody> body,
           kj::Array<kj::String> urlList = {},
           kj::Maybe<jsg::Ref<WebSocket>> webSocket = kj::none,
           Response::BodyEncoding bodyEncoding = Response::BodyEncoding::AUTO);

  // ---------------------------------------------------------------------------
  // JS API

  struct InitializerDict {
    jsg::Optional<int> status;
    jsg::Optional<kj::String> statusText;
    jsg::Optional<Headers::Initializer> headers;

    // Cloudflare-specific feature flags.
    jsg::Optional<jsg::V8Ref<v8::Object>> cf;

    jsg::Optional<kj::Maybe<jsg::Ref<WebSocket>>> webSocket;

    jsg::Optional<kj::String> encodeBody;

    JSG_STRUCT(status, statusText, headers, cf, webSocket, encodeBody);
    JSG_STRUCT_TS_OVERRIDE(ResponseInit {
      headers?: HeadersInit;
      encodeBody?: "automatic" | "manual";
    });
  };

  using Initializer = kj::OneOf<InitializerDict, jsg::Ref<Response>>;

  // Response's constructor has two arguments: an optional, nullable body that defaults to null, and
  // an optional initializer property bag. Tragically, the only way to express the "optional,
  // nullable body that defaults to null" is with an Optional<Maybe<Body::Initializer>>. The reason
  // for this is because:
  //
  //   - We need to be able to call `new Response()`, meaning the body initializer MUST be Optional.
  //   - We need to be able to call `new Response(null)`, but `null` cannot implicitly convert to
  //     an Optional, so we need an inner Maybe to inhibit string coercion to Body::Initializer.
  static jsg::Ref<Response> constructor(
      jsg::Lock& js,
      jsg::Optional<kj::Maybe<Body::Initializer>> bodyInit,
      jsg::Optional<Initializer> maybeInit);

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
  static jsg::Ref<Response> redirect(jsg::Lock& js, jsg::UsvString url, jsg::Optional<int> status);

  // Constructs a `network error` response.
  //
  // A network error is a response whose status is always 0, status message is always the empty
  // byte sequence, header list is always empty, body is always null, and trailer is always empty.
  static jsg::Unimplemented error() { return {}; };
  // TODO(conform): implementation is missing; two approaches where tested:
  //  - returning a HTTP 5xx response but that doesn't match the spec and we didn't
  //    find it useful.
  //  - throwing/propagating a DISCONNECTED kj::Exception to actually disconnect the
  //    client. However, we were concerned about possible side-effects and incorrect
  //    error reporting.

  jsg::Ref<Response> clone(jsg::Lock& js);

  static jsg::Ref<Response> json_(
      jsg::Lock& js,
      jsg::JsValue any,
      jsg::Optional<Initializer> maybeInit);

  struct SendOptions {
    bool allowWebSocket = false;
  };

  // Helper not exposed to JavaScript.
  kj::Promise<DeferredProxy<void>> send(
      jsg::Lock& js, kj::HttpService::Response& outer, SendOptions options,
      kj::Maybe<const kj::HttpHeaders&> maybeReqHeaders);

  int getStatus();
  kj::StringPtr getStatusText();
  jsg::Ref<Headers> getHeaders(jsg::Lock& js);

  bool getOk();
  bool getRedirected();
  kj::StringPtr getUrl();

  kj::Maybe<jsg::Ref<WebSocket>> getWebSocket(jsg::Lock& js);

  // Returns the `cf` field containing Cloudflare feature flags.
jsg::Optional<jsg::JsObject> getCf(jsg::Lock& js);

  // This relates to CORS, which doesn't apply on the edge -- see Request::Initializer::mode.
  // In discussing with other runtime implementations that do not implement CORS, it was
  // determined that just have this property as undefined is the best option.
  // jsg::JsValue getType(jsg::Lock& js) { return js.v8Undefined(); }
  // TODO(conform): Won't implement?

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

      // TODO(conform): This is a standard properties that we do not implement (see description
      // above).
      // JSG_READONLY_PROTOTYPE_PROPERTY(type, getType);
    } else {
      JSG_READONLY_INSTANCE_PROPERTY(status, getStatus);
      JSG_READONLY_INSTANCE_PROPERTY(statusText, getStatusText);
      JSG_READONLY_INSTANCE_PROPERTY(headers, getHeaders);

      JSG_READONLY_INSTANCE_PROPERTY(ok, getOk);
      JSG_READONLY_INSTANCE_PROPERTY(redirected, getRedirected);
      JSG_READONLY_INSTANCE_PROPERTY(url, getUrl);

      JSG_READONLY_INSTANCE_PROPERTY(webSocket, getWebSocket);

      JSG_READONLY_INSTANCE_PROPERTY(cf, getCf);

      // TODO(conform): This is a standard properties that we do not implement (see description
      // above).
      // JSG_READONLY_INSTANCE_PROPERTY(type, getType);
    }

    JSG_TS_OVERRIDE({ constructor(body?: BodyInit | null, init?: ResponseInit); });
    // Use `BodyInit` and `ResponseInit` type aliases in constructor instead of inlining
  }

private:
  int statusCode;
  kj::String statusText;
  jsg::Ref<Headers> headers;
  CfProperty cf;

  // The URL list, per the Fetch spec. Only Responses actually created by fetch() have a non-empty
  // URL list; for responses created from JavaScript this is empty. The list is filled in with the
  // sequence of URLs that fetch() requested. In redirect manual mode, this will be one element,
  // and just be a copy of the corresponding request's URL; in redirect follow mode the length of
  // the list will be one plus the number of redirects followed.
  //
  // The last URL is typically the only one that the user will care about, and is the one exposed
  // by getUrl().
  kj::Array<kj::String> urlList;

  // If this response represents a successful WebSocket handshake, this is the socket, and the body
  // is empty.
  kj::Maybe<jsg::Ref<WebSocket>> webSocket;

  // If this response is already encoded and the user don't want to encode the
  // body twice, they can specify encodeBody: "manual".
  Response::BodyEncoding bodyEncoding;

  bool hasEnabledWebSocketCompression = false;

  // Capturing the AsyncContextFrame when the Response is created is necessary because there's
  // a natural separation that occurs between the moment the Response is created and when we
  // actually start consuming it. If a JS-backed ReadableStream is used, we end up losing the
  // appropriate async context in the promise read loop since that is kicked off later.
  kj::Maybe<jsg::Ref<jsg::AsyncContextFrame>> asyncContext;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(headers, webSocket, cf, asyncContext);
  }
};

class FetchEvent: public ExtendableEvent {
public:
  FetchEvent(jsg::Ref<Request> request)
      : ExtendableEvent("fetch"), request(kj::mv(request)),
        state(AwaitingRespondWith()) {}

  kj::Maybe<jsg::Promise<jsg::Ref<Response>>> getResponsePromise(jsg::Lock& js);

  // TODO(soon): constructor
  static jsg::Ref<FetchEvent> constructor(kj::String type) = delete;

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
    jsg::Optional<Request::Initializer> requestInit);

jsg::Ref<Response> makeHttpResponse(
    jsg::Lock& js, kj::HttpMethod method, kj::Vector<kj::Url> urlList,
    uint statusCode, kj::StringPtr statusText, const kj::HttpHeaders& headers,
    kj::Own<kj::AsyncInputStream> body, kj::Maybe<jsg::Ref<WebSocket>> webSocket,
    Response::BodyEncoding bodyEncoding = Response::BodyEncoding::AUTO,
    kj::Maybe<jsg::Ref<AbortSignal>> signal = kj::none);

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
  api::Fetcher::PutOptions,           \
  api::Fetcher::ScheduledOptions,     \
  api::Fetcher::ScheduledResult,      \
  api::Fetcher::QueueResult,          \
  api::Fetcher::ServiceBindingQueueMessage

// The list of http.h types that are added to worker.c++'s JSG_DECLARE_ISOLATE_TYPE
}  // namespace workerd::api
