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
  struct UploadPartCopySource {
    kj::String bucket;
    kj::String object;
    jsg::Optional<kj::OneOf<R2Bucket::Conditional, jsg::Ref<Headers>>> onlyIf;
    jsg::Optional<kj::OneOf<R2Bucket::Range, jsg::Ref<Headers>>> range;
    jsg::Optional<kj::OneOf<kj::Array<byte>, kj::String>> ssecKey;

    JSG_STRUCT(bucket, object, onlyIf, range, ssecKey);
    JSG_STRUCT_TS_OVERRIDE(R2UploadPartCopySource);
  };
  struct UploadPartCopyOptions {
    jsg::Optional<kj::OneOf<kj::Array<byte>, kj::String>> ssecKey;

    JSG_STRUCT(ssecKey);
    JSG_STRUCT_TS_OVERRIDE(R2UploadPartCopyOptions);
  };

  R2MultipartUpload(kj::String key, kj::String uploadId, jsg::Ref<R2Bucket> bucket)
      : key(kj::mv(key)),
        uploadId(kj::mv(uploadId)),
        bucket(kj::mv(bucket)) {}

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
  jsg::Promise<kj::Maybe<UploadedPart>> uploadPartCopy(jsg::Lock& js,
      int partNumber,
      UploadPartCopySource source,
      jsg::Optional<UploadPartCopyOptions> options,
      const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType);
  jsg::Promise<void> abort(jsg::Lock& js, const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType);
  jsg::Promise<jsg::Ref<R2Bucket::HeadResult>> complete(jsg::Lock& js,
      kj::Array<UploadedPart> uploadedParts,
      const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType);

  JSG_RESOURCE_TYPE(R2MultipartUpload) {
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(key, getKey);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(uploadId, getUploadId);
    JSG_METHOD(uploadPart);
    JSG_METHOD(uploadPartCopy);
    JSG_METHOD(abort);
    JSG_METHOD(complete);

    JSG_TS_OVERRIDE({
       uploadPartCopy(partNumber: number, source: R2UploadPartCopySource & { onlyIf: R2BucketConditional | Headers }, options?: R2UploadPartCopyOptions): Promise<R2UploadedPart | undefined>;
       uploadPartCopy(partNumber: number, source: R2UploadPartCopySource, options?: R2UploadPartCopyOptions): Promise<R2UploadedPart>;
    });
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("key", key);
    tracker.trackField("uploadId", uploadId);
    tracker.trackField("bucket", bucket);
  }

 protected:
  kj::String key;
  kj::String uploadId;
  jsg::Ref<R2Bucket> bucket;

 private:
  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(bucket);
  }
};

}  // namespace workerd::api::public_beta
