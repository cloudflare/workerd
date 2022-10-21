// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "r2-rpc.h"

#include <workerd/jsg/jsg.h>
#include <workerd/api/http.h>

namespace workerd::api::public_beta {

kj::Array<kj::byte> cloneByteArray(const kj::Array<kj::byte>& arr);

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
  };

  struct Conditional {
    jsg::Optional<jsg::NonCoercible<kj::String>> etagMatches;
    jsg::Optional<jsg::NonCoercible<kj::String>> etagDoesNotMatch;
    jsg::Optional<kj::Date> uploadedBefore;
    jsg::Optional<kj::Date> uploadedAfter;
    jsg::Optional<bool> secondsGranularity;

    JSG_STRUCT(etagMatches, etagDoesNotMatch, uploadedBefore, uploadedAfter, secondsGranularity);
  };

  struct GetOptions {
    jsg::Optional<kj::OneOf<Conditional, jsg::Ref<Headers>>> onlyIf;
    jsg::Optional<kj::OneOf<Range, jsg::Ref<Headers>>> range;

    JSG_STRUCT(onlyIf, range);
  };

  struct StringChecksums {
    jsg::Optional<kj::String> md5;
    jsg::Optional<kj::String> sha1;
    jsg::Optional<kj::String> sha256;
    jsg::Optional<kj::String> sha384;
    jsg::Optional<kj::String> sha512;

    JSG_STRUCT(md5, sha1, sha256, sha384, sha512);
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
  };

  struct ListOptions {
    jsg::Optional<int> limit;
    jsg::Optional<jsg::NonCoercible<kj::String>> prefix;
    jsg::Optional<jsg::NonCoercible<kj::String>> cursor;
    jsg::Optional<jsg::NonCoercible<kj::String>> delimiter;
    jsg::Optional<jsg::NonCoercible<kj::String>> startAfter;
    jsg::Optional<kj::Array<jsg::NonCoercible<kj::String>>> include;

    JSG_STRUCT(limit, prefix, cursor, delimiter, startAfter, include);
  };

  jsg::Promise<kj::Maybe<jsg::Ref<HeadResult>>> head(jsg::Lock& js, kj::String key,
      const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType);

  jsg::Promise<kj::OneOf<kj::Maybe<jsg::Ref<GetResult>>, jsg::Ref<HeadResult>>> get(
      jsg::Lock& js, kj::String key, jsg::Optional<GetOptions> options,
      const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType);
  jsg::Promise<kj::Maybe<jsg::Ref<HeadResult>>> put(jsg::Lock& js,
      kj::String key, kj::Maybe<R2PutValue> value, jsg::Optional<PutOptions> options,
      const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType);
  jsg::Promise<void> delete_(jsg::Lock& js, kj::OneOf<kj::String, kj::Array<kj::String>> keys,
      const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType);
  jsg::Promise<ListResult> list(jsg::Lock& js, jsg::Optional<ListOptions> options,
      const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType);

  JSG_RESOURCE_TYPE(R2Bucket) {
    JSG_METHOD(head);
    JSG_METHOD(get);
    JSG_METHOD(put);
    JSG_METHOD_NAMED(delete, delete_);
    JSG_METHOD(list);
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
};

#define EW_R2_PUBLIC_BETA_ISOLATE_TYPES \
  api::R2Error, \
  api::public_beta::R2Bucket, \
  api::public_beta::R2Bucket::HeadResult, \
  api::public_beta::R2Bucket::GetResult, \
  api::public_beta::R2Bucket::Range, \
  api::public_beta::R2Bucket::Conditional, \
  api::public_beta::R2Bucket::GetOptions, \
  api::public_beta::R2Bucket::PutOptions, \
  api::public_beta::R2Bucket::Checksums, \
  api::public_beta::R2Bucket::StringChecksums, \
  api::public_beta::R2Bucket::HttpMetadata, \
  api::public_beta::R2Bucket::ListOptions, \
  api::public_beta::R2Bucket::ListResult
// The list of r2-bucket.h types that are added to worker.c++'s JSG_DECLARE_ISOLATE_TYPE

}  // namespace workerd::api::public_beta
