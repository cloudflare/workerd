// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "r2-bucket.h"

#include <workerd/jsg/jsg.h>

namespace workerd::api::public_beta {

class R2MultipartUpload: public jsg::Object {
 public:
  struct UploadedPart {
    int partNumber;
    kj::String etag;

    JSG_STRUCT(partNumber, etag);
    JSG_STRUCT_TS_OVERRIDE(R2UploadedPart);
  };
  struct UploadPartOptions {
    jsg::Optional<kj::OneOf<kj::Array<byte>, kj::String>> ssecKey;

    JSG_STRUCT(ssecKey);
    JSG_STRUCT_TS_OVERRIDE(R2UploadPartOptions);
  };

  R2MultipartUpload(kj::String key, kj::String uploadId, jsg::Ref<R2Bucket> bucket)
      : key(kj::mv(key)),
        uploadId(kj::mv(uploadId)),
        bucket(kj::mv(bucket)) {}
  R2MultipartUpload(
      kj::String key, kj::String uploadId, jsg::Ref<R2Bucket> bucket, R2RpcClient rpcClient)
      : key(kj::mv(key)),
        uploadId(kj::mv(uploadId)),
        bucket(kj::mv(bucket)),
        rpcClient(kj::mv(rpcClient)) {}

  kj::StringPtr getKey() const {
    return key;
  }
  kj::StringPtr getUploadId() const {
    return uploadId;
  }

  jsg::Promise<UploadedPart> uploadPart(jsg::Lock& js,
      int partNumber,
      R2PutValue value,
      jsg::Optional<UploadPartOptions> options,
      const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType);
  jsg::Promise<void> abort(jsg::Lock& js, const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType);
  jsg::Promise<jsg::Ref<R2Bucket::HeadResult>> complete(jsg::Lock& js,
      kj::Array<UploadedPart> uploadedParts,
      const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType);
  jsg::Promise<UploadedPart> uploadPartRpc(jsg::Lock& js,
      int partNumber,
      R2PutValue value,
      jsg::Optional<UploadPartOptions> options,
      const jsg::TypeHandler<jsg::Ref<JsRpcProperty>>& rpcPropHandler,
      const jsg::TypeHandler<jsg::Function<jsg::Value(
          int, R2PutValueRpc, jsg::Optional<UploadPartOptions>)>>& uploadPartFnHandler,
      const jsg::TypeHandler<jsg::Promise<UploadedPart>>& uploadPartResultHandler);
  jsg::Promise<void> abortRpc(jsg::Lock& js,
      const jsg::TypeHandler<jsg::Ref<JsRpcProperty>>& rpcPropHandler,
      const jsg::TypeHandler<jsg::Function<jsg::Value()>>& abortFnHandler,
      const jsg::TypeHandler<jsg::Promise<void>>& abortResultHandler);
  jsg::Promise<jsg::Ref<R2Bucket::HeadResult>> completeRpc(jsg::Lock& js,
      kj::Array<UploadedPart> uploadedParts,
      const jsg::TypeHandler<jsg::Ref<JsRpcProperty>>& rpcPropHandler,
      const jsg::TypeHandler<jsg::Function<jsg::Value(kj::Array<UploadedPart>)>>& completeFnHandler,
      const jsg::TypeHandler<jsg::Promise<R2Bucket::HeadResultRpc>>& completeResultHandler);

  JSG_RESOURCE_TYPE(R2MultipartUpload, CompatibilityFlags::Reader flags) {
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(key, getKey);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(uploadId, getUploadId);
    if (util::Autogate::isEnabled(util::AutogateKey::R2_BINDINGS_JSRPC) &&
        flags.getR2BindingsJsrpc()) {
      JSG_METHOD_NAMED(uploadPart, uploadPartRpc);
      JSG_METHOD_NAMED(abort, abortRpc);
      JSG_METHOD_NAMED(complete, completeRpc);
    } else {
      JSG_METHOD(uploadPart);
      JSG_METHOD(abort);
      JSG_METHOD(complete);
    }
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("key", key);
    tracker.trackField("uploadId", uploadId);
    tracker.trackField("bucket", bucket);
    tracker.trackFieldWithSize("rpcClient", sizeof(rpcClient));
  }

 protected:
  kj::String key;
  kj::String uploadId;
  jsg::Ref<R2Bucket> bucket;
  kj::Maybe<R2RpcClient> rpcClient;

 private:
  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(bucket);
  }
};

}  // namespace workerd::api::public_beta
