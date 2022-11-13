// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "r2-rpc.h"

#include <workerd/jsg/jsg.h>
#include <workerd/api/http.h>
#include <workerd/api/r2-api.capnp.h>
#include <capnp/compat/json.h>
#include <workerd/util/http-util.h>

namespace workerd::api::public_beta {

kj::Array<kj::byte> cloneByteArray(const kj::Array<kj::byte>& arr);

class R2MultipartUpload;

class R2Bucket: public jsg::Object {
  // A capability to an R2 Bucket.

protected:
  struct friend_tag_t {};

  struct FeatureFlags {
    FeatureFlags(CompatibilityFlags::Reader featureFlags);

    bool listHonorsIncludes;
  };

public:
  explicit R2Bucket(CompatibilityFlags::Reader featureFlags, uint clientIndex)
      : featureFlags(featureFlags), clientIndex(clientIndex) {}
  // `clientIndex` is what to pass to IoContext::getHttpClient() to get an HttpClient
  // representing this namespace.

  explicit R2Bucket(FeatureFlags featureFlags, uint clientIndex, kj::String bucket, friend_tag_t)
      : featureFlags(featureFlags), clientIndex(clientIndex), adminBucket(kj::mv(bucket)) {}

  struct Range {
    jsg::Optional<double> offset;
    jsg::Optional<double> length;
    jsg::Optional<double> suffix;

    JSG_STRUCT(offset, length, suffix);
    JSG_STRUCT_TS_OVERRIDE(type R2Range =
      | { offset: number; length?: number }
      | { offset?: number; length: number }
      | { suffix: number }
    );
  };

  struct Conditional {
    jsg::Optional<jsg::NonCoercible<kj::String>> etagMatches;
    jsg::Optional<jsg::NonCoercible<kj::String>> etagDoesNotMatch;
    jsg::Optional<kj::Date> uploadedBefore;
    jsg::Optional<kj::Date> uploadedAfter;
    jsg::Optional<bool> secondsGranularity;

    JSG_STRUCT(etagMatches, etagDoesNotMatch, uploadedBefore, uploadedAfter, secondsGranularity);
    JSG_STRUCT_TS_OVERRIDE(R2Conditional);
  };

  struct GetOptions {
    jsg::Optional<kj::OneOf<Conditional, jsg::Ref<Headers>>> onlyIf;
    jsg::Optional<kj::OneOf<Range, jsg::Ref<Headers>>> range;

    JSG_STRUCT(onlyIf, range);
    JSG_STRUCT_TS_OVERRIDE(R2GetOptions);
  };

  struct StringChecksums {
    jsg::Optional<kj::String> md5;
    jsg::Optional<kj::String> sha1;
    jsg::Optional<kj::String> sha256;
    jsg::Optional<kj::String> sha384;
    jsg::Optional<kj::String> sha512;

    JSG_STRUCT(md5, sha1, sha256, sha384, sha512);
    JSG_STRUCT_TS_OVERRIDE(R2StringChecksums);
  };

  class Checksums: public jsg::Object {
  public:
    Checksums(
      jsg::Optional<kj::Array<kj::byte>> md5,
      jsg::Optional<kj::Array<kj::byte>> sha1,
      jsg::Optional<kj::Array<kj::byte>> sha256,
      jsg::Optional<kj::Array<kj::byte>> sha384,
      jsg::Optional<kj::Array<kj::byte>> sha512
    ):
      md5(kj::mv(md5)),
      sha1(kj::mv(sha1)),
      sha256(kj::mv(sha256)),
      sha384(kj::mv(sha384)),
      sha512(kj::mv(sha512)) {}

    jsg::Optional<kj::Array<kj::byte>> getMd5() const { return md5.map(cloneByteArray); }
    jsg::Optional<kj::Array<kj::byte>> getSha1() const { return sha1.map(cloneByteArray); }
    jsg::Optional<kj::Array<kj::byte>> getSha256() const { return sha256.map(cloneByteArray); }
    jsg::Optional<kj::Array<kj::byte>> getSha384() const { return sha384.map(cloneByteArray); }
    jsg::Optional<kj::Array<kj::byte>> getSha512() const { return sha512.map(cloneByteArray); }

    StringChecksums toJSON();

    JSG_RESOURCE_TYPE(Checksums) {
      JSG_LAZY_READONLY_INSTANCE_PROPERTY(md5, getMd5);
      JSG_LAZY_READONLY_INSTANCE_PROPERTY(sha1, getSha1);
      JSG_LAZY_READONLY_INSTANCE_PROPERTY(sha256, getSha256);
      JSG_LAZY_READONLY_INSTANCE_PROPERTY(sha384, getSha384);
      JSG_LAZY_READONLY_INSTANCE_PROPERTY(sha512, getSha512);
      JSG_METHOD(toJSON);
      JSG_TS_OVERRIDE(R2Checksums);
    }

    jsg::Optional<kj::Array<kj::byte>> md5;
    jsg::Optional<kj::Array<kj::byte>> sha1;
    jsg::Optional<kj::Array<kj::byte>> sha256;
    jsg::Optional<kj::Array<kj::byte>> sha384;
    jsg::Optional<kj::Array<kj::byte>> sha512;
  };

  struct HttpMetadata {
    static HttpMetadata fromRequestHeaders(jsg::Lock& js, Headers& h);

    jsg::Optional<kj::String> contentType;
    jsg::Optional<kj::String> contentLanguage;
    jsg::Optional<kj::String> contentDisposition;
    jsg::Optional<kj::String> contentEncoding;
    jsg::Optional<kj::String> cacheControl;
    jsg::Optional<kj::Date> cacheExpiry;

    JSG_STRUCT(contentType, contentLanguage, contentDisposition,
                contentEncoding, cacheControl, cacheExpiry);
    JSG_STRUCT_TS_OVERRIDE(R2HTTPMetadata);

    HttpMetadata clone() const;
  };

  struct PutOptions {
    jsg::Optional<kj::OneOf<Conditional, jsg::Ref<Headers>>> onlyIf;
    jsg::Optional<kj::OneOf<HttpMetadata, jsg::Ref<Headers>>> httpMetadata;
    jsg::Optional<jsg::Dict<kj::String>> customMetadata;
    jsg::Optional<kj::OneOf<kj::Array<kj::byte>, jsg::NonCoercible<kj::String>>> md5;
    jsg::Optional<kj::OneOf<kj::Array<kj::byte>, jsg::NonCoercible<kj::String>>> sha1;
    jsg::Optional<kj::OneOf<kj::Array<kj::byte>, jsg::NonCoercible<kj::String>>> sha256;
    jsg::Optional<kj::OneOf<kj::Array<kj::byte>, jsg::NonCoercible<kj::String>>> sha384;
    jsg::Optional<kj::OneOf<kj::Array<kj::byte>, jsg::NonCoercible<kj::String>>> sha512;

    JSG_STRUCT(onlyIf, httpMetadata, customMetadata, md5, sha1, sha256, sha384, sha512);
    JSG_STRUCT_TS_OVERRIDE(R2PutOptions);
  };

  struct MultipartOptions {
    jsg::Optional<kj::OneOf<HttpMetadata, jsg::Ref<Headers>>> httpMetadata;
    jsg::Optional<jsg::Dict<kj::String>> customMetadata;

    JSG_STRUCT(httpMetadata, customMetadata);
    JSG_STRUCT_TS_OVERRIDE(R2MultipartOptions);
  };

  class HeadResult: public jsg::Object {
  public:
    HeadResult(kj::String name, kj::String version, double size,
               kj::String etag, jsg::Ref<Checksums> checksums, kj::Date uploaded, jsg::Optional<HttpMetadata> httpMetadata,
               jsg::Optional<jsg::Dict<kj::String>> customMetadata, jsg::Optional<Range> range):
        name(kj::mv(name)), version(kj::mv(version)), size(size), etag(kj::mv(etag)),
        checksums(kj::mv(checksums)), uploaded(uploaded), httpMetadata(kj::mv(httpMetadata)),
        customMetadata(kj::mv(customMetadata)), range(kj::mv(range)) {}

    kj::String getName() const { return kj::str(name); }
    kj::String getVersion() const { return kj::str(version); }
    double getSize() const { return size; }
    kj::String getEtag() const { return kj::str(etag); }
    kj::String getHttpEtag() const { return kj::str('"', etag, '"'); }
    jsg::Ref<Checksums> getChecksums() { return checksums.addRef();}
    kj::Date getUploaded() const { return uploaded; }

    jsg::Optional<HttpMetadata> getHttpMetadata() const {
      return httpMetadata.map([](const HttpMetadata& m) { return m.clone(); });
    }

    const jsg::Optional<jsg::Dict<kj::String>> getCustomMetadata() const {
      return customMetadata.map([](const jsg::Dict<kj::String>& m) {
        return jsg::Dict<kj::String>{
          .fields = KJ_MAP(f, m.fields) {
            return jsg::Dict<kj::String>::Field{
              .name = kj::str(f.name), .value = kj::str(f.value)
            };
          },
        };
      });
    }


    jsg::Optional<Range> getRange() { return range; }

    void writeHttpMetadata(jsg::Lock& js, Headers& headers);

    JSG_RESOURCE_TYPE(HeadResult) {
      JSG_LAZY_READONLY_INSTANCE_PROPERTY(key, getName);
      JSG_LAZY_READONLY_INSTANCE_PROPERTY(version, getVersion);
      JSG_LAZY_READONLY_INSTANCE_PROPERTY(size, getSize);
      JSG_LAZY_READONLY_INSTANCE_PROPERTY(etag, getEtag);
      JSG_LAZY_READONLY_INSTANCE_PROPERTY(httpEtag, getHttpEtag);
      JSG_LAZY_READONLY_INSTANCE_PROPERTY(checksums, getChecksums);
      JSG_LAZY_READONLY_INSTANCE_PROPERTY(uploaded, getUploaded);
      JSG_LAZY_READONLY_INSTANCE_PROPERTY(httpMetadata, getHttpMetadata);
      JSG_LAZY_READONLY_INSTANCE_PROPERTY(customMetadata, getCustomMetadata);
      JSG_LAZY_READONLY_INSTANCE_PROPERTY(range, getRange);
      JSG_METHOD(writeHttpMetadata);
      JSG_TS_OVERRIDE(R2Object);
    }

  protected:
    kj::String name;
    kj::String version;
    double size;
    kj::String etag;
    jsg::Ref<Checksums> checksums;
    kj::Date uploaded;
    jsg::Optional<HttpMetadata> httpMetadata;
    jsg::Optional<jsg::Dict<kj::String>> customMetadata;

    jsg::Optional<Range> range;
    friend class R2Bucket;
  };

  class GetResult: public HeadResult {
  public:
    GetResult(kj::String name, kj::String version, double size,
              kj::String etag, jsg::Ref<Checksums> checksums, kj::Date uploaded, jsg::Optional<HttpMetadata> httpMetadata,
              jsg::Optional<jsg::Dict<kj::String>> customMetadata, jsg::Optional<Range> range,
              jsg::Ref<ReadableStream> body)
      : HeadResult(
          kj::mv(name), kj::mv(version), size, kj::mv(etag), kj::mv(checksums), uploaded,
          kj::mv(KJ_ASSERT_NONNULL(httpMetadata)), kj::mv(KJ_ASSERT_NONNULL(customMetadata)), range),
          body(kj::mv(body)) {}

    jsg::Ref<ReadableStream> getBody() {
      return body.addRef();
    }

    bool getBodyUsed() {
      return body->isDisturbed();
    }

    jsg::Promise<kj::Array<kj::byte>> arrayBuffer(jsg::Lock& js);
    jsg::Promise<kj::String> text(jsg::Lock& js);
    jsg::Promise<jsg::Value> json(jsg::Lock& js);
    jsg::Promise<jsg::Ref<Blob>> blob(jsg::Lock& js);

    JSG_RESOURCE_TYPE(GetResult) {
      JSG_INHERIT(HeadResult);
      JSG_READONLY_PROTOTYPE_PROPERTY(body, getBody);
      JSG_READONLY_PROTOTYPE_PROPERTY(bodyUsed, getBodyUsed);
      JSG_METHOD(arrayBuffer);
      JSG_METHOD(text);
      JSG_METHOD(json);
      JSG_METHOD(blob);
      JSG_TS_OVERRIDE(R2ObjectBody {
        json<T>(): Promise<T>;
      });
    }
  private:
    jsg::Ref<ReadableStream> body;
  };

  struct ListResult {
    kj::Array<jsg::Ref<HeadResult>> objects;
    bool truncated;
    jsg::Optional<kj::String> cursor;
    kj::Array<kj::String> delimitedPrefixes;

    JSG_STRUCT(objects, truncated, cursor, delimitedPrefixes);
    JSG_STRUCT_TS_OVERRIDE(R2Objects);
  };

  struct ListOptions {
    jsg::Optional<int> limit;
    jsg::Optional<jsg::NonCoercible<kj::String>> prefix;
    jsg::Optional<jsg::NonCoercible<kj::String>> cursor;
    jsg::Optional<jsg::NonCoercible<kj::String>> delimiter;
    jsg::Optional<jsg::NonCoercible<kj::String>> startAfter;
    jsg::Optional<kj::Array<jsg::NonCoercible<kj::String>>> include;

    JSG_STRUCT(limit, prefix, cursor, delimiter, startAfter, include);
    JSG_STRUCT_TS_OVERRIDE(type R2ListOptions = never);
    // Delete the auto-generated ListOptions definition, we instead define it
    // with R2Bucket so we can access compatibility flags. Note, even though
    // we're deleting the definition, all definitions will still be renamed
    // from `R2BucketListOptions` to `R2ListOptions`.
  };

  jsg::Promise<kj::Maybe<jsg::Ref<HeadResult>>> head(
      jsg::Lock& js, kj::String key,
      const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType
  );
  jsg::Promise<kj::OneOf<kj::Maybe<jsg::Ref<GetResult>>, jsg::Ref<HeadResult>>> get(
      jsg::Lock& js, kj::String key, jsg::Optional<GetOptions> options,
      const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType
  );
  jsg::Promise<kj::Maybe<jsg::Ref<HeadResult>>> put(jsg::Lock& js,
      kj::String key, kj::Maybe<R2PutValue> value, jsg::Optional<PutOptions> options,
      const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType
  );
  jsg::Promise<jsg::Ref<R2MultipartUpload>> createMultipartUpload(
      jsg::Lock& js, kj::String key, jsg::Optional<MultipartOptions> options,
      const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType
  );
  jsg::Ref<R2MultipartUpload> resumeMultipartUpload(
      jsg::Lock& js, kj::String key, kj::String uploadId,
      const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType
  );
  jsg::Promise<void> delete_(
      jsg::Lock& js, kj::OneOf<kj::String, kj::Array<kj::String>> keys,
      const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType
  );
  jsg::Promise<ListResult> list(
      jsg::Lock& js, jsg::Optional<ListOptions> options,
      const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType
  );

  JSG_RESOURCE_TYPE(R2Bucket, CompatibilityFlags::Reader flags) {
    JSG_METHOD(head);
    JSG_METHOD(get);
    JSG_METHOD(put);
    JSG_METHOD(createMultipartUpload);
    JSG_METHOD(resumeMultipartUpload);
    JSG_METHOD_NAMED(delete, delete_);
    JSG_METHOD(list);

    JSG_TS_ROOT();
    JSG_TS_OVERRIDE({
      get(key: string, options: R2GetOptions & { onlyIf: R2BucketConditional | Headers }): Promise<R2ObjectBody | R2Object | null>;
      get(key: string, options?: R2GetOptions): Promise<R2ObjectBody | null>;

      put(key: string, value: ReadableStream | ArrayBuffer | ArrayBufferView | string | null | Blob, options?: R2PutOptions): Promise<R2Object>;
    });
    // Exclude `R2Object` from `get` return type if `onlyIf` not specified, and exclude `null` from `put` return type

    // Rather than using the auto-generated R2ListOptions definition, we define
    // it here so we can access compatibility flags from JSG_RESOURCE_TYPE.
    if (flags.getR2ListHonorIncludeFields()) {
      JSG_TS_DEFINE(interface R2ListOptions {
        limit?: number;
        prefix?: string;
        cursor?: string;
        delimiter?: string;
        startAfter?: string;
        include?: ("httpMetadata" | "customMetadata")[];
      });
    } else {
      JSG_TS_DEFINE(interface R2ListOptions {
        limit?: number;
        prefix?: string;
        cursor?: string;
        delimiter?: string;
        startAfter?: string;
      });
      // Omit `include` field if compatibility flag disabled as ignored
    }
  }

  struct UnwrappedConditional {
    UnwrappedConditional(jsg::Lock& js, Headers& h);
    UnwrappedConditional(const Conditional& c);

    kj::Maybe<kj::String> etagMatches;
    kj::Maybe<kj::String> etagDoesNotMatch;
    kj::Maybe<kj::Date> uploadedBefore;
    kj::Maybe<kj::Date> uploadedAfter;
    bool secondsGranularity = false;
  };

protected:
  kj::Maybe<kj::StringPtr> adminBucketName() const {
    return adminBucket;
  }

private:
  FeatureFlags featureFlags;
  uint clientIndex;
  kj::Maybe<kj::String> adminBucket;

  friend class R2Admin;
  friend class R2MultipartUpload;
};

kj::Maybe<jsg::Ref<R2Bucket::HeadResult>> parseHeadResultWrapper(
  kj::StringPtr action, R2Result& r2Result, const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType);
// Non-generic wrapper avoid moving the parseObjectMetadata implementation into this header file
// by making use of dynamic dispatch.

}  // namespace workerd::api::public_beta

