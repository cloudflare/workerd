// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "r2-bucket.h"

#include "r2-impl-utils.h"
#include "r2-multipart.h"
#include "r2-rpc.h"
#include "util.h"

#include <workerd/api/http.h>
#include <workerd/api/r2-api.capnp.h>
#include <workerd/api/streams.h>
#include <workerd/util/http-util.h>
#include <workerd/util/mimetype.h>

#include <capnp/compat/json.h>
#include <capnp/message.h>
#include <kj/compat/http.h>
#include <kj/encoding.h>

#include <array>

namespace workerd::api::public_beta {
kj::Own<kj::HttpClient> r2GetClient(
    IoContext& context, uint subrequestChannel, R2UserTracing user) {
  kj::Vector<Span::Tag> tags;
  tags.add("rpc.service"_kjc, kj::str("r2"_kjc));
  tags.add(user.method.key, kj::str(user.method.value));
  KJ_IF_SOME(b, user.bucket) {
    tags.add("cloudflare.r2.bucket"_kjc, kj::str(b));
  }
  KJ_IF_SOME(tag, user.extraTag) {
    tags.add(tag.key, kj::str(tag.value));
  }

  return context.getHttpClientWithSpans(subrequestChannel, true, kj::none, user.op, kj::mv(tags));
}

// TODO(perf): Would be nice to expose the v8 internals for parsing a date/stringifying it as
// something an embedder can call directly rather than doing this rigamarole. It would also avoid
// concerns about the user overriding the methods we're invoking.
static kj::Date parseDate(jsg::Lock& js, kj::StringPtr value) {
  return js.date(value);
}

static jsg::ByteString toUTCString(jsg::Lock& js, kj::Date date) {
  return js.date(date).toUTCString(js);
}

enum class OptionalMetadata : uint16_t {
  Http = static_cast<uint8_t>(R2ListRequest::IncludeField::HTTP),
  Custom = static_cast<uint8_t>(R2ListRequest::IncludeField::CUSTOM),
};

template <typename T>
concept HeadResultT = std::is_base_of_v<R2Bucket::HeadResult, T>;

template <HeadResultT T, typename... Args>
static jsg::Ref<T> parseObjectMetadata(jsg::Lock& js,
    R2HeadResponse::Reader responseReader,
    kj::ArrayPtr<const OptionalMetadata> expectedOptionalFields,
    Args&&... args) {
  // optionalFieldsExpected is initialized by default to HTTP + CUSTOM if the user doesn't specify
  // anything. If they specify the empty array, then nothing is returned.
  kj::Date uploaded =
      kj::UNIX_EPOCH + responseReader.getUploadedMillisecondsSinceEpoch() * kj::MILLISECONDS;

  jsg::Optional<R2Bucket::HttpMetadata> httpMetadata;
  if (responseReader.hasHttpFields()) {
    R2Bucket::HttpMetadata m;

    auto httpFields = responseReader.getHttpFields();
    if (httpFields.hasContentType()) {
      m.contentType = kj::str(httpFields.getContentType());
    }
    if (httpFields.hasContentDisposition()) {
      m.contentDisposition = kj::str(httpFields.getContentDisposition());
    }
    if (httpFields.hasContentEncoding()) {
      m.contentEncoding = kj::str(httpFields.getContentEncoding());
    }
    if (httpFields.hasContentLanguage()) {
      m.contentLanguage = kj::str(httpFields.getContentLanguage());
    }
    if (httpFields.hasCacheControl()) {
      m.cacheControl = kj::str(httpFields.getCacheControl());
    }
    if (httpFields.getCacheExpiry() != 0xffffffffffffffff) {
      m.cacheExpiry = kj::UNIX_EPOCH + httpFields.getCacheExpiry() * kj::MILLISECONDS;
    }

    httpMetadata = kj::mv(m);
  } else if (std::find(expectedOptionalFields.begin(), expectedOptionalFields.end(),
                 OptionalMetadata::Http) != expectedOptionalFields.end()) {
    // HTTP metadata was asked for but the object didn't have anything.
    httpMetadata = R2Bucket::HttpMetadata{};
  }

  jsg::Optional<jsg::Dict<kj::String>> customMetadata;
  if (responseReader.hasCustomFields()) {
    customMetadata = jsg::Dict<kj::String>{.fields =
                                               KJ_MAP(field, responseReader.getCustomFields()) {
      jsg::Dict<kj::String>::Field item;
      item.name = kj::str(field.getK());
      item.value = kj::str(field.getV());
      return item;
    }};
  } else if (std::find(expectedOptionalFields.begin(), expectedOptionalFields.end(),
                 OptionalMetadata::Custom) != expectedOptionalFields.end()) {
    // Custom metadata was asked for but the object didn't have anything.
    customMetadata = jsg::Dict<kj::String>{};
  }

  jsg::Optional<R2Bucket::Range> range;

  if (responseReader.hasRange()) {
    auto rangeBuilder = responseReader.getRange();
    range = R2Bucket::Range{
      .offset = static_cast<double>(rangeBuilder.getOffset()),
      .length = static_cast<double>(rangeBuilder.getLength()),
    };
  }

  jsg::Ref<R2Bucket::Checksums> checksums =
      js.alloc<R2Bucket::Checksums>(kj::none, kj::none, kj::none, kj::none, kj::none);

  if (responseReader.hasChecksums()) {
    R2Checksums::Reader checksumsBuilder = responseReader.getChecksums();
    if (checksumsBuilder.hasMd5()) {
      // Note that we don't check the length of checksums in here. We know
      // that some artifact were stored with truncated checksums (e.g. 8 bytes
      // instead of 16 for some). We're not validating the checksum lengths
      // here and instead we're just passing them through.
      checksums->md5 = kj::heapArray(checksumsBuilder.getMd5());
    }
    if (checksumsBuilder.hasSha1()) {
      checksums->sha1 = kj::heapArray(checksumsBuilder.getSha1());
    }
    if (checksumsBuilder.hasSha256()) {
      checksums->sha256 = kj::heapArray(checksumsBuilder.getSha256());
    }
    if (checksumsBuilder.hasSha384()) {
      checksums->sha384 = kj::heapArray(checksumsBuilder.getSha384());
    }
    if (checksumsBuilder.hasSha512()) {
      checksums->sha512 = kj::heapArray(checksumsBuilder.getSha512());
    }
  }

  jsg::Optional<kj::String> ssecKeyMd5;

  if (responseReader.hasSsec()) {
    auto ssecBuilder = responseReader.getSsec();
    ssecKeyMd5 = kj::str(ssecBuilder.getKeyMd5());
  }

  return js.alloc<T>(kj::str(responseReader.getName()), kj::str(responseReader.getVersion()),
      responseReader.getSize(), kj::str(responseReader.getEtag()), kj::mv(checksums), uploaded,
      kj::mv(httpMetadata), kj::mv(customMetadata), range,
      kj::str(responseReader.getStorageClass()), kj::mv(ssecKeyMd5), kj::fwd<Args>(args)...);
}

template <HeadResultT T, typename... Args>
static kj::Maybe<jsg::Ref<T>> parseObjectMetadata(jsg::Lock& js,
    kj::StringPtr action,
    R2Result& r2Result,
    const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType,
    Args&&... args) {
  if (r2Result.objectNotFound()) {
    return kj::none;
  }
  if (!r2Result.preconditionFailed()) {
    r2Result.throwIfError(action, errorType);
  }

  // Non-list operations always return these.
  std::array expectedFieldsOwned = {OptionalMetadata::Http, OptionalMetadata::Custom};
  kj::ArrayPtr<OptionalMetadata> expectedFields = {
    expectedFieldsOwned.data(), expectedFieldsOwned.size()};

  capnp::MallocMessageBuilder responseMessage;
  capnp::JsonCodec json;
  // Annoyingly our R2GetResponse alias isn't emitted.
  json.handleByAnnotation<R2HeadResponse>();
  auto responseBuilder = responseMessage.initRoot<R2HeadResponse>();
  json.decode(KJ_ASSERT_NONNULL(r2Result.metadataPayload), responseBuilder);

  return parseObjectMetadata<T>(js, responseBuilder, expectedFields, kj::fwd<Args>(args)...);
}

struct MetadataReturn {
  jsg::Dict<kj::String> customMetadata;
  R2Bucket::HttpMetadata httpMetadata;
};

template <typename Builder, typename Options>
void initMetadata(jsg::Lock& js, Builder& builder, Options& o) {
  KJ_IF_SOME(m, o.customMetadata) {
    auto fields = builder.initCustomFields(m.fields.size());
    for (size_t i = 0; i < m.fields.size(); i++) {
      fields[i].setK(m.fields[i].name);
      fields[i].setV(m.fields[i].value);
    }
  }
  KJ_IF_SOME(m, o.httpMetadata) {
    auto fields = builder.initHttpFields();
    auto httpMetadata = [&]() {
      KJ_SWITCH_ONEOF(m) {
        KJ_CASE_ONEOF(m, R2Bucket::HttpMetadata) {
          return kj::mv(m);
        }
        KJ_CASE_ONEOF(h, jsg::Ref<Headers>) {
          return R2Bucket::HttpMetadata::fromRequestHeaders(js, *h);
        }
      }
      KJ_UNREACHABLE;
    }();

    KJ_IF_SOME(ct, httpMetadata.contentType) {
      fields.setContentType(ct);
    }
    KJ_IF_SOME(ce, httpMetadata.contentEncoding) {
      fields.setContentEncoding(ce);
    }
    KJ_IF_SOME(cd, httpMetadata.contentDisposition) {
      fields.setContentDisposition(cd);
    }
    KJ_IF_SOME(cl, httpMetadata.contentLanguage) {
      fields.setContentLanguage(cl);
    }
    KJ_IF_SOME(cc, httpMetadata.cacheControl) {
      fields.setCacheControl(cc);
    }
    KJ_IF_SOME(ce, httpMetadata.cacheExpiry) {
      fields.setCacheExpiry((ce - kj::UNIX_EPOCH) / kj::MILLISECONDS);
    }
  }
}

template <typename Builder, typename Options>
void initGetOptions(jsg::Lock& js, Builder& builder, Options& o) {
  initOnlyIf(js, builder, o);
  initRange(js, builder, o);
  initSsec(js, builder, o);
}

static bool isQuotedEtag(kj::StringPtr etag) {
  return etag.startsWith("\"") && etag.endsWith("\"");
}

jsg::Promise<kj::Maybe<jsg::Ref<R2Bucket::HeadResult>>> R2Bucket::head(jsg::Lock& js,
    kj::String name,
    const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType,
    CompatibilityFlags::Reader flags) {
  return js.evalNow([&] {
    auto& context = IoContext::current();

    auto client = r2GetClient(context, clientIndex,
        {"r2_get"_kjc, {"rpc.method"_kjc, "GetObject"_kjc}, this->adminBucketName()});

    capnp::JsonCodec json;
    json.handleByAnnotation<R2BindingRequest>();
    json.setHasMode(capnp::HasMode::NON_DEFAULT);
    capnp::MallocMessageBuilder requestMessage;

    auto requestBuilder = requestMessage.initRoot<R2BindingRequest>();
    requestBuilder.setVersion(VERSION_PUBLIC_BETA);
    auto payloadBuilder = requestBuilder.initPayload();
    auto headBuilder = payloadBuilder.initHead();
    headBuilder.setObject(name);

    auto requestJson = json.encode(requestBuilder);
    kj::StringPtr components[1];
    auto path = fillR2Path(components, adminBucket);
    auto promise = doR2HTTPGetRequest(kj::mv(client), kj::mv(requestJson), path, jwt, flags);

    return context.awaitIo(js, kj::mv(promise), [&errorType](jsg::Lock& js, R2Result r2Result) {
      return parseObjectMetadata<HeadResult>(js, "head", r2Result, errorType);
    });
  });
}

R2Bucket::FeatureFlags::FeatureFlags(CompatibilityFlags::Reader featureFlags)
    : listHonorsIncludes(featureFlags.getR2ListHonorIncludeFields()) {}

jsg::Promise<kj::OneOf<kj::Maybe<jsg::Ref<R2Bucket::GetResult>>, jsg::Ref<R2Bucket::HeadResult>>>
R2Bucket::get(jsg::Lock& js,
    kj::String name,
    jsg::Optional<GetOptions> options,
    const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType,
    CompatibilityFlags::Reader flags) {
  return js.evalNow([&] {
    auto& context = IoContext::current();

    auto client = r2GetClient(context, clientIndex,
        {"r2_get"_kjc, {"rpc.method"_kjc, "GetObject"_kjc}, this->adminBucketName(),
          {{"cloudflare.r2.bucket"_kjc, name.asPtr()}}});

    capnp::JsonCodec json;
    json.handleByAnnotation<R2BindingRequest>();
    json.setHasMode(capnp::HasMode::NON_DEFAULT);
    capnp::MallocMessageBuilder requestMessage;

    auto requestBuilder = requestMessage.initRoot<R2BindingRequest>();
    requestBuilder.setVersion(VERSION_PUBLIC_BETA);
    auto payloadBuilder = requestBuilder.initPayload();
    auto getBuilder = payloadBuilder.initGet();
    getBuilder.setObject(name);

    KJ_IF_SOME(o, options) {
      initGetOptions(js, getBuilder, o);
    }
    auto requestJson = json.encode(requestBuilder);
    kj::StringPtr components[1];
    auto path = fillR2Path(components, adminBucket);
    auto promise = doR2HTTPGetRequest(kj::mv(client), kj::mv(requestJson), path, jwt, flags);

    return context.awaitIo(js, kj::mv(promise),
        [&context, &errorType](jsg::Lock& js,
            R2Result r2Result) -> kj::OneOf<kj::Maybe<jsg::Ref<GetResult>>, jsg::Ref<HeadResult>> {
      kj::OneOf<kj::Maybe<jsg::Ref<GetResult>>, jsg::Ref<HeadResult>> result;

      if (r2Result.preconditionFailed()) {
        result = KJ_ASSERT_NONNULL(parseObjectMetadata<HeadResult>(js, "get", r2Result, errorType));
      } else {
        jsg::Ref<ReadableStream> body = nullptr;

        KJ_IF_SOME(s, r2Result.stream) {
          body = js.alloc<ReadableStream>(context, kj::mv(s));
          r2Result.stream = kj::none;
        }
        result = parseObjectMetadata<GetResult>(js, "get", r2Result, errorType, kj::mv(body));
      }
      return result;
    });
  });
}

jsg::Promise<kj::Maybe<jsg::Ref<R2Bucket::HeadResult>>> R2Bucket::put(jsg::Lock& js,
    kj::String name,
    kj::Maybe<R2PutValue> value,
    jsg::Optional<PutOptions> options,
    const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType) {
  return js.evalNow([&] {
    auto cancelReader = kj::defer([&] {
      KJ_IF_SOME(v, value) {
        KJ_SWITCH_ONEOF(v) {
          KJ_CASE_ONEOF(v, jsg::Ref<ReadableStream>) {
            (*v).cancel(js,
                js.v8Error(kj::str(
                    "Stream cancelled because the associated put operation encountered an error.")));
          }
          KJ_CASE_ONEOF_DEFAULT {}
        }
      }
    });

    auto& context = IoContext::current();
    auto client = r2GetClient(context, clientIndex,
        {"r2_put"_kjc, {"rpc.method"_kjc, "PutObject"_kjc}, this->adminBucketName(),
          {{"cloudflare.r2.key"_kjc, name.asPtr()}}});

    capnp::JsonCodec json;
    json.handleByAnnotation<R2BindingRequest>();
    json.setHasMode(capnp::HasMode::NON_DEFAULT);
    capnp::MallocMessageBuilder requestMessage;

    auto requestBuilder = requestMessage.initRoot<R2BindingRequest>();
    requestBuilder.setVersion(VERSION_PUBLIC_BETA);
    auto payloadBuilder = requestBuilder.initPayload();
    auto putBuilder = payloadBuilder.initPut();
    putBuilder.setObject(name);

    bool hashAlreadySpecified = false;
    const auto verifyHashNotSpecified = [&] {
      JSG_REQUIRE(
          !hashAlreadySpecified, TypeError, "You cannot specify multiple hashing algorithms.");
      hashAlreadySpecified = true;
    };

    KJ_IF_SOME(o, options) {
      initOnlyIf(js, putBuilder, o);
      initMetadata(js, putBuilder, o);
      KJ_IF_SOME(md5, o.md5) {
        verifyHashNotSpecified();
        KJ_SWITCH_ONEOF(md5) {
          KJ_CASE_ONEOF(bin, jsg::BufferSource) {
            JSG_REQUIRE(bin.size() == 16, TypeError, "MD5 is 16 bytes, not ", bin.size());
            putBuilder.setMd5(bin.asArrayPtr());
          }
          KJ_CASE_ONEOF(hex, jsg::NonCoercible<kj::String>) {
            JSG_REQUIRE(hex.value.size() == 32, TypeError, "MD5 is 32 hex characters, not ",
                hex.value.size());
            const auto decoded = kj::decodeHex(hex.value);
            JSG_REQUIRE(!decoded.hadErrors, TypeError, "Provided MD5 wasn't a valid hex string");
            putBuilder.setMd5(decoded);
          }
        }
      }
      KJ_IF_SOME(sha1, o.sha1) {
        verifyHashNotSpecified();
        KJ_SWITCH_ONEOF(sha1) {
          KJ_CASE_ONEOF(bin, jsg::BufferSource) {
            JSG_REQUIRE(bin.size() == 20, TypeError, "SHA-1 is 20 bytes, not ", bin.size());
            putBuilder.setSha1(bin.asArrayPtr());
          }
          KJ_CASE_ONEOF(hex, jsg::NonCoercible<kj::String>) {
            JSG_REQUIRE(hex.value.size() == 40, TypeError, "SHA-1 is 40 hex characters, not ",
                hex.value.size());
            const auto decoded = kj::decodeHex(hex.value);
            JSG_REQUIRE(!decoded.hadErrors, TypeError, "Provided SHA-1 wasn't a valid hex string");
            putBuilder.setSha1(decoded);
          }
        }
      }
      KJ_IF_SOME(sha256, o.sha256) {
        verifyHashNotSpecified();
        KJ_SWITCH_ONEOF(sha256) {
          KJ_CASE_ONEOF(bin, jsg::BufferSource) {
            JSG_REQUIRE(bin.size() == 32, TypeError, "SHA-256 is 32 bytes, not ", bin.size());
            putBuilder.setSha256(bin.asArrayPtr());
          }
          KJ_CASE_ONEOF(hex, jsg::NonCoercible<kj::String>) {
            JSG_REQUIRE(hex.value.size() == 64, TypeError, "SHA-256 is 64 hex characters, not ",
                hex.value.size());
            const auto decoded = kj::decodeHex(hex.value);
            JSG_REQUIRE(
                !decoded.hadErrors, TypeError, "Provided SHA-256 wasn't a valid hex string");
            putBuilder.setSha256(decoded);
          }
        }
      }
      KJ_IF_SOME(sha384, o.sha384) {
        verifyHashNotSpecified();
        KJ_SWITCH_ONEOF(sha384) {
          KJ_CASE_ONEOF(bin, jsg::BufferSource) {
            JSG_REQUIRE(bin.size() == 48, TypeError, "SHA-384 is 48 bytes, not ", bin.size());
            putBuilder.setSha384(bin.asArrayPtr());
          }
          KJ_CASE_ONEOF(hex, jsg::NonCoercible<kj::String>) {
            JSG_REQUIRE(hex.value.size() == 96, TypeError, "SHA-384 is 96 hex characters, not ",
                hex.value.size());
            const auto decoded = kj::decodeHex(hex.value);
            JSG_REQUIRE(
                !decoded.hadErrors, TypeError, "Provided SHA-384 wasn't a valid hex string");
            putBuilder.setSha384(decoded);
          }
        }
      }
      KJ_IF_SOME(sha512, o.sha512) {
        verifyHashNotSpecified();
        KJ_SWITCH_ONEOF(sha512) {
          KJ_CASE_ONEOF(bin, jsg::BufferSource) {
            JSG_REQUIRE(bin.size() == 64, TypeError, "SHA-512 is 64 bytes, not ", bin.size());
            putBuilder.setSha512(bin.asArrayPtr());
          }
          KJ_CASE_ONEOF(hex, jsg::NonCoercible<kj::String>) {
            JSG_REQUIRE(hex.value.size() == 128, TypeError, "SHA-512 is 128 hex characters, not ",
                hex.value.size());
            const auto decoded = kj::decodeHex(hex.value);
            JSG_REQUIRE(
                !decoded.hadErrors, TypeError, "Provided SHA-512 wasn't a valid hex string");
            putBuilder.setSha512(decoded);
          }
        }
      }
      KJ_IF_SOME(s, o.storageClass) {
        putBuilder.setStorageClass(s);
      }
      initSsec(js, putBuilder, o);
    }

    auto requestJson = json.encode(requestBuilder);

    cancelReader.cancel();
    kj::StringPtr components[1];
    auto path = fillR2Path(components, adminBucket);
    auto promise =
        doR2HTTPPutRequest(kj::mv(client), kj::mv(value), kj::none, kj::mv(requestJson), path, jwt);

    return context.awaitIo(js, kj::mv(promise),
        [&errorType](jsg::Lock& js, R2Result r2Result) mutable -> kj::Maybe<jsg::Ref<HeadResult>> {
      if (r2Result.preconditionFailed()) {
        return kj::none;
      } else {
        return parseObjectMetadata<HeadResult>(js, "put", r2Result, errorType);
      }
    });
  });
}

jsg::Promise<kj::Maybe<jsg::Ref<R2Bucket::HeadResult>>> R2Bucket::copy(jsg::Lock& js,
    kj::String key,
    CopySource source,
    jsg::Optional<CopyOptions> options,
    const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType) {
  return js.evalNow([&] {
    auto& context = IoContext::current();
    auto client = r2GetClient(context, clientIndex,
        {"r2_copyObject"_kjc, {"rpc.method"_kjc, "CopyObject"_kjc}, this->adminBucketName(),
          {{"cloudflare.r2.key"_kjc, key.asPtr()}}});

    capnp::JsonCodec json;
    json.handleByAnnotation<R2BindingRequest>();
    json.setHasMode(capnp::HasMode::NON_DEFAULT);
    capnp::MallocMessageBuilder requestMessage;

    auto requestBuilder = requestMessage.initRoot<R2BindingRequest>();
    requestBuilder.setVersion(VERSION_PUBLIC_BETA);
    auto payloadBuilder = requestBuilder.initPayload();
    auto copyBuilder = payloadBuilder.initCopy();
    copyBuilder.setObject(key);
    auto sourceBuilder = copyBuilder.initSource();
    sourceBuilder.setBucket(source.bucket);
    sourceBuilder.setObject(source.object);
    initOnlyIf(js, sourceBuilder, source);
    initSsec(js, sourceBuilder, source);

    KJ_IF_SOME(o, options) {
      KJ_IF_SOME(metadataDirective, o.metadataDirective) {
        if (metadataDirective == "COPY" || metadataDirective == "REPLACE" ||
            metadataDirective == "MERGE") {
          copyBuilder.setMetadataDirective(metadataDirective);
        } else {
          JSG_FAIL_REQUIRE(RangeError, "Unsupported metadata directive value ", metadataDirective);
        }
      }
      initOnlyIf(js, copyBuilder, o);
      initMetadata(js, copyBuilder, o);
      KJ_IF_SOME(s, o.storageClass) {
        copyBuilder.setStorageClass(s);
      }
      initSsec(js, copyBuilder, o);
    }

    auto requestJson = json.encode(requestBuilder);

    kj::StringPtr components[1];
    auto path = fillR2Path(components, adminBucket);
    auto promise =
        doR2HTTPPutRequest(kj::mv(client), kj::none, kj::none, kj::mv(requestJson), path, jwt);

    return context.awaitIo(js, kj::mv(promise),
        [&errorType](jsg::Lock& js, R2Result r2Result) mutable -> kj::Maybe<jsg::Ref<HeadResult>> {
      if (r2Result.preconditionFailed()) {
        return kj::none;
      } else {
        return parseObjectMetadata<HeadResult>(js, "put", r2Result, errorType);
      }
    });
  });
}

jsg::Promise<jsg::Ref<R2MultipartUpload>> R2Bucket::createMultipartUpload(jsg::Lock& js,
    kj::String key,
    jsg::Optional<MultipartOptions> options,
    const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType) {
  return js.evalNow([&] {
    auto& context = IoContext::current();
    auto client = r2GetClient(context, clientIndex,
        {"r2_createMultipartUpload"_kjc, {"rpc.method"_kjc, "CreateMultipartUpload"_kjc},
          this->adminBucketName()});

    capnp::JsonCodec json;
    json.handleByAnnotation<R2BindingRequest>();
    json.setHasMode(capnp::HasMode::NON_DEFAULT);
    capnp::MallocMessageBuilder requestMessage;

    auto requestBuilder = requestMessage.initRoot<R2BindingRequest>();
    requestBuilder.setVersion(VERSION_PUBLIC_BETA);
    auto payloadBuilder = requestBuilder.initPayload();
    auto createMultipartUploadBuilder = payloadBuilder.initCreateMultipartUpload();
    createMultipartUploadBuilder.setObject(key);

    KJ_IF_SOME(o, options) {
      initMetadata(js, createMultipartUploadBuilder, o);
      KJ_IF_SOME(s, o.storageClass) {
        createMultipartUploadBuilder.setStorageClass(s);
      }
      initSsec(js, createMultipartUploadBuilder, o);
    }

    auto requestJson = json.encode(requestBuilder);
    kj::StringPtr components[1];
    auto path = fillR2Path(components, adminBucket);
    auto promise =
        doR2HTTPPutRequest(kj::mv(client), kj::none, kj::none, kj::mv(requestJson), path, jwt);

    return context.awaitIo(js, kj::mv(promise),
        [&errorType, key = kj::mv(key), this](jsg::Lock& js, R2Result r2Result) mutable {
      r2Result.throwIfError("createMultipartUpload", errorType);

      capnp::MallocMessageBuilder responseMessage;
      capnp::JsonCodec json;
      json.handleByAnnotation<R2CreateMultipartUploadResponse>();
      auto responseBuilder = responseMessage.initRoot<R2CreateMultipartUploadResponse>();

      json.decode(KJ_ASSERT_NONNULL(r2Result.metadataPayload), responseBuilder);
      kj::String uploadId = kj::str(responseBuilder.getUploadId());
      return js.alloc<R2MultipartUpload>(kj::mv(key), kj::mv(uploadId), JSG_THIS);
    });
  });
}

jsg::Ref<R2MultipartUpload> R2Bucket::resumeMultipartUpload(jsg::Lock& js,
    kj::String key,
    kj::String uploadId,
    const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType) {
  return js.alloc<R2MultipartUpload>(kj::mv(key), kj::mv(uploadId), JSG_THIS);
}

jsg::Promise<void> R2Bucket::delete_(jsg::Lock& js,
    kj::OneOf<kj::String, kj::Array<kj::String>> keys,
    const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType) {
  return js.evalNow([&] {
    auto& context = IoContext::current();
    auto deleteKey = [&]() {
      KJ_SWITCH_ONEOF(keys) {
        KJ_CASE_ONEOF(ks, kj::Array<kj::String>) {
          return kj::str(ks);
        }
        KJ_CASE_ONEOF(k, kj::String) {
          return kj::str(k);
        }
      }
      KJ_UNREACHABLE;
    }();
    auto client = r2GetClient(context, clientIndex,
        {"r2_delete"_kjc, {"rpc.method"_kjc, "DeleteObject"_kjc}, this->adminBucketName(),
          {{"cloudflare.r2.delete"_kjc, deleteKey.asPtr()}}});

    capnp::JsonCodec json;
    json.handleByAnnotation<R2BindingRequest>();
    capnp::MallocMessageBuilder requestMessage;

    auto requestBuilder = requestMessage.initRoot<R2BindingRequest>();
    requestBuilder.setVersion(VERSION_PUBLIC_BETA);
    auto deleteBuilder = requestBuilder.initPayload().initDelete();

    KJ_SWITCH_ONEOF(keys) {
      KJ_CASE_ONEOF(ks, kj::Array<kj::String>) {
        auto keys = deleteBuilder.initObjects(ks.size());
        for (unsigned int i = 0; i < ks.size(); i++) {
          keys.set(i, ks[i]);
        }
      }
      KJ_CASE_ONEOF(k, kj::String) {
        deleteBuilder.setObject(k);
      }
    }

    auto requestJson = json.encode(requestBuilder);

    kj::StringPtr components[1];
    auto path = fillR2Path(components, adminBucket);
    auto promise =
        doR2HTTPPutRequest(kj::mv(client), kj::none, kj::none, kj::mv(requestJson), path, jwt);

    return context.awaitIo(js, kj::mv(promise), [&errorType](jsg::Lock& js, R2Result r) {
      if (r.objectNotFound()) {
        return;
      }

      r.throwIfError("delete", errorType);
    });
  });
}

jsg::Promise<R2Bucket::ListResult> R2Bucket::list(jsg::Lock& js,
    jsg::Optional<ListOptions> options,
    const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType,
    CompatibilityFlags::Reader flags) {
  return js.evalNow([&] {
    auto& context = IoContext::current();
    auto client = r2GetClient(context, clientIndex,
        {"r2_list"_kjc, {"rpc.method"_kjc, "ListObjects"_kjc}, this->adminBucketName()});

    capnp::JsonCodec json;
    json.handleByAnnotation<R2BindingRequest>();
    json.setHasMode(capnp::HasMode::NON_DEFAULT);
    capnp::MallocMessageBuilder requestMessage;

    auto requestBuilder = requestMessage.initRoot<R2BindingRequest>();
    requestBuilder.setVersion(VERSION_PUBLIC_BETA);
    auto listBuilder = requestBuilder.initPayload().initList();

    kj::Vector<OptionalMetadata> expectedOptionalFields(2);

    KJ_IF_SOME(o, options) {
      KJ_IF_SOME(l, o.limit) {
        listBuilder.setLimit(l);
      }
      KJ_IF_SOME(p, o.prefix) {
        listBuilder.setPrefix(p.value);
      }
      KJ_IF_SOME(c, o.cursor) {
        listBuilder.setCursor(c.value);
      }
      KJ_IF_SOME(d, o.delimiter) {
        listBuilder.setDelimiter(d.value);
      }
      KJ_IF_SOME(d, o.startAfter) {
        listBuilder.setStartAfter(d.value);
      }
      KJ_IF_SOME(i, o.include) {
        using Field = typename jsg::Dict<uint16_t>::Field;
        static const std::array<Field, 2> fields = {
          Field{
            .name = kj::str("httpMetadata"),
            .value = static_cast<uint16_t>(R2ListRequest::IncludeField::HTTP),
          },
          Field{
            .name = kj::str("customMetadata"),
            .value = static_cast<uint16_t>(R2ListRequest::IncludeField::CUSTOM),
          },
        };

        expectedOptionalFields.clear();

        listBuilder.setInclude(KJ_MAP(reqField, i) {
          for (const auto& field: fields) {
            if (field.name == reqField.value) {
              expectedOptionalFields.add(static_cast<OptionalMetadata>(field.value));
              return field.value;
            }
          }

          JSG_FAIL_REQUIRE(RangeError, "Unsupported include value ", reqField.value);
        });
      } else if (featureFlags.listHonorsIncludes) {
        listBuilder.initInclude(0);
      }
    }

    // TODO(later): Add a sentry message (+ console warning) to check if we have users that aren't
    // asking for any optional metadata but are asking it in the result anyway just so that we can
    // kill all the compat flag logic.
    if (!featureFlags.listHonorsIncludes) {
      // Unconditionally send this so that when running against an R2 instance that does honor these
      // we do the right back-compat behavior.
      auto includes = listBuilder.initInclude(2);
      includes.set(0, static_cast<uint16_t>(R2ListRequest::IncludeField::HTTP));
      includes.set(1, static_cast<uint16_t>(R2ListRequest::IncludeField::CUSTOM));
      expectedOptionalFields.clear();
      expectedOptionalFields.add(OptionalMetadata::Http);
      expectedOptionalFields.add(OptionalMetadata::Custom);
    }

    auto requestJson = json.encode(requestBuilder);

    kj::StringPtr components[1];
    auto path = fillR2Path(components, adminBucket);
    auto promise = doR2HTTPGetRequest(kj::mv(client), kj::mv(requestJson), path, jwt, flags);

    return context.awaitIo(js, kj::mv(promise),
        [expectedOptionalFields = expectedOptionalFields.releaseAsArray(), &errorType](
            jsg::Lock& js, R2Result r2Result) {
      r2Result.throwIfError("list", errorType);

      R2Bucket::ListResult result;
      capnp::MallocMessageBuilder responseMessage;
      capnp::JsonCodec json;
      json.handleByAnnotation<R2ListResponse>();
      auto responseBuilder = responseMessage.initRoot<R2ListResponse>();

      json.decode(KJ_ASSERT_NONNULL(r2Result.metadataPayload), responseBuilder);

      result.objects = KJ_MAP(o, responseBuilder.getObjects()) {
        return parseObjectMetadata<HeadResult>(js, o, expectedOptionalFields);
      };
      result.truncated = responseBuilder.getTruncated();
      if (responseBuilder.hasCursor()) {
        result.cursor = kj::str(responseBuilder.getCursor());
      }
      if (responseBuilder.hasDelimitedPrefixes()) {
        result.delimitedPrefixes =
          KJ_MAP(e, responseBuilder.getDelimitedPrefixes()) { return kj::str(e); };
      }

      return kj::mv(result);
    });
  });
}

namespace {

kj::Array<R2Bucket::Etag> parseConditionalEtagHeader(kj::StringPtr condHeader,
    kj::Vector<R2Bucket::Etag> etagAccumulator = kj::Vector<R2Bucket::Etag>(),
    bool leadingCommaRequired = false) {
  // Vague recursion termination proof:
  // Stop condition triggers when no more etags and wildcards are found
  // => empty string also results in termination
  // There are 2 recursive calls in this function body, each of them always moves the start of the
  // condHeader to some value found in the condHeader + 1.
  // => upon each recursion, the size of condHeader is reduced by at least 1.
  // Eventually we must arrive at an empty string, hence triggering the stop condition.

  size_t nextWildcard = condHeader.findFirst('*').orDefault(SIZE_MAX);
  size_t nextQuotation = condHeader.findFirst('"').orDefault(SIZE_MAX);
  size_t nextWeak = condHeader.findFirst('W').orDefault(SIZE_MAX);
  size_t nextComma = condHeader.findFirst(',').orDefault(SIZE_MAX);

  if (nextQuotation == SIZE_MAX && nextWildcard == SIZE_MAX) {
    // Both of these being SIZE_MAX means no more wildcards or double quotes are left in the header.
    // When this is the case, there's no more useful etags that can potentially still be extracted.
    return etagAccumulator.releaseAsArray();
  }

  if (nextComma < nextWildcard && nextComma < nextQuotation && nextComma < nextWeak) {
    // Get rid of leading commas, this can happen during recursion because servers must deal with
    // empty list elements. E.g.: If-None-Match "abc", , "cdef" should be accepted by the server.
    // This slice is always safe, since we're at most setting start to the last index + 1,
    // which just results in an empty list if it's out of bounds by 1.
    return parseConditionalEtagHeader(condHeader.slice(nextComma + 1), kj::mv(etagAccumulator));
  } else if (leadingCommaRequired) {
    // we don't need to include nextComma in this min check since in this else branch nextComma is
    // always larger than at least one of nextWildcard, nextQuotation and nextWeak
    size_t firstEncounteredProblem = std::min({nextWildcard, nextQuotation, nextWeak});

    kj::String failureReason;
    if (firstEncounteredProblem == nextWildcard) {
      failureReason = kj::str("Encountered a wildcard character '*' instead.");
    } else if (firstEncounteredProblem == nextQuotation) {
      failureReason = kj::str("Encountered a double quote character '\"' instead. "
                              "This would otherwise indicate the start of a new strong etag.");
    } else if (firstEncounteredProblem == nextWeak) {
      failureReason = kj::str("Encountered a weak quotation character 'W' instead. "
                              "This would otherwise indicate the start of a new weak etag.");
    } else {
      KJ_FAIL_ASSERT(
          "We shouldn't be able to reach this point. The above etag parsing code is incorrect.");
    }

    // Did not find a leading comma, and we expected a leading comma before any further etags
    JSG_FAIL_REQUIRE(Error, "Comma was expected to separate etags. ", failureReason);
  }

  if (nextWildcard < nextQuotation) {
    // Unquoted wildcard found
    // remove all other etags since they're overridden by the wildcard anyways
    etagAccumulator.clear();
    struct R2Bucket::WildcardEtag etag = {};
    etagAccumulator.add(kj::mv(etag));
    return etagAccumulator.releaseAsArray();
  }
  if (nextQuotation < nextWildcard) {
    size_t etagValueStart = nextQuotation + 1;
    // Find closing quotation mark, instead of going by the next comma.
    // This is done because commas are allowed in etags, and double quotes are not.
    kj::Maybe<size_t> closingQuotation =
        condHeader.slice(etagValueStart).findFirst('"').map([=](size_t cq) {
      return cq + etagValueStart;
    });

    KJ_IF_SOME(cq, closingQuotation) {
      // Slice end is non inclusive, meaning that this drops the closingQuotation from the etag
      kj::String etagValue = kj::str(condHeader.slice(etagValueStart, cq));
      if (nextWeak < nextQuotation) {
        JSG_REQUIRE(condHeader.size() > nextWeak + 2 && condHeader[nextWeak + 1] == '/' &&
                nextWeak + 2 == nextQuotation,
            Error, "Weak etags must start with W/ and their value must be quoted");
        R2Bucket::WeakEtag etag = {kj::mv(etagValue)};
        etagAccumulator.add(kj::mv(etag));
      } else {
        R2Bucket::StrongEtag etag = {kj::mv(etagValue)};
        etagAccumulator.add(kj::mv(etag));
      }
      return parseConditionalEtagHeader(condHeader.slice(cq + 1), kj::mv(etagAccumulator), true);
    } else {
      JSG_FAIL_REQUIRE(Error, "Unclosed double quote for Etag");
    }
  } else {
    JSG_FAIL_REQUIRE(Error, "Invalid conditional header");
  }
}

kj::Array<R2Bucket::Etag> buildSingleEtagArray(kj::StringPtr etagValue) {
  kj::ArrayBuilder<R2Bucket::Etag> etagArrayBuilder = kj::heapArrayBuilder<R2Bucket::Etag>(1);

  if (etagValue == "*") {
    struct R2Bucket::WildcardEtag etag = {};
    etagArrayBuilder.add(kj::mv(etag));
  } else {
    struct R2Bucket::StrongEtag etag = {.value = kj::str(etagValue)};
    etagArrayBuilder.add(kj::mv(etag));
  }

  return etagArrayBuilder.finish();
}

}  // namespace

R2Bucket::UnwrappedConditional::UnwrappedConditional(jsg::Lock& js, Headers& h)
    : secondsGranularity(true) {
  KJ_IF_SOME(e, h.getNoChecks(js, "if-match"_kj)) {
    etagMatches = parseConditionalEtagHeader(kj::str(e));
  }
  KJ_IF_SOME(e, h.getNoChecks(js, "if-none-match"_kj)) {
    etagDoesNotMatch = parseConditionalEtagHeader(kj::str(e));
  }
  KJ_IF_SOME(d, h.getNoChecks(js, "if-modified-since"_kj)) {
    auto date = parseDate(js, d);
    uploadedAfter = date;
  }
  KJ_IF_SOME(d, h.getNoChecks(js, "if-unmodified-since"_kj)) {
    auto date = parseDate(js, d);
    uploadedBefore = date;
  }
}

R2Bucket::UnwrappedConditional::UnwrappedConditional(const Conditional& c)
    : secondsGranularity(c.secondsGranularity.orDefault(false)) {
  KJ_IF_SOME(e, c.etagMatches) {
    JSG_REQUIRE(!isQuotedEtag(e.value), TypeError,
        "Conditional ETag should not be wrapped in quotes (", e.value, ").");
    etagMatches = buildSingleEtagArray(e.value);
  }
  KJ_IF_SOME(e, c.etagDoesNotMatch) {
    JSG_REQUIRE(!isQuotedEtag(e.value), TypeError,
        "Conditional ETag should not be wrapped in quotes (", e.value, ").");
    etagDoesNotMatch = buildSingleEtagArray(e.value);
  }
  KJ_IF_SOME(d, c.uploadedAfter) {
    uploadedAfter = d;
  }
  KJ_IF_SOME(d, c.uploadedBefore) {
    uploadedBefore = d;
  }
}

R2Bucket::HttpMetadata R2Bucket::HttpMetadata::fromRequestHeaders(jsg::Lock& js, Headers& h) {
  HttpMetadata result;
  KJ_IF_SOME(ct, h.getNoChecks(js, "content-type")) {
    result.contentType = kj::mv(ct);
  }
  KJ_IF_SOME(ce, h.getNoChecks(js, "content-encoding"_kj)) {
    result.contentEncoding = kj::mv(ce);
  }
  KJ_IF_SOME(cd, h.getNoChecks(js, "content-disposition"_kj)) {
    result.contentDisposition = kj::mv(cd);
  }
  KJ_IF_SOME(cl, h.getNoChecks(js, "content-language"_kj)) {
    result.contentLanguage = kj::mv(cl);
  }
  KJ_IF_SOME(cc, h.getNoChecks(js, "cache-control"_kj)) {
    result.cacheControl = kj::mv(cc);
  }
  KJ_IF_SOME(ceStr, h.getNoChecks(js, "expires"_kj)) {
    result.cacheExpiry = parseDate(js, ceStr);
  }

  return result;
}

R2Bucket::HttpMetadata R2Bucket::HttpMetadata::clone() const {
  auto cloneStr = [](const kj::String& str) { return kj::str(str); };
  return {
    .contentType = contentType.map(cloneStr),
    .contentLanguage = contentLanguage.map(cloneStr),
    .contentDisposition = contentDisposition.map(cloneStr),
    .contentEncoding = contentEncoding.map(cloneStr),
    .cacheControl = cacheControl.map(cloneStr),
    .cacheExpiry = cacheExpiry,
  };
}

void R2Bucket::HeadResult::writeHttpMetadata(jsg::Lock& js, Headers& headers) {
  JSG_REQUIRE(httpMetadata != kj::none, TypeError, "HTTP metadata unknown for key `", name,
      "`. Did you forget to add 'httpMetadata' to `include` when listing?");
  const auto& m = KJ_REQUIRE_NONNULL(httpMetadata);

  KJ_IF_SOME(ct, m.contentType) {
    headers.set(js, js.accountedByteString("content-type"_kj), js.accountedByteString(ct));
  }
  KJ_IF_SOME(cl, m.contentLanguage) {
    headers.set(js, js.accountedByteString("content-language"_kj), js.accountedByteString(cl));
  }
  KJ_IF_SOME(cd, m.contentDisposition) {
    headers.set(js, js.accountedByteString("content-disposition"_kj), js.accountedByteString(cd));
  }
  KJ_IF_SOME(ce, m.contentEncoding) {
    headers.set(js, js.accountedByteString("content-encoding"_kj), js.accountedByteString(ce));
  }
  KJ_IF_SOME(cc, m.cacheControl) {
    headers.set(js, js.accountedByteString("cache-control"_kj), js.accountedByteString(cc));
  }
  KJ_IF_SOME(ce, m.cacheExpiry) {
    headers.set(js, js.accountedByteString("expires"_kj), toUTCString(js, ce));
  }
}

jsg::Promise<jsg::BufferSource> R2Bucket::GetResult::arrayBuffer(jsg::Lock& js) {
  return js.evalNow([&] {
    JSG_REQUIRE(!body->isDisturbed(), TypeError,
        "Body has already been used. "
        "It can only be used once. Use tee() first if you need to read it twice.");

    auto& context = IoContext::current();
    return body->getController().readAllBytes(js, context.getLimitEnforcer().getBufferingLimit());
  });
}

jsg::Promise<jsg::BufferSource> R2Bucket::GetResult::bytes(jsg::Lock& js) {
  return js.evalNow([&] {
    JSG_REQUIRE(!body->isDisturbed(), TypeError,
        "Body has already been used. "
        "It can only be used once. Use tee() first if you need to read it twice.");

    auto& context = IoContext::current();
    return body->getController()
        .readAllBytes(js, context.getLimitEnforcer().getBufferingLimit())
        .then(js, [](jsg::Lock& js, jsg::BufferSource data) {
      return data.getTypedView<v8::Uint8Array>(js);
    });
  });
}

jsg::Promise<kj::String> R2Bucket::GetResult::text(jsg::Lock& js) {
  // Copy-pasted from http.c++
  return js.evalNow([&] {
    JSG_REQUIRE(!body->isDisturbed(), TypeError,
        "Body has already been used. "
        "It can only be used once. Use tee() first if you need to read it twice.");

    auto& context = IoContext::current();
    // A common mistake is to call .text() on non-text content, e.g. because you're implementing a
    // search-and-replace across your whole site and you forgot that it'll apply to images too.
    // When running in the fiddle, let's warn the developer if they do this.
    if (context.isInspectorEnabled()) {
      // httpMetadata can't be null because GetResult always populates it.
      KJ_IF_SOME(type, KJ_REQUIRE_NONNULL(httpMetadata).contentType) {
        maybeWarnIfNotText(js, type);
      }
    }

    return body->getController().readAllText(js, context.getLimitEnforcer().getBufferingLimit());
  });
}

jsg::Promise<jsg::Value> R2Bucket::GetResult::json(jsg::Lock& js) {
  // Copy-pasted from http.c++
  return text(js).then(js, [](jsg::Lock& js, kj::String text) { return js.parseJson(text); });
}

jsg::Promise<jsg::Ref<Blob>> R2Bucket::GetResult::blob(jsg::Lock& js) {
  // Copy-pasted from http.c++
  return arrayBuffer(js).then(js, [this](jsg::Lock& js, jsg::BufferSource buffer) {
    // httpMetadata can't be null because GetResult always populates it.
    kj::String contentType = KJ_REQUIRE_NONNULL(httpMetadata)
                                 .contentType.map([](const auto& str) {
      return kj::str(str);
    }).orDefault(nullptr);
    return js.alloc<Blob>(js, kj::mv(buffer), kj::mv(contentType));
  });
}

R2Bucket::StringChecksums R2Bucket::Checksums::toJSON() {
  return {
    .md5 = this->md5.map(kj::encodeHex),
    .sha1 = this->sha1.map(kj::encodeHex),
    .sha256 = this->sha256.map(kj::encodeHex),
    .sha384 = this->sha384.map(kj::encodeHex),
    .sha512 = this->sha512.map(kj::encodeHex),
  };
}

namespace {
jsg::Optional<jsg::BufferSource> copyHash(
    jsg::Lock& js, const jsg::Optional<kj::Array<kj::byte>>& maybeHash) {
  KJ_IF_SOME(hash, maybeHash) {
    auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, hash.size());
    backing.asArrayPtr().copyFrom(hash);
    return jsg::BufferSource(js, kj::mv(backing));
  }
  return kj::none;
}
}  // namespace

jsg::Optional<jsg::BufferSource> R2Bucket::Checksums::getMd5(jsg::Lock& js) {
  return copyHash(js, md5);
}
jsg::Optional<jsg::BufferSource> R2Bucket::Checksums::getSha1(jsg::Lock& js) {
  return copyHash(js, sha1);
}
jsg::Optional<jsg::BufferSource> R2Bucket::Checksums::getSha256(jsg::Lock& js) {
  return copyHash(js, sha256);
}
jsg::Optional<jsg::BufferSource> R2Bucket::Checksums::getSha384(jsg::Lock& js) {
  return copyHash(js, sha384);
}
jsg::Optional<jsg::BufferSource> R2Bucket::Checksums::getSha512(jsg::Lock& js) {
  return copyHash(js, sha512);
}

kj::Maybe<jsg::Ref<R2Bucket::HeadResult>> parseHeadResultWrapper(jsg::Lock& js,
    kj::StringPtr action,
    R2Result& r2Result,
    const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType) {
  return parseObjectMetadata<R2Bucket::HeadResult>(js, action, r2Result, errorType);
}

kj::ArrayPtr<kj::StringPtr> fillR2Path(
    kj::StringPtr pathStorage[1], const kj::Maybe<kj::String>& bucket) {
  int numComponents = 0;

  KJ_IF_SOME(b, bucket) {
    pathStorage[numComponents++] = b;
  }

  return kj::arrayPtr(pathStorage, numComponents);
}

}  // namespace workerd::api::public_beta
