// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>
#include "http.h"
#include "workerd/io/io-context.h"

namespace workerd::api {

class KvNamespace: public jsg::Object {
  // A capability to a KV namespace.

public:
  struct AdditionalHeader {
    kj::String name;
    kj::String value;
  };

  explicit KvNamespace(kj::Array<AdditionalHeader> additionalHeaders, uint subrequestChannel)
      : additionalHeaders(kj::mv(additionalHeaders)), subrequestChannel(subrequestChannel) {}
  // `subrequestChannel` is what to pass to IoContext::getHttpClient() to get an HttpClient
  // representing this namespace.
  // `additionalHeaders` is what gets appended to every outbound request.

  struct GetOptions {
    jsg::Optional<kj::String> type;
    jsg::Optional<int> cacheTtl;

    JSG_STRUCT(type, cacheTtl);
  };

  using GetResult = kj::Maybe<
      kj::OneOf<jsg::Ref<ReadableStream>, kj::Array<byte>, kj::String, jsg::Value>>;

  jsg::Promise<GetResult> get(
      jsg::Lock& js,
      kj::String name,
      jsg::Optional<kj::OneOf<kj::String, GetOptions>> options);

  struct GetWithMetadataResult {
    GetResult value;
    kj::Maybe<jsg::Value> metadata;
    JSG_STRUCT(value, metadata);
  };

  jsg::Promise<GetWithMetadataResult> getWithMetadata(
      jsg::Lock& js,
      kj::String name,
      jsg::Optional<kj::OneOf<kj::String, GetOptions>> options);

  struct ListOptions {
    jsg::Optional<int> limit;
    jsg::Optional<kj::Maybe<kj::String>> prefix;
    jsg::Optional<kj::Maybe<kj::String>> cursor;

    JSG_STRUCT(limit, prefix, cursor);
  };

  jsg::Promise<jsg::Value> list(jsg::Lock& js, jsg::Optional<ListOptions> options);

  struct PutOptions {
    // Optional parameter for passing options into a Fetcher::put. Initially
    // intended for supporting expiration times in KV bindings.

    jsg::Optional<int> expiration;
    jsg::Optional<int> expirationTtl;
    jsg::Optional<kj::Maybe<jsg::Value>> metadata;

    JSG_STRUCT(expiration, expirationTtl, metadata);
  };

  using PutBody = kj::OneOf<kj::String, v8::Local<v8::Object>>;
  // We can't just list the supported types in this OneOf because if we did then arbitrary objects
  // would get coerced into meaningless strings like "[object Object]". Instead we first use this
  // OneOf to differentiate between primitives and objects, and check the object for the types that
  // we specifically support later.

  using PutSupportedTypes = kj::OneOf<kj::String, kj::Array<byte>, jsg::Ref<ReadableStream>>;

  jsg::Promise<void> put(
      jsg::Lock& js,
      kj::String name,
      PutBody body,
      jsg::Optional<PutOptions> options,
      const jsg::TypeHandler<PutSupportedTypes>& putTypeHandler);

  jsg::Promise<void> delete_(jsg::Lock& js, kj::String name);

  JSG_RESOURCE_TYPE(KvNamespace) {
    JSG_METHOD(get);
    JSG_METHOD(list);
    JSG_METHOD(put);
    JSG_METHOD(getWithMetadata);
    JSG_METHOD_NAMED(delete, delete_);
  }

protected:
  kj::Own<kj::HttpClient> getHttpClient(
      IoContext& context,
      kj::HttpHeaders& headers,
      kj::OneOf<LimitEnforcer::KvOpType, kj::StringPtr> opTypeOrName,
      kj::StringPtr urlStr
  );
  // Do the boilerplate work of constructing an HTTP client to KV. Setting a KvOptType causes
  // the limiter for that op type to be checked. If a string is used, that's used as the operation
  // name for the HttpClient without any limiter enforcement.
  // NOTE: The urlStr is added to the headers as a non-owning reference and thus must outlive
  // the usage of the headers.

private:
  kj::Array<AdditionalHeader> additionalHeaders;
  uint subrequestChannel;
};

#define EW_KV_ISOLATE_TYPES                 \
  api::KvNamespace,                         \
  api::KvNamespace::ListOptions,            \
  api::KvNamespace::GetOptions,             \
  api::KvNamespace::PutOptions,             \
  api::KvNamespace::GetWithMetadataResult
// The list of kv.h types that are added to worker.c++'s JSG_DECLARE_ISOLATE_TYPE
}  // namespace workerd::api
