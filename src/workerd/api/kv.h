// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>
#include "http.h"

namespace workerd::api {

class KvNamespace: public jsg::Object {
  // A capability to a KV namespace.

public:
  explicit KvNamespace(uint subrequestChannel): subrequestChannel(subrequestChannel) {}
  // `subrequestChannel` is what to pass to IoContext::getHttpClient() to get an HttpClient
  // representing this namespace.

  struct GetOptions {
    jsg::Optional<kj::String> type;
    jsg::Optional<int> cacheTtl;

    JSG_STRUCT(type, cacheTtl);
    JSG_STRUCT_TS_OVERRIDE(KVNamespaceGetOptions<Type> {
      type: Type;
    });
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
    JSG_STRUCT_TS_OVERRIDE(KVNamespaceGetWithMetadataResult<Value, Metadata> {
      value: Value | null;
      metadata: Metadata | null;
    });
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
    JSG_STRUCT_TS_OVERRIDE(KVNamespaceListOptions);
  };

  jsg::Promise<jsg::Value> list(jsg::Lock& js, jsg::Optional<ListOptions> options);

  struct PutOptions {
    // Optional parameter for passing options into a Fetcher::put. Initially
    // intended for supporting expiration times in KV bindings.

    jsg::Optional<int> expiration;
    jsg::Optional<int> expirationTtl;
    jsg::Optional<kj::Maybe<jsg::Value>> metadata;

    JSG_STRUCT(expiration, expirationTtl, metadata);
    JSG_STRUCT_TS_OVERRIDE(KVNamespacePutOptions);
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

    JSG_TS_ROOT();

    JSG_TS_DEFINE(
      interface KVNamespaceListKey<Metadata, Key extends string = string> {
        name: Key;
        expiration?: number;
        metadata?: Metadata;
      }
      type KVNamespaceListResult<Metadata, Key extends string = string> =
        | { list_complete: false; keys: KVNamespaceListKey<Metadata, Key>[]; cursor: string; }
        | { list_complete: true; keys: KVNamespaceListKey<Metadata, Key>[]; };
    );
    // `Metadata` before `Key` type parameter for backwards-compatibility with `workers-types@3`.
    // `Key` is also an optional type parameter, which must come after required parameters.

    JSG_TS_OVERRIDE(KVNamespace<Key extends string = string> {
      get(key: Key, options?: Partial<KVNamespaceGetOptions<undefined>>): Promise<string | null>;
      get(key: Key, type: "text"): Promise<string | null>;
      get<ExpectedValue = unknown>(key: Key, type: "json"): Promise<ExpectedValue | null>;
      get(key: Key, type: "arrayBuffer"): Promise<ArrayBuffer | null>;
      get(key: Key, type: "stream"): Promise<ReadableStream | null>;
      get(key: Key, options?: KVNamespaceGetOptions<"text">): Promise<string | null>;
      get<ExpectedValue = unknown>(key: Key, options?: KVNamespaceGetOptions<"json">): Promise<ExpectedValue | null>;
      get(key: Key, options?: KVNamespaceGetOptions<"arrayBuffer">): Promise<string | null>;
      get(key: Key, options?: KVNamespaceGetOptions<"stream">): Promise<string | null>;

      list<Metadata = unknown>(options?: KVNamespaceListOptions): Promise<KVNamespaceListResult<Metadata, Key>>;

      put(key: Key, value: string | ArrayBuffer | ArrayBufferView | ReadableStream, options?: KVNamespacePutOptions): Promise<void>;

      getWithMetadata<Metadata = unknown>(key: Key, options?: Partial<KVNamespaceGetOptions<undefined>>): Promise<KVNamespaceGetWithMetadataResult<string, Metadata>>;
      getWithMetadata<Metadata = unknown>(key: Key, type: "text"): Promise<KVNamespaceGetWithMetadataResult<string, Metadata>>;
      getWithMetadata<ExpectedValue = unknown, Metadata = unknown>(key: Key, type: "json"): Promise<KVNamespaceGetWithMetadataResult<ExpectedValue, Metadata>>;
      getWithMetadata<Metadata = unknown>(key: Key, type: "arrayBuffer"): Promise<KVNamespaceGetWithMetadataResult<ArrayBuffer, Metadata>>;
      getWithMetadata<Metadata = unknown>(key: Key, type: "stream"): Promise<KVNamespaceGetWithMetadataResult<ReadableStream, Metadata>>;
      getWithMetadata<Metadata = unknown>(key: Key, options: KVNamespaceGetOptions<"text">): Promise<KVNamespaceGetWithMetadataResult<string, Metadata>>;
      getWithMetadata<ExpectedValue = unknown, Metadata = unknown>(key: Key, options: KVNamespaceGetOptions<"json">): Promise<KVNamespaceGetWithMetadataResult<ExpectedValue, Metadata>>;
      getWithMetadata<Metadata = unknown>(key: Key, options: KVNamespaceGetOptions<"arrayBuffer">): Promise<KVNamespaceGetWithMetadataResult<ArrayBuffer, Metadata>>;
      getWithMetadata<Metadata = unknown>(key: Key, options: KVNamespaceGetOptions<"stream">): Promise<KVNamespaceGetWithMetadataResult<ReadableStream, Metadata>>;

      delete(key: Key): Promise<void>;
    });
  }

private:
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
