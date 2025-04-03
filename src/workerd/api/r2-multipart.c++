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

jsg::Promise<R2MultipartUpload::UploadedPart> R2MultipartUpload::uploadPart(jsg::Lock& js,
    int partNumber,
    R2PutValue value,
    jsg::Optional<UploadPartOptions> options,
    const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType) {
  return js.evalNow([&] {
    JSG_REQUIRE(partNumber >= 1 && partNumber <= 10000, TypeError,
        "Part number must be between 1 and 10000 (inclusive). Actual value was: ", partNumber);

    auto& context = IoContext::current();
    auto client = r2GetClient(context, this->bucket->clientIndex,
        {"r2_uploadPart"_kjc, {"rpc.method"_kjc, "UploadPart"_kjc}, this->bucket->adminBucketName(),
          {{"cloudflare.r2.upload_id"_kjc, uploadId.asPtr()}}});

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
          }
          KJ_CASE_ONEOF(keyBuff, kj::Array<byte>) {
            JSG_REQUIRE(keyBuff.size() == 32, Error, "SSE-C Key must be 32 bytes in length");
            ssecBuilder.setKey(kj::encodeHex(keyBuff));
          }
        }
      }
    }

    auto requestJson = json.encode(requestBuilder);
    auto bucket = this->bucket->adminBucket.map([](auto&& s) { return kj::str(s); });

    kj::StringPtr components[1];
    auto path = fillR2Path(components, this->bucket->adminBucket);
    auto promise = doR2HTTPPutRequest(
        kj::mv(client), kj::mv(value), kj::none, kj::mv(requestJson), path, kj::none);

    return context.awaitIo(
        js, kj::mv(promise), [&errorType, partNumber](jsg::Lock& js, R2Result r2Result) mutable {
      r2Result.throwIfError("uploadPart", errorType);

      capnp::MallocMessageBuilder responseMessage;
      capnp::JsonCodec json;
      json.handleByAnnotation<R2UploadPartResponse>();
      auto responseBuilder = responseMessage.initRoot<R2UploadPartResponse>();

      json.decode(KJ_ASSERT_NONNULL(r2Result.metadataPayload), responseBuilder);
      kj::String etag = kj::str(responseBuilder.getEtag());
      UploadedPart uploadedPart = {partNumber, kj::mv(etag)};
      return uploadedPart;
    });
  });
}

jsg::Promise<jsg::Ref<R2Bucket::HeadResult>> R2MultipartUpload::complete(jsg::Lock& js,
    kj::Array<UploadedPart> uploadedParts,
    const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType) {
  return js.evalNow([&] {
    auto& context = IoContext::current();
    auto client = r2GetClient(context, this->bucket->clientIndex,
        {"r2_completeMultipartUpload"_kjc, {"rpc.method"_kjc, "CompleteMultipartUpload"_kjc},
          this->bucket->adminBucketName(), {{"cloudflare.r2.upload_id"_kjc, uploadId.asPtr()}}});

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
    auto promise =
        doR2HTTPPutRequest(kj::mv(client), kj::none, kj::none, kj::mv(requestJson), path, kj::none);

    return context.awaitIo(
        js, kj::mv(promise), [&errorType](jsg::Lock& js, R2Result r2Result) mutable {
      auto parsedObject =
          parseHeadResultWrapper(js, "completeMultipartUpload", r2Result, errorType);
      KJ_IF_SOME(obj, parsedObject) {
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
    auto client = r2GetClient(context, this->bucket->clientIndex,
        {"r2_abortMultipartUpload"_kjc, {"rpc.method"_kjc, "AbortMultipartUpload"_kjc},
          this->bucket->adminBucketName(), {{"cloudflare.r2.upload_id"_kjc, uploadId.asPtr()}}});

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
    auto promise =
        doR2HTTPPutRequest(kj::mv(client), kj::none, kj::none, kj::mv(requestJson), path, kj::none);

    return context.awaitIo(js, kj::mv(promise), [&errorType](jsg::Lock& js, R2Result r) {
      if (r.objectNotFound()) {
        return;
      }

      r.throwIfError("abortMultipartUpload", errorType);
    });
  });
}
}  // namespace workerd::api::public_beta
