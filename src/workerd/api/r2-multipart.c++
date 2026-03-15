// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "r2-multipart.h"

#include "r2-bucket.h"
#include "r2-rpc.h"
#include "workerd/jsg/jsg.h"

#include <workerd/api/r2-api.capnp.h>
#include <workerd/util/http-util.h>

#include <capnp/compat/json.h>
#include <capnp/message.h>
#include <kj/compat/http.h>
#include <kj/encoding.h>

#include <regex>

namespace workerd::api::public_beta {

static void addR2ResponseSpanTags(TraceContext& traceContext, R2Result& r2Result) {
  traceContext.setTag("cloudflare.r2.response.success"_kjc, r2Result.success());
  KJ_IF_SOME(e, r2Result.getR2ErrorMessage()) {
    traceContext.setTag("error.type"_kjc, e.asPtr());
    traceContext.setTag("cloudflare.r2.error.message"_kjc, e.asPtr());
  }
  KJ_IF_SOME(v4, r2Result.v4ErrorCode()) {
    traceContext.setTag("cloudflare.r2.error.code"_kjc, static_cast<int64_t>(v4));
  }
}

jsg::Promise<R2MultipartUpload::UploadedPart> R2MultipartUpload::uploadPart(jsg::Lock& js,
    int partNumber,
    R2PutValue value,
    jsg::Optional<UploadPartOptions> options,
    const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType) {
  return js.evalNow([&] {
    JSG_REQUIRE(partNumber >= 1 && partNumber <= 10000, TypeError,
        "Part number must be between 1 and 10000 (inclusive). Actual value was: ", partNumber);

    auto& context = IoContext::current();
    TraceContext traceContext = context.makeUserTraceSpan("r2_uploadPart"_kjc);

    traceContext.setTag("cloudflare.binding.type"_kjc, "r2"_kjc);
    KJ_IF_SOME(b, this->bucket->bindingName()) {
      traceContext.setTag("cloudflare.binding.name"_kjc, b);
    }
    traceContext.setTag("cloudflare.r2.operation"_kjc, "UploadPart"_kjc);
    KJ_IF_SOME(b, this->bucket->bucketName()) {
      traceContext.setTag("cloudflare.r2.bucket"_kjc, b);
    }
    traceContext.setTag("cloudflare.r2.request.upload_id"_kjc, uploadId.asPtr());
    traceContext.setTag("cloudflare.r2.request.part_number"_kjc, static_cast<int64_t>(partNumber));
    traceContext.setTag("cloudflare.r2.request.key"_kjc, key.asPtr());

    capnp::JsonCodec json;
    json.handleByAnnotation<R2BindingRequest>();
    json.setHasMode(capnp::HasMode::NON_DEFAULT);
    capnp::MallocMessageBuilder requestMessage;

    auto requestBuilder = requestMessage.initRoot<R2BindingRequest>();
    requestBuilder.setVersion(VERSION_PUBLIC_BETA);
    auto payloadBuilder = requestBuilder.initPayload();
    auto uploadPartBuilder = payloadBuilder.initUploadPart();

    uploadPartBuilder.setUploadId(uploadId);
    uploadPartBuilder.setPartNumber(partNumber);
    uploadPartBuilder.setObject(key);
    KJ_IF_SOME(options, options) {
      KJ_IF_SOME(ssecKey, options.ssecKey) {
        auto ssecBuilder = uploadPartBuilder.initSsec();
        KJ_SWITCH_ONEOF(ssecKey) {
          KJ_CASE_ONEOF(keyString, kj::String) {
            JSG_REQUIRE(
                std::regex_match(keyString.begin(), keyString.end(), std::regex("^[0-9a-f]+$")),
                Error, "SSE-C Key has invalid format");
            JSG_REQUIRE(keyString.size() == 64, Error, "SSE-C Key must be 32 bytes in length");
            ssecBuilder.setKey(kj::str(keyString));
            traceContext.setTag("cloudflare.r2.request.ssec_key"_kjc, true);
          }
          KJ_CASE_ONEOF(keyBuff, kj::Array<byte>) {
            JSG_REQUIRE(keyBuff.size() == 32, Error, "SSE-C Key must be 32 bytes in length");
            ssecBuilder.setKey(kj::encodeHex(keyBuff));
            traceContext.setTag("cloudflare.r2.request.ssec_key"_kjc, true);
          }
        }
      }
    }

    kj::Maybe<int64_t> requestSize = kj::none;
    KJ_SWITCH_ONEOF(value) {
      KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
        KJ_IF_SOME(size, stream->tryGetLength(StreamEncoding::IDENTITY)) {
          requestSize = size;
        }
      }
      KJ_CASE_ONEOF(text, jsg::NonCoercible<kj::String>) {
        requestSize = text.value.size();
      }
      KJ_CASE_ONEOF(data, kj::Array<byte>) {
        requestSize = data.size();
      }
      KJ_CASE_ONEOF(blob, jsg::Ref<Blob>) {
        requestSize = blob->getSize();
      }
    }
    KJ_IF_SOME(size, requestSize) {
      traceContext.setTag("cloudflare.r2.request.size"_kjc, size);
    }

    auto requestJson = json.encode(requestBuilder);
    auto bucket = this->bucket->adminBucket.map([](auto&& s) { return kj::str(s); });

    kj::StringPtr components[1];
    auto path = fillR2Path(components, this->bucket->adminBucket);
    auto client = context.getHttpClient(this->bucket->clientIndex, true, kj::none, traceContext);
    auto promise = doR2HTTPPutRequest(
        kj::mv(client), kj::mv(value), kj::none, kj::mv(requestJson), path, kj::none);

    return context.awaitIo(js, kj::mv(promise),
        [&errorType, partNumber, traceContext = kj::mv(traceContext)](
            jsg::Lock& js, R2Result r2Result) mutable {
      addR2ResponseSpanTags(traceContext, r2Result);
      r2Result.throwIfError("uploadPart", errorType);

      capnp::MallocMessageBuilder responseMessage;
      capnp::JsonCodec json;
      json.handleByAnnotation<R2UploadPartResponse>();
      auto responseBuilder = responseMessage.initRoot<R2UploadPartResponse>();

      json.decode(KJ_ASSERT_NONNULL(r2Result.metadataPayload), responseBuilder);
      kj::StringPtr etag = responseBuilder.getEtag();
      traceContext.setTag("cloudflare.r2.response.etag"_kjc, etag);
      UploadedPart uploadedPart = {partNumber, kj::str(etag)};
      return uploadedPart;
    });
  });
}

jsg::Promise<jsg::Ref<R2Bucket::HeadResult>> R2MultipartUpload::complete(jsg::Lock& js,
    kj::Array<UploadedPart> uploadedParts,
    const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType) {
  return js.evalNow([&] {
    auto& context = IoContext::current();
    TraceContext traceContext = context.makeUserTraceSpan("r2_completeMultipartUpload"_kjc);

    traceContext.setTag("cloudflare.binding.type"_kjc, "r2"_kjc);
    KJ_IF_SOME(b, this->bucket->bindingName()) {
      traceContext.setTag("cloudflare.binding.name"_kjc, b);
    }
    traceContext.setTag("cloudflare.r2.operation"_kjc, "CompleteMultipartUpload"_kjc);
    KJ_IF_SOME(b, this->bucket->bucketName()) {
      traceContext.setTag("cloudflare.r2.bucket"_kjc, b);
    }
    traceContext.setTag("cloudflare.r2.request.upload_id"_kjc, uploadId.asPtr());
    traceContext.setTag("cloudflare.r2.request.key"_kjc, key.asPtr());
    kj::String partIds =
        kj::strArray(KJ_MAP(part, uploadedParts) { return kj::str(part.partNumber); }, ", ");
    traceContext.setTag("cloudflare.r2.request.uploaded_parts"_kjc, kj::mv(partIds));

    capnp::JsonCodec json;
    json.handleByAnnotation<R2BindingRequest>();
    capnp::MallocMessageBuilder requestMessage;

    auto requestBuilder = requestMessage.initRoot<R2BindingRequest>();
    requestBuilder.setVersion(VERSION_PUBLIC_BETA);
    auto completeMultipartUploadBuilder =
        requestBuilder.initPayload().initCompleteMultipartUpload();

    completeMultipartUploadBuilder.setObject(key);
    completeMultipartUploadBuilder.setUploadId(uploadId);

    auto partsList = completeMultipartUploadBuilder.initParts(uploadedParts.size());
    UploadedPart* currentPart = uploadedParts.begin();
    for (unsigned int i = 0; i < uploadedParts.size(); i++) {
      int partNumber = currentPart->partNumber;
      JSG_REQUIRE(partNumber >= 1 && partNumber <= 10000, TypeError,
          "Part number must be between 1 and 10000 (inclusive). Actual value was: ", partNumber);
      partsList[i].setPart(partNumber);
      partsList[i].setEtag(currentPart->etag);
      currentPart = std::next(currentPart);
    }

    auto requestJson = json.encode(requestBuilder);

    kj::StringPtr components[1];
    auto path = fillR2Path(components, this->bucket->adminBucket);
    auto client = context.getHttpClient(this->bucket->clientIndex, true, kj::none, traceContext);
    auto promise =
        doR2HTTPPutRequest(kj::mv(client), kj::none, kj::none, kj::mv(requestJson), path, kj::none);

    return context.awaitIo(js, kj::mv(promise),
        [&errorType, traceContext = kj::mv(traceContext)](
            jsg::Lock& js, R2Result r2Result) mutable {
      addR2ResponseSpanTags(traceContext, r2Result);
      auto parsedObject =
          parseHeadResultWrapper(js, "completeMultipartUpload", r2Result, errorType);
      KJ_IF_SOME(obj, parsedObject) {
        addHeadResultSpanTags(js, traceContext, *obj.get());
        return obj.addRef();
      } else {
        KJ_FAIL_ASSERT(
            "Shouldn't happen, multipart completion should either error or return an object");
      }
    });
  });
}

jsg::Promise<void> R2MultipartUpload::abort(
    jsg::Lock& js, const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType) {
  return js.evalNow([&] {
    auto& context = IoContext::current();
    TraceContext traceContext = context.makeUserTraceSpan("r2_abortMultipartUpload"_kjc);

    traceContext.setTag("cloudflare.binding.type"_kjc, "r2"_kjc);
    KJ_IF_SOME(b, this->bucket->bindingName()) {
      traceContext.setTag("cloudflare.binding.name"_kjc, b);
    }
    traceContext.setTag("cloudflare.r2.operation"_kjc, "AbortMultipartUpload"_kjc);
    KJ_IF_SOME(b, this->bucket->bucketName()) {
      traceContext.setTag("cloudflare.r2.bucket"_kjc, b);
    }
    traceContext.setTag("cloudflare.r2.request.upload_id"_kjc, uploadId.asPtr());
    traceContext.setTag("cloudflare.r2.request.key"_kjc, key.asPtr());

    capnp::JsonCodec json;
    json.handleByAnnotation<R2BindingRequest>();
    capnp::MallocMessageBuilder requestMessage;

    auto requestBuilder = requestMessage.initRoot<R2BindingRequest>();
    requestBuilder.setVersion(VERSION_PUBLIC_BETA);
    auto abortMultipartUploadBuilder = requestBuilder.initPayload().initAbortMultipartUpload();

    abortMultipartUploadBuilder.setObject(key);
    abortMultipartUploadBuilder.setUploadId(uploadId);

    auto requestJson = json.encode(requestBuilder);

    kj::StringPtr components[1];
    auto path = fillR2Path(components, this->bucket->adminBucket);
    auto client = context.getHttpClient(this->bucket->clientIndex, true, kj::none, traceContext);
    auto promise =
        doR2HTTPPutRequest(kj::mv(client), kj::none, kj::none, kj::mv(requestJson), path, kj::none);

    return context.awaitIo(js, kj::mv(promise),
        [&errorType, traceContext = kj::mv(traceContext)](
            jsg::Lock& js, R2Result r2Result) mutable {
      addR2ResponseSpanTags(traceContext, r2Result);
      if (r2Result.objectNotFound()) {
        return;
      }

      r2Result.throwIfError("abortMultipartUpload", errorType);
    });
  });
}

jsg::Promise<R2MultipartUpload::ListPartsResult> R2MultipartUpload::listParts(jsg::Lock& js,
    jsg::Optional<ListPartsOptions> options,
    const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType) {
  return js.evalNow([&] {
    auto& context = IoContext::current();

    auto traceSpan = context.makeTraceSpan("r2_listParts"_kjc);
    auto userSpan = context.makeUserTraceSpan("r2_listParts"_kjc);
    TraceContext traceContext(kj::mv(traceSpan), kj::mv(userSpan));
    auto client = context.getHttpClient(this->bucket->clientIndex, true, kj::none, traceContext);

    traceContext.userSpan.setTag("cloudflare.binding.type"_kjc, "r2"_kjc);
    KJ_IF_SOME(b, this->bucket->bindingName()) {
      traceContext.userSpan.setTag("cloudflare.binding.name"_kjc, b);
    }
    traceContext.userSpan.setTag("cloudflare.r2.operation"_kjc, "ListParts"_kjc);
    KJ_IF_SOME(b, this->bucket->bucketName()) {
      traceContext.userSpan.setTag("cloudflare.r2.bucket"_kjc, b);
    }
    traceContext.userSpan.setTag("cloudflare.r2.request.upload_id"_kjc, uploadId.asPtr());
    traceContext.userSpan.setTag("cloudflare.r2.request.key"_kjc, key.asPtr());

    capnp::JsonCodec json;
    json.handleByAnnotation<R2BindingRequest>();
    json.setHasMode(capnp::HasMode::NON_DEFAULT);
    capnp::MallocMessageBuilder requestMessage;

    auto requestBuilder = requestMessage.initRoot<R2BindingRequest>();
    requestBuilder.setVersion(VERSION_PUBLIC_BETA);
    auto listPartsBuilder = requestBuilder.initPayload().initListParts();

    listPartsBuilder.setObject(key);
    listPartsBuilder.setUploadId(uploadId);

    KJ_IF_SOME(o, options) {
      KJ_IF_SOME(m, o.maxParts) {
        JSG_REQUIRE(m >= 1 && m <= 1000, RangeError,
            "maxParts must be between 1 and 1000 (inclusive). Actual value was: ", m);
        listPartsBuilder.setMaxParts(m);
        traceContext.userSpan.setTag(
            "cloudflare.r2.request.max_parts"_kjc, static_cast<int64_t>(m));
      }
      KJ_IF_SOME(p, o.partNumberMarker) {
        JSG_REQUIRE(
            p > 0, RangeError, "partNumberMarker must be non-negative. Actual value was: ", p);
        listPartsBuilder.setPartNumberMarker(p);
        traceContext.userSpan.setTag(
            "cloudflare.r2.request.part_number_marker"_kjc, static_cast<int64_t>(p));
      }
    }

    auto requestJson = json.encode(requestBuilder);

    kj::StringPtr components[1];
    auto path = fillR2Path(components, this->bucket->adminBucket);
    CompatibilityFlags::Reader flags = {};
    auto jwt = this->bucket->jwt.map([](kj::String& s) -> kj::StringPtr { return s; });
    auto promise = doR2HTTPGetRequest(kj::mv(client), kj::mv(requestJson), path, jwt, flags);

    return context.awaitIo(js, kj::mv(promise),
        [&errorType, traceContext = kj::mv(traceContext)](
            jsg::Lock& js, R2Result r2Result) mutable {
      addR2ResponseSpanTags(traceContext, r2Result);
      r2Result.throwIfError("listParts", errorType);

      ListPartsResult result;
      capnp::MallocMessageBuilder responseMessage;
      capnp::JsonCodec json;
      json.handleByAnnotation<R2ListPartsResponse>();
      auto responseBuilder = responseMessage.initRoot<R2ListPartsResponse>();

      json.decode(KJ_ASSERT_NONNULL(r2Result.metadataPayload), responseBuilder);

      result.parts = KJ_MAP(p, responseBuilder.getParts()) {
        return UploadedPartInfo{
          .partNumber = static_cast<int>(p.getPartNumber()),
          .etag = kj::str(p.getEtag()),
          .size = static_cast<double>(p.getSize()),
          .uploaded = kj::UNIX_EPOCH + p.getUploadedMillisecondsSinceEpoch() * kj::MILLISECONDS,
        };
      };
      result.truncated = responseBuilder.getTruncated();
      if (result.truncated) {
        result.partNumberMarker = static_cast<int>(responseBuilder.getPartNumberMarker());
      }

      traceContext.userSpan.setTag(
          "cloudflare.r2.response.returned_parts"_kjc, static_cast<int64_t>(result.parts.size()));
      traceContext.userSpan.setTag("cloudflare.r2.response.truncated"_kjc, result.truncated);
      return kj::mv(result);
    });
  });
}
}  // namespace workerd::api::public_beta
