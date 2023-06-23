// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "r2-multipart.h"
#include "r2-bucket.h"
#include "r2-rpc.h"
#include <array>
#include <math.h>
#include <workerd/api/http.h>
#include <workerd/api/streams.h>
#include <kj/encoding.h>
#include <kj/compat/http.h>
#include <capnp/compat/json.h>
#include <workerd/util/http-util.h>
#include <workerd/api/r2-api.capnp.h>

namespace workerd::api::public_beta {
static bool isWholeNumber(double x) {
  double intpart;
  return modf(x, &intpart) == 0;
}

// TODO(perf): Would be nice to expose the v8 internals for parsing a date/stringifying it as
// something an embedder can call directly rather than doing this rigamarole. It would also avoid
// concerns about the user overriding the methods we're invoking.
static kj::Date parseDate(jsg::Lock& js, kj::StringPtr value) {
  auto isolate = js.v8Isolate;
  const auto context = js.v8Context();
  const auto tmp = jsg::check(v8::Date::New(context, 0));
  KJ_REQUIRE(tmp->IsDate());
  const auto constructor = jsg::check(tmp.template As<v8::Date>()->Get(
      context, jsg::v8StrIntern(isolate, "constructor")));
  JSG_REQUIRE(constructor->IsFunction(), TypeError, "Date.constructor is not a function");
  v8::Local<v8::Value> argv = jsg::v8Str(isolate, value);
  const auto converted = jsg::check(
      constructor.template As<v8::Function>()->NewInstance(context, 1, &argv));
  JSG_REQUIRE(converted->IsDate(), TypeError, "Date.constructor did not return a Date");
  return kj::UNIX_EPOCH +
      (int64_t(converted.template As<v8::Date>()->ValueOf()) * kj::MILLISECONDS);
}

static jsg::ByteString toUTCString(jsg::Lock& js, kj::Date date) {
  // NOTE: If you need toISOString just unify it into this function as the only difference will be
  // the function name called.
  auto isolate = js.v8Isolate;
  const auto context = js.v8Context();
  const auto converted = jsg::check(v8::Date::New(
      context, (date - kj::UNIX_EPOCH) / kj::MILLISECONDS));
  KJ_REQUIRE(converted->IsDate());
  const auto stringify = jsg::check(converted.template As<v8::Date>()->Get(
      context, jsg::v8StrIntern(isolate, "toUTCString")));
  JSG_REQUIRE(stringify->IsFunction(), TypeError, "toUTCString on a Date is not a function");
  const auto stringified = jsg::check(stringify.template As<v8::Function>()->Call(
      context, stringify, 0, nullptr));
  JSG_REQUIRE(stringified->IsString(), TypeError, "toUTCString on a Date did not return a string");

  const auto str = stringified.template As<v8::String>();
  auto buf = kj::heapArray<char>(str->Utf8Length(isolate) + 1);
  str->WriteUtf8(isolate, buf.begin(), buf.size());
  buf[buf.size() - 1] = 0;
  return jsg::ByteString(kj::String(kj::mv(buf)));
}

enum class OptionalMetadata: uint16_t {
  Http = static_cast<uint8_t>(R2ListRequest::IncludeField::HTTP),
  Custom = static_cast<uint8_t>(R2ListRequest::IncludeField::CUSTOM),
};

template <typename T>
concept HeadResultT = std::is_base_of_v<R2Bucket::HeadResult, T>;

template <HeadResultT T, typename... Args>
static jsg::Ref<T> parseObjectMetadata(R2HeadResponse::Reader responseReader,
    kj::ArrayPtr<const OptionalMetadata> expectedOptionalFields, Args&&... args) {
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
      m.cacheExpiry =
          kj::UNIX_EPOCH + httpFields.getCacheExpiry() * kj::MILLISECONDS;
    }

    httpMetadata = kj::mv(m);
  } else if (std::find(expectedOptionalFields.begin(), expectedOptionalFields.end(),
      OptionalMetadata::Http) != expectedOptionalFields.end()) {
    // HTTP metadata was asked for but the object didn't have anything.
    httpMetadata = R2Bucket::HttpMetadata{};
  }

  jsg::Optional<jsg::Dict<kj::String>> customMetadata;
  if (responseReader.hasCustomFields()) {
    customMetadata = jsg::Dict<kj::String> {
      .fields = KJ_MAP(field, responseReader.getCustomFields()) {
        jsg::Dict<kj::String>::Field item;
        item.name = kj::str(field.getK());
        item.value = kj::str(field.getV());
        return item;
      }
    };
  } else if (std::find(expectedOptionalFields.begin(), expectedOptionalFields.end(),
      OptionalMetadata::Custom) != expectedOptionalFields.end()) {
    // Custom metadata was asked for but the object didn't have anything.
    customMetadata = jsg::Dict<kj::String>{};
  }

  jsg::Optional<R2Bucket::Range> range;

  if (responseReader.hasRange()) {
    auto rangeBuilder = responseReader.getRange();
    range = R2Bucket::Range {
      .offset = static_cast<double>(rangeBuilder.getOffset()),
      .length = static_cast<double>(rangeBuilder.getLength()),
    };
  }

  jsg::Ref<R2Bucket::Checksums> checksums = jsg::alloc<R2Bucket::Checksums>(nullptr, nullptr, nullptr, nullptr, nullptr);

  if (responseReader.hasChecksums()) {
    R2Checksums::Reader checksumsBuilder = responseReader.getChecksums();
    if (checksumsBuilder.hasMd5()) {
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

  return jsg::alloc<T>(kj::str(responseReader.getName()),
      kj::str(responseReader.getVersion()), responseReader.getSize(),
      kj::str(responseReader.getEtag()), kj::mv(checksums), uploaded, kj::mv(httpMetadata),
      kj::mv(customMetadata), range, kj::fwd<Args>(args)...);
}

template <HeadResultT T, typename... Args>
static kj::Maybe<jsg::Ref<T>> parseObjectMetadata(kj::StringPtr action, R2Result& r2Result,
    const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType, Args&&... args) {
  if (r2Result.objectNotFound()) {
    return nullptr;
  }
  if (!r2Result.preconditionFailed()) {
    r2Result.throwIfError(action, errorType);
  }

  // Non-list operations always return these.
  std::array expectedFieldsOwned = { OptionalMetadata::Http, OptionalMetadata::Custom };
  kj::ArrayPtr<OptionalMetadata> expectedFields = {
    expectedFieldsOwned.data(), expectedFieldsOwned.size()
  };

  capnp::MallocMessageBuilder responseMessage;
  capnp::JsonCodec json;
  // Annoyingly our R2GetResponse alias isn't emitted.
  json.handleByAnnotation<R2HeadResponse>();
  auto responseBuilder = responseMessage.initRoot<R2HeadResponse>();
  json.decode(KJ_ASSERT_NONNULL(r2Result.metadataPayload), responseBuilder);

  return parseObjectMetadata<T>(responseBuilder, expectedFields, kj::fwd<Args>(args)...);
}

namespace {

void addEtagsToBuilder(capnp::List<R2Etag>::Builder etagListBuilder, kj::ArrayPtr<R2Bucket::Etag> etagArray) {
  R2Bucket::Etag* currentEtag = etagArray.begin();
  for (unsigned int i = 0; i < etagArray.size(); i++) {
    KJ_SWITCH_ONEOF(*currentEtag) {
      KJ_CASE_ONEOF(e, R2Bucket::WildcardEtag) {
        etagListBuilder[i].initType().setWildcard();
      } KJ_CASE_ONEOF(e, R2Bucket::StrongEtag) {
        etagListBuilder[i].initType().setStrong();
        etagListBuilder[i].setValue(e.value);
      } KJ_CASE_ONEOF(e, R2Bucket::WeakEtag) {
        etagListBuilder[i].initType().setWeak();
        etagListBuilder[i].setValue(e.value);
      }
    }
    currentEtag = std::next(currentEtag);
  }
}

} // namespace

template <typename Builder, typename Options>
void initOnlyIf(jsg::Lock& js, Builder& builder, Options& o) {
  KJ_IF_MAYBE(i, o.onlyIf) {
    R2Bucket::UnwrappedConditional c = [&]{
      KJ_SWITCH_ONEOF(*i) {
        KJ_CASE_ONEOF(conditional, R2Bucket::Conditional) {
          return R2Bucket::UnwrappedConditional(conditional);
        }
        KJ_CASE_ONEOF(h, jsg::Ref<Headers>) {
          return R2Bucket::UnwrappedConditional(js, *h);
        }
      }
      KJ_UNREACHABLE;
    }();

    R2Conditional::Builder onlyIfBuilder = builder.initOnlyIf();
    KJ_IF_MAYBE(etagArray, c.etagMatches) {
      capnp::List<R2Etag>::Builder etagMatchList = onlyIfBuilder.initEtagMatches(etagArray->size());
      addEtagsToBuilder(
        etagMatchList,
        kj::arrayPtr<R2Bucket::Etag>(etagArray->begin(), etagArray->size())
      );
    }
    KJ_IF_MAYBE(etagArray, c.etagDoesNotMatch) {
      auto etagDoesNotMatchList = onlyIfBuilder.initEtagDoesNotMatch(etagArray->size());
      addEtagsToBuilder(
        etagDoesNotMatchList,
        kj::arrayPtr<R2Bucket::Etag>(etagArray->begin(), etagArray->size())
      );
    }
    KJ_IF_MAYBE(d, c.uploadedBefore) {
      onlyIfBuilder.setUploadedBefore(
          (*d - kj::UNIX_EPOCH) / kj::MILLISECONDS
      );
      if (c.secondsGranularity) {
        onlyIfBuilder.setSecondsGranularity(true);
      }
    }
    KJ_IF_MAYBE(d, c.uploadedAfter) {
      onlyIfBuilder.setUploadedAfter(
          (*d - kj::UNIX_EPOCH) / kj::MILLISECONDS
      );
      if (c.secondsGranularity) {
        onlyIfBuilder.setSecondsGranularity(true);
      }
    }
  }
}

template <typename Builder, typename Options>
void initGetOptions(jsg::Lock& js, Builder& builder, Options& o) {
  initOnlyIf(js, builder, o);
  KJ_IF_MAYBE(range, o.range) {
    KJ_SWITCH_ONEOF(*range) {
      KJ_CASE_ONEOF(r, R2Bucket::Range) {
        auto rangeBuilder = builder.initRange();
        KJ_IF_MAYBE(offset, r.offset) {
          JSG_REQUIRE(*offset >= 0, RangeError,
              "Invalid range. Starting offset (", offset, ") must be greater than or equal to 0.");
          JSG_REQUIRE(isWholeNumber(*offset), RangeError,
              "Invalid range. Starting offset (", offset, ") must be an integer, not floating point.");
          rangeBuilder.setOffset(static_cast<uint64_t>(*offset));
        }

        KJ_IF_MAYBE(length, r.length) {
          JSG_REQUIRE(*length >= 0, RangeError,
            "Invalid range. Length (", *length, ") must be greater than or equal to 0.");
          JSG_REQUIRE(isWholeNumber(*length), RangeError,
            "Invalid range. Length (", *length, ") must be an integer, not floating point.");

          rangeBuilder.setLength(static_cast<uint64_t>(*length));
        }
        KJ_IF_MAYBE(suffix, r.suffix) {
          JSG_REQUIRE(r.offset == nullptr, TypeError, "Suffix is incompatible with offset.");
          JSG_REQUIRE(r.length == nullptr, TypeError, "Suffix is incompatible with length.");

          JSG_REQUIRE(*suffix >= 0, RangeError,
            "Invalid suffix. Suffix (", *suffix, ") must be greater than or equal to 0.");
          JSG_REQUIRE(isWholeNumber(*suffix), RangeError,
            "Invalid range. Suffix (", *suffix, ") must be an integer, not floating point.");

          rangeBuilder.setSuffix(static_cast<uint64_t>(*suffix));
        }
      }

      KJ_CASE_ONEOF(h, jsg::Ref<Headers>) {
        KJ_IF_MAYBE(e, h->get(jsg::ByteString(kj::str("range")))) {
          builder.setRangeHeader(kj::str(*e));
        }
      }
    }
  }
}

static bool isQuotedEtag(kj::StringPtr etag) {
  return etag.startsWith("\"") && etag.endsWith("\"");
}

jsg::Promise<kj::Maybe<jsg::Ref<R2Bucket::HeadResult>>> R2Bucket::head(
    jsg::Lock& js, kj::String name, const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType,
    CompatibilityFlags::Reader flags) {
  return js.evalNow([&] {
    auto& context = IoContext::current();

    auto client = context.getHttpClient(clientIndex, true, nullptr, "r2_get"_kjc);

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
    auto promise = doR2HTTPGetRequest(kj::mv(client), kj::mv(requestJson), path, jwt,
        flags);

    return context.awaitIo(kj::mv(promise), [&errorType](R2Result r2Result) {
      return parseObjectMetadata<HeadResult>("head", r2Result, errorType);
    });
  });
}

R2Bucket::FeatureFlags::FeatureFlags(CompatibilityFlags::Reader featureFlags)
    : listHonorsIncludes(featureFlags.getR2ListHonorIncludeFields()) {}

jsg::Promise<kj::OneOf<kj::Maybe<jsg::Ref<R2Bucket::GetResult>>, jsg::Ref<R2Bucket::HeadResult>>>
R2Bucket::get(jsg::Lock& js, kj::String name, jsg::Optional<GetOptions> options,
    const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType, CompatibilityFlags::Reader flags) {
  return js.evalNow([&] {
    auto& context = IoContext::current();

    auto client = context.getHttpClient(clientIndex, true, nullptr, "r2_get"_kjc);

    capnp::JsonCodec json;
    json.handleByAnnotation<R2BindingRequest>();
    json.setHasMode(capnp::HasMode::NON_DEFAULT);
    capnp::MallocMessageBuilder requestMessage;

    auto requestBuilder = requestMessage.initRoot<R2BindingRequest>();
    requestBuilder.setVersion(VERSION_PUBLIC_BETA);
    auto payloadBuilder = requestBuilder.initPayload();
    auto getBuilder = payloadBuilder.initGet();
    getBuilder.setObject(name);

    KJ_IF_MAYBE(o, options) {
      initGetOptions(js, getBuilder, *o);
    }
    auto requestJson = json.encode(requestBuilder);
    kj::StringPtr components[1];
    auto path = fillR2Path(components, adminBucket);
    auto promise = doR2HTTPGetRequest(kj::mv(client), kj::mv(requestJson), path, jwt,
        flags);

    return context.awaitIo(kj::mv(promise), [&context, &errorType](R2Result r2Result)
        -> kj::OneOf<kj::Maybe<jsg::Ref<GetResult>>, jsg::Ref<HeadResult>> {
      kj::OneOf<kj::Maybe<jsg::Ref<GetResult>>, jsg::Ref<HeadResult>> result;

      if (r2Result.preconditionFailed()) {
        result = KJ_ASSERT_NONNULL(parseObjectMetadata<HeadResult>("get", r2Result, errorType));
      } else {
        jsg::Ref<ReadableStream> body = nullptr;

        KJ_IF_MAYBE (s, r2Result.stream) {
          body = jsg::alloc<ReadableStream>(context, kj::mv(*s));
          r2Result.stream = nullptr;
        }
        result = parseObjectMetadata<GetResult>("get", r2Result, errorType, kj::mv(body));
      }
      return result;
    });
  });
}

jsg::Promise<kj::Maybe<jsg::Ref<R2Bucket::HeadResult>>>
R2Bucket::put(jsg::Lock& js, kj::String name, kj::Maybe<R2PutValue> value,
    jsg::Optional<PutOptions> options, const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType) {
  return js.evalNow([&] {
    auto cancelReader = kj::defer([&] {
      KJ_IF_MAYBE(v, value) {
        KJ_SWITCH_ONEOF(*v) {
          KJ_CASE_ONEOF(v, jsg::Ref<ReadableStream>) {
            (*v).cancel(js, js.v8Error(kj::str("Stream cancelled because the associated put operation encountered an error.")));
          }
          KJ_CASE_ONEOF_DEFAULT {}
        }
      }
    });

    auto& context = IoContext::current();
    auto client = context.getHttpClient(clientIndex, true, nullptr, "r2_put"_kjc);

    capnp::JsonCodec json;
    json.handleByAnnotation<R2BindingRequest>();
    json.setHasMode(capnp::HasMode::NON_DEFAULT);
    capnp::MallocMessageBuilder requestMessage;

    auto requestBuilder = requestMessage.initRoot<R2BindingRequest>();
    requestBuilder.setVersion(VERSION_PUBLIC_BETA);
    auto payloadBuilder = requestBuilder.initPayload();
    auto putBuilder = payloadBuilder.initPut();
    putBuilder.setObject(name);

    HttpMetadata sentHttpMetadata;
    jsg::Dict<kj::String> sentCustomMetadata;

    bool hashAlreadySpecified = false;
    const auto verifyHashNotSpecified = [&] {
      JSG_REQUIRE(!hashAlreadySpecified, TypeError, "You cannot specify multiple hashing algorithms.");
      hashAlreadySpecified = true;
    };

    KJ_IF_MAYBE(o, options) {
      initOnlyIf(js, putBuilder, *o);
      KJ_IF_MAYBE(m, o->customMetadata) {
        auto fields = putBuilder.initCustomFields(m->fields.size());
        for (size_t i = 0; i < m->fields.size(); i++) {
          fields[i].setK(m->fields[i].name);
          fields[i].setV(m->fields[i].value);
        }
        sentCustomMetadata = kj::mv(*m);
      }
      KJ_IF_MAYBE(m, o->httpMetadata) {
        auto fields = putBuilder.initHttpFields();
        sentHttpMetadata = [&]() {
          KJ_SWITCH_ONEOF(*m) {
            KJ_CASE_ONEOF(m, HttpMetadata) {
              return kj::mv(m);
            }
            KJ_CASE_ONEOF(h, jsg::Ref<Headers>) {
              return HttpMetadata::fromRequestHeaders(js, *h);
            }
          }
          KJ_UNREACHABLE;
        }();

        KJ_IF_MAYBE(ct, sentHttpMetadata.contentType) {
          fields.setContentType(*ct);
        }
        KJ_IF_MAYBE(ce, sentHttpMetadata.contentEncoding) {
          fields.setContentEncoding(*ce);
        }
        KJ_IF_MAYBE(cd, sentHttpMetadata.contentDisposition) {
          fields.setContentDisposition(*cd);
        }
        KJ_IF_MAYBE(cl, sentHttpMetadata.contentLanguage) {
          fields.setContentLanguage(*cl);
        }
        KJ_IF_MAYBE(cc, sentHttpMetadata.cacheControl) {
          fields.setCacheControl(*cc);
        }
        KJ_IF_MAYBE(ce, sentHttpMetadata.cacheExpiry) {
          fields.setCacheExpiry((*ce - kj::UNIX_EPOCH) / kj::MILLISECONDS);
        }
      }
      KJ_IF_MAYBE(md5, o->md5) {
        verifyHashNotSpecified();
        KJ_SWITCH_ONEOF(*md5) {
          KJ_CASE_ONEOF(bin, kj::Array<kj::byte>) {
            JSG_REQUIRE(bin.size() == 16, TypeError, "MD5 is 16 bytes, not ", bin.size());
            putBuilder.setMd5(bin);
          }
          KJ_CASE_ONEOF(hex, jsg::NonCoercible<kj::String>) {
            JSG_REQUIRE(hex.value.size() == 32, TypeError,
                "MD5 is 32 hex characters, not ", hex.value.size());
            const auto decoded = kj::decodeHex(hex.value);
            JSG_REQUIRE(!decoded.hadErrors, TypeError, "Provided MD5 wasn't a valid hex string");
            putBuilder.setMd5(decoded);
          }
        }
      }
      KJ_IF_MAYBE(sha1, o->sha1) {
        verifyHashNotSpecified();
        KJ_SWITCH_ONEOF(*sha1) {
          KJ_CASE_ONEOF(bin, kj::Array<kj::byte>) {
            JSG_REQUIRE(bin.size() == 20, TypeError, "SHA-1 is 20 bytes, not ", bin.size());
            putBuilder.setSha1(bin);
          }
          KJ_CASE_ONEOF(hex, jsg::NonCoercible<kj::String>) {
            JSG_REQUIRE(hex.value.size() == 40, TypeError,
                "SHA-1 is 40 hex characters, not ", hex.value.size());
            const auto decoded = kj::decodeHex(hex.value);
            JSG_REQUIRE(!decoded.hadErrors, TypeError, "Provided SHA-1 wasn't a valid hex string");
            putBuilder.setSha1(decoded);
          }
        }
      }
      KJ_IF_MAYBE(sha256, o->sha256) {
        verifyHashNotSpecified();
        KJ_SWITCH_ONEOF(*sha256) {
          KJ_CASE_ONEOF(bin, kj::Array<kj::byte>) {
            JSG_REQUIRE(bin.size() == 32, TypeError, "SHA-256 is 32 bytes, not ", bin.size());
            putBuilder.setSha256(bin);
          }
          KJ_CASE_ONEOF(hex, jsg::NonCoercible<kj::String>) {
            JSG_REQUIRE(hex.value.size() == 64, TypeError,
                "SHA-256 is 64 hex characters, not ", hex.value.size());
            const auto decoded = kj::decodeHex(hex.value);
            JSG_REQUIRE(!decoded.hadErrors, TypeError, "Provided SHA-256 wasn't a valid hex string");
            putBuilder.setSha256(decoded);
          }
        }
      }
      KJ_IF_MAYBE(sha384, o->sha384) {
        verifyHashNotSpecified();
        KJ_SWITCH_ONEOF(*sha384) {
          KJ_CASE_ONEOF(bin, kj::Array<kj::byte>) {
            JSG_REQUIRE(bin.size() == 48, TypeError, "SHA-384 is 48 bytes, not ", bin.size());
            putBuilder.setSha384(bin);
          }
          KJ_CASE_ONEOF(hex, jsg::NonCoercible<kj::String>) {
            JSG_REQUIRE(hex.value.size() == 96, TypeError,
                "SHA-384 is 96 hex characters, not ", hex.value.size());
            const auto decoded = kj::decodeHex(hex.value);
            JSG_REQUIRE(!decoded.hadErrors, TypeError, "Provided SHA-384 wasn't a valid hex string");
            putBuilder.setSha384(decoded);
          }
        }
      }
      KJ_IF_MAYBE(sha512, o->sha512) {
        verifyHashNotSpecified();
        KJ_SWITCH_ONEOF(*sha512) {
          KJ_CASE_ONEOF(bin, kj::Array<kj::byte>) {
            JSG_REQUIRE(bin.size() == 64, TypeError, "SHA-512 is 64 bytes, not ", bin.size());
            putBuilder.setSha512(bin);
          }
          KJ_CASE_ONEOF(hex, jsg::NonCoercible<kj::String>) {
            JSG_REQUIRE(hex.value.size() == 128, TypeError,
                "SHA-512 is 128 hex characters, not ", hex.value.size());
            const auto decoded = kj::decodeHex(hex.value);
            JSG_REQUIRE(!decoded.hadErrors, TypeError, "Provided SHA-512 wasn't a valid hex string");
            putBuilder.setSha512(decoded);
          }
        }
      }
    }

    auto requestJson = json.encode(requestBuilder);

    cancelReader.cancel();
    kj::StringPtr components[1];
    auto path = fillR2Path(components, adminBucket);
    auto promise = doR2HTTPPutRequest(js, kj::mv(client), kj::mv(value), nullptr,
                                      kj::mv(requestJson), path, jwt);

    return context.awaitIo(js, kj::mv(promise),
        [sentHttpMetadata = kj::mv(sentHttpMetadata),
         sentCustomMetadata = kj::mv(sentCustomMetadata),
         &errorType]
        (jsg::Lock& js, R2Result r2Result) mutable -> kj::Maybe<jsg::Ref<HeadResult>> {
      if (r2Result.preconditionFailed()) {
        return nullptr;
      } else {
        auto result = parseObjectMetadata<HeadResult>("put", r2Result, errorType);
        KJ_IF_MAYBE(o, result) {
          o->get()->httpMetadata = kj::mv(sentHttpMetadata);
          o->get()->customMetadata = kj::mv(sentCustomMetadata);
        }
        return result;
      }
    });
  });
}

jsg::Promise<jsg::Ref<R2MultipartUpload>> R2Bucket::createMultipartUpload(jsg::Lock& js, kj::String key,
    jsg::Optional<MultipartOptions> options, const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType) {
  return js.evalNow([&] {
    auto& context = IoContext::current();
    auto client = context.getHttpClient(
      clientIndex, true, nullptr, "r2_createMultipartUpload"_kjc);

    capnp::JsonCodec json;
    json.handleByAnnotation<R2BindingRequest>();
    json.setHasMode(capnp::HasMode::NON_DEFAULT);
    capnp::MallocMessageBuilder requestMessage;

    auto requestBuilder = requestMessage.initRoot<R2BindingRequest>();
    requestBuilder.setVersion(VERSION_PUBLIC_BETA);
    auto payloadBuilder = requestBuilder.initPayload();
    auto createMultipartUploadBuilder = payloadBuilder.initCreateMultipartUpload();
    createMultipartUploadBuilder.setObject(key);

    KJ_IF_MAYBE(o, options) {
      KJ_IF_MAYBE(m, o->customMetadata) {
        auto fields = createMultipartUploadBuilder.initCustomFields(m->fields.size());
        for (size_t i = 0; i < m->fields.size(); i++) {
          fields[i].setK(m->fields[i].name);
          fields[i].setV(m->fields[i].value);
        }
      }
      KJ_IF_MAYBE(m, o->httpMetadata) {
        auto fields = createMultipartUploadBuilder.initHttpFields();
        HttpMetadata httpMetadata = [&]() {
          KJ_SWITCH_ONEOF(*m) {
            KJ_CASE_ONEOF(m, HttpMetadata) {
              return kj::mv(m);
            }
            KJ_CASE_ONEOF(h, jsg::Ref<Headers>) {
              return HttpMetadata::fromRequestHeaders(js, *h);
            }
          }
          KJ_UNREACHABLE;
        }();

        KJ_IF_MAYBE(ct, httpMetadata.contentType) {
          fields.setContentType(*ct);
        }
        KJ_IF_MAYBE(ce, httpMetadata.contentEncoding) {
          fields.setContentEncoding(*ce);
        }
        KJ_IF_MAYBE(cd, httpMetadata.contentDisposition) {
          fields.setContentDisposition(*cd);
        }
        KJ_IF_MAYBE(cl, httpMetadata.contentLanguage) {
          fields.setContentLanguage(*cl);
        }
        KJ_IF_MAYBE(cc, httpMetadata.cacheControl) {
          fields.setCacheControl(*cc);
        }
        KJ_IF_MAYBE(ce, httpMetadata.cacheExpiry) {
          fields.setCacheExpiry((*ce - kj::UNIX_EPOCH) / kj::MILLISECONDS);
        }
      }
    }

    auto requestJson = json.encode(requestBuilder);
    kj::StringPtr components[1];
    auto path = fillR2Path(components, adminBucket);
    auto promise = doR2HTTPPutRequest(js, kj::mv(client), nullptr, nullptr, kj::mv(requestJson),
                                      path, jwt);

    return context.awaitIo(js, kj::mv(promise),
        [&errorType, key=kj::mv(key), this] (jsg::Lock& js, R2Result r2Result) mutable {
      r2Result.throwIfError("createMultipartUpload", errorType);

      capnp::MallocMessageBuilder responseMessage;
      capnp::JsonCodec json;
      json.handleByAnnotation<R2CreateMultipartUploadResponse>();
      auto responseBuilder = responseMessage.initRoot<R2CreateMultipartUploadResponse>();

      json.decode(KJ_ASSERT_NONNULL(r2Result.metadataPayload), responseBuilder);
      kj::String uploadId = kj::str(responseBuilder.getUploadId());
      return jsg::alloc<R2MultipartUpload>(kj::mv(key), kj::mv(uploadId), JSG_THIS);
    });
  });
}

jsg::Ref<R2MultipartUpload> R2Bucket::resumeMultipartUpload(jsg::Lock& js,
      kj::String key, kj::String uploadId, const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType) {
    return jsg::alloc<R2MultipartUpload>(kj::mv(key), kj::mv(uploadId), JSG_THIS);
}

jsg::Promise<void> R2Bucket::delete_(jsg::Lock& js, kj::OneOf<kj::String, kj::Array<kj::String>> keys,
    const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType) {
  return js.evalNow([&] {
    auto& context = IoContext::current();
    auto client = context.getHttpClient(clientIndex, true, nullptr, "r2_delete"_kjc);

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
    auto promise = doR2HTTPPutRequest(js, kj::mv(client), nullptr, nullptr, kj::mv(requestJson),
                                      path, jwt);

    return context.awaitIo(js, kj::mv(promise), [&errorType](jsg::Lock& js, R2Result r) {
      if (r.objectNotFound()) {
        return;
      }

      r.throwIfError("delete", errorType);
    });
  });
}

jsg::Promise<R2Bucket::ListResult> R2Bucket::list(
    jsg::Lock& js, jsg::Optional<ListOptions> options,
    const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType, CompatibilityFlags::Reader flags) {
  return js.evalNow([&] {
    auto& context = IoContext::current();
    auto client = context.getHttpClient(clientIndex, true, nullptr, "r2_list"_kjc);

    capnp::JsonCodec json;
    json.handleByAnnotation<R2BindingRequest>();
    json.setHasMode(capnp::HasMode::NON_DEFAULT);
    capnp::MallocMessageBuilder requestMessage;

    auto requestBuilder = requestMessage.initRoot<R2BindingRequest>();
    requestBuilder.setVersion(VERSION_PUBLIC_BETA);
    auto listBuilder = requestBuilder.initPayload().initList();

    kj::Vector<OptionalMetadata> expectedOptionalFields(2);

    KJ_IF_MAYBE(o, options) {
      KJ_IF_MAYBE(l, o->limit) {
        listBuilder.setLimit(*l);
      }
      KJ_IF_MAYBE(p, o->prefix) {
        listBuilder.setPrefix(p->value);
      }
      KJ_IF_MAYBE(c, o->cursor) {
        listBuilder.setCursor(c->value);
      }
      KJ_IF_MAYBE(d, o->delimiter) {
        listBuilder.setDelimiter(d->value);
      }
      KJ_IF_MAYBE(d, o->startAfter) {
        listBuilder.setStartAfter(d->value);
      }
      KJ_IF_MAYBE(i, o->include) {
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

        listBuilder.setInclude(KJ_MAP(reqField, *i) {
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

    // TODO(soon): Remove this after the release for 2022-07-04 is cut (from here & R2 worker).
    // This just tells the R2 worker that it can honor the `includes` field without breaking back
    // compat. If we just started spontaneously honoring the `includes` field then existing Workers
    // might suddenly lose http metadata because they weren't explicitly asking for it even though
    listBuilder.setNewRuntime(true);

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
    auto promise = doR2HTTPGetRequest(kj::mv(client), kj::mv(requestJson), path, jwt,
        flags);

    return context.awaitIo(kj::mv(promise),
        [expectedOptionalFields = expectedOptionalFields.releaseAsArray(), &errorType]
        (R2Result r2Result) {
      r2Result.throwIfError("list", errorType);

      R2Bucket::ListResult result;
      capnp::MallocMessageBuilder responseMessage;
      capnp::JsonCodec json;
      json.handleByAnnotation<R2ListResponse>();
      auto responseBuilder = responseMessage.initRoot<R2ListResponse>();

      json.decode(KJ_ASSERT_NONNULL(r2Result.metadataPayload), responseBuilder);

      result.objects = KJ_MAP(o, responseBuilder.getObjects()) {
        return parseObjectMetadata<HeadResult>(o, expectedOptionalFields);
      };
      result.truncated = responseBuilder.getTruncated();
      if (responseBuilder.hasCursor()) {
        result.cursor = kj::str(responseBuilder.getCursor());
      }
      if (responseBuilder.hasDelimitedPrefixes()) {
        result.delimitedPrefixes = KJ_MAP(e, responseBuilder.getDelimitedPrefixes()) {
          return kj::str(e);
        };
      }

      return kj::mv(result);
    });
  });
}

namespace {

kj::Array<R2Bucket::Etag> parseConditionalEtagHeader(
  kj::StringPtr condHeader,
  kj::Vector<R2Bucket::Etag> etagAccumulator = kj::Vector<R2Bucket::Etag>(),
  bool leadingCommaRequired = false
) {
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
  // Did not find a leading comma, and we expected a leading comma before any further etags
    KJ_FAIL_REQUIRE("Comma was expected to separate etags");
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
      condHeader.slice(etagValueStart)
      .findFirst('"')
      .map([=](size_t cq) { return cq + etagValueStart; });

    KJ_IF_MAYBE(cq, closingQuotation) {
      // Slice end is non inclusive, meaning that this drops the closingQuotation from the etag
      kj::String etagValue = kj::str(condHeader.slice(etagValueStart, *cq));
      if (nextWeak < nextQuotation) {
        KJ_REQUIRE(
          condHeader.size() > nextWeak + 2
            && condHeader[nextWeak + 1] == '/'
            && nextWeak + 2 == nextQuotation,
          "Weak etags must start with W/ and their value must be quoted"
        );
        R2Bucket::WeakEtag etag = {kj::mv(etagValue)};
        etagAccumulator.add(kj::mv(etag));
      } else {
        R2Bucket::StrongEtag etag = {kj::mv(etagValue)};
        etagAccumulator.add(kj::mv(etag));
      }
      return parseConditionalEtagHeader(
        condHeader.slice(*cq + 1),
        kj::mv(etagAccumulator),
        true
      );
    } else {
      KJ_FAIL_REQUIRE("Unclosed double quote for Etag");
    }
  } else {
    KJ_FAIL_REQUIRE("Invalid conditional header");
  }
}

kj::Array<R2Bucket::Etag> buildSingleStrongEtagArray(kj::StringPtr etagValue) {
  struct R2Bucket::StrongEtag etag = {.value = kj::str(etagValue)};
  kj::ArrayBuilder<R2Bucket::Etag> etagArrayBuilder = kj::heapArrayBuilder<R2Bucket::Etag>(1);
  etagArrayBuilder.add(kj::mv(etag));
  return etagArrayBuilder.finish();
}

} // namespace

R2Bucket::UnwrappedConditional::UnwrappedConditional(jsg::Lock& js, Headers& h)
    : secondsGranularity(true) {
  KJ_IF_MAYBE(e, h.get(jsg::ByteString(kj::str("if-match")))) {
    etagMatches = parseConditionalEtagHeader(kj::str(*e));
  }
  KJ_IF_MAYBE(e, h.get(jsg::ByteString(kj::str("if-none-match")))) {
    etagDoesNotMatch = parseConditionalEtagHeader(kj::str(*e));
  }
  KJ_IF_MAYBE(d, h.get(jsg::ByteString(kj::str("if-modified-since")))) {
    auto date = parseDate(js, *d);
    uploadedAfter = date;
  }
  KJ_IF_MAYBE(d, h.get(jsg::ByteString(kj::str("if-unmodified-since")))) {
    auto date = parseDate(js, *d);
    uploadedBefore = date;
  }
}

R2Bucket::UnwrappedConditional::UnwrappedConditional(const Conditional& c)
  : secondsGranularity(c.secondsGranularity.orDefault(false)) {
  KJ_IF_MAYBE(e, c.etagMatches) {
    JSG_REQUIRE(!isQuotedEtag(e->value), TypeError,
      "Conditional ETag should not be wrapped in quotes (", e->value, ").");
    etagMatches = buildSingleStrongEtagArray(e->value);
  }
  KJ_IF_MAYBE(e, c.etagDoesNotMatch) {
    JSG_REQUIRE(!isQuotedEtag(e->value), TypeError,
      "Conditional ETag should not be wrapped in quotes (", e->value, ").");
    etagDoesNotMatch = buildSingleStrongEtagArray(e->value);
  }
  KJ_IF_MAYBE(d, c.uploadedAfter) {
    uploadedAfter = *d;
  }
  KJ_IF_MAYBE(d, c.uploadedBefore) {
    uploadedBefore = *d;
  }
}

R2Bucket::HttpMetadata R2Bucket::HttpMetadata::fromRequestHeaders(jsg::Lock& js, Headers& h) {
  HttpMetadata result;
  KJ_IF_MAYBE(ct, h.get(jsg::ByteString(kj::str("content-type")))) {
    result.contentType = kj::mv(*ct);
  }
  KJ_IF_MAYBE(ce, h.get(jsg::ByteString(kj::str("content-encoding")))) {
    result.contentEncoding = kj::mv(*ce);
  }
  KJ_IF_MAYBE(cd, h.get(jsg::ByteString(kj::str("content-disposition")))) {
    result.contentDisposition = kj::mv(*cd);
  }
  KJ_IF_MAYBE(cl, h.get(jsg::ByteString(kj::str("content-language")))) {
    result.contentLanguage = kj::mv(*cl);
  }
  KJ_IF_MAYBE(cc, h.get(jsg::ByteString(kj::str("cache-control")))) {
    result.cacheControl = kj::mv(*cc);
  }
  KJ_IF_MAYBE(ceStr, h.get(jsg::ByteString(kj::str("expires")))) {
    result.cacheExpiry = parseDate(js, *ceStr);
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
  JSG_REQUIRE(httpMetadata != nullptr, TypeError,
      "HTTP metadata unknown for key `", name,
      "`. Did you forget to add 'httpMetadata' to `include` when listing?");
  const auto& m = KJ_REQUIRE_NONNULL(httpMetadata);

  KJ_IF_MAYBE(ct, m.contentType) {
    headers.set(jsg::ByteString(kj::str("content-type")), jsg::ByteString(kj::str(*ct)));
  }
  KJ_IF_MAYBE(cl, m.contentLanguage) {
    headers.set(jsg::ByteString(kj::str("content-language")), jsg::ByteString(kj::str(*cl)));
  }
  KJ_IF_MAYBE(cd, m.contentDisposition) {
    headers.set(jsg::ByteString(kj::str("content-disposition")), jsg::ByteString(kj::str(*cd)));
  }
  KJ_IF_MAYBE(ce, m.contentEncoding) {
    headers.set(jsg::ByteString(kj::str("content-encoding")), jsg::ByteString(kj::str(*ce)));
  }
  KJ_IF_MAYBE(cc, m.cacheControl) {
    headers.set(jsg::ByteString(kj::str("cache-control")), jsg::ByteString(kj::str(*cc)));
  }
  KJ_IF_MAYBE(ce, m.cacheExpiry) {
    headers.set(jsg::ByteString(kj::str("expires")), toUTCString(js, *ce));
  }
}

jsg::Promise<kj::Array<kj::byte>> R2Bucket::GetResult::arrayBuffer(jsg::Lock& js) {
  return js.evalNow([&] {
    JSG_REQUIRE(!body->isDisturbed(), TypeError, "Body has already been used. "
      "It can only be used once. Use tee() first if you need to read it twice.");

    auto& context = IoContext::current();
    return body->getController().readAllBytes(js, context.getLimitEnforcer().getBufferingLimit());
  });
}

jsg::Promise<kj::String> R2Bucket::GetResult::text(jsg::Lock& js) {
  // Copy-pasted from http.c++
  return js.evalNow([&] {
    JSG_REQUIRE(!body->isDisturbed(), TypeError, "Body has already been used. "
      "It can only be used once. Use tee() first if you need to read it twice.");

    // A common mistake is to call .text() on non-text content, e.g. because you're implementing a
    // search-and-replace across your whole site and you forgot that it'll apply to images too.
    // When running in the fiddle, let's warn the developer if they do this.
    auto& context = IoContext::current();
    if (context.isInspectorEnabled()) {
      // httpMetadata can't be null because GetResult always populates it.
      KJ_IF_MAYBE(type, KJ_REQUIRE_NONNULL(httpMetadata).contentType) {
        if (!type->startsWith("text/") &&
            !type->endsWith("charset=UTF-8") &&
            !type->endsWith("charset=utf-8") &&
            !type->endsWith("xml") &&
            !type->endsWith("json") &&
            !type->endsWith("javascript")) {
          context.logWarning(kj::str(
              "Called .text() on an HTTP body which does not appear to be text. The body's "
              "Content-Type is \"", *type, "\". The result will probably be corrupted. Consider "
              "checking the Content-Type header before interpreting entities as text."));
        }
      }
    }

    return body->getController().readAllText(js, context.getLimitEnforcer().getBufferingLimit());
  });
}

jsg::Promise<jsg::Value> R2Bucket::GetResult::json(jsg::Lock& js) {
  // Copy-pasted from http.c++
  return text(js).then(js, [](jsg::Lock& js, kj::String text) {
    return js.parseJson(text);
  });
}

jsg::Promise<jsg::Ref<Blob>> R2Bucket::GetResult::blob(jsg::Lock& js) {
  // Copy-pasted from http.c++
  return arrayBuffer(js).then(js, [this](jsg::Lock& js, kj::Array<byte> buffer) {
    // httpMetadata can't be null because GetResult always populates it.
    kj::String contentType = KJ_REQUIRE_NONNULL(httpMetadata).contentType
        .map([](const auto& str) { return kj::str(str); })
        .orDefault(nullptr);
    return jsg::alloc<Blob>(kj::mv(buffer), kj::mv(contentType));
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

kj::Array<kj::byte> cloneByteArray(const kj::Array<kj::byte> &arr) {
  return kj::heapArray(arr.asPtr());
}

kj::Maybe<jsg::Ref<R2Bucket::HeadResult>> parseHeadResultWrapper(
  kj::StringPtr action, R2Result& r2Result, const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType) {
    return parseObjectMetadata<R2Bucket::HeadResult>(action, r2Result, errorType);
}

kj::ArrayPtr<kj::StringPtr> fillR2Path(kj::StringPtr pathStorage[1], const kj::Maybe<kj::String>& bucket) {
  int numComponents = 0;

  KJ_IF_MAYBE(b, bucket) {
    pathStorage[numComponents++] = *b;
  }

  return kj::arrayPtr(pathStorage, numComponents);
}


} // namespace workerd::api::public_beta
