// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "r2-bucket.h"

#include <workerd/jsg/jsg.h>

namespace workerd::api::public_beta {
class R2MultipartUpload: public jsg::Object {
  public:
    R2MultipartUpload(kj::String key, kj::String uploadId, jsg::Ref<R2Bucket> bucket):
      key(kj::mv(key)), uploadId(kj::mv(uploadId)), bucket(kj::mv(bucket)) {}

    kj::String getKey() const { return kj::str(key); }
    kj::String getUploadId() const { return kj::str(uploadId); }

    jsg::Promise<R2Bucket::UploadedPart> uploadPart(
      jsg::Lock& js,
      int partNumber,
      R2PutValue value,
      const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType) {
        return bucket.get()->uploadPart(
          js, kj::str(key), kj::str(uploadId), partNumber, kj::mv(value), errorType
        );
    }
    jsg::Promise<void> abort(jsg::Lock& js, const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType) {
      return bucket.get()->abortMultipartUpload(
        js, kj::str(key), kj::str(uploadId), errorType
      );
    }
    jsg::Promise<jsg::Ref<R2Bucket::HeadResult>> complete(
      jsg::Lock& js,
      kj::Array<R2Bucket::UploadedPart> uploadedParts,
      const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType) {
        return bucket.get()->completeMultipartUpload(
          js, kj::str(key), kj::str(uploadId), kj::mv(uploadedParts), errorType
        );
    }

    JSG_RESOURCE_TYPE(R2MultipartUpload) {
      JSG_LAZY_READONLY_INSTANCE_PROPERTY(key, getKey);
      JSG_LAZY_READONLY_INSTANCE_PROPERTY(uploadId, getUploadId);
      JSG_METHOD(uploadPart);
      JSG_METHOD(abort);
      JSG_METHOD(complete);
    }

  protected:
    kj::String key;
    kj::String uploadId;
    jsg::Ref<R2Bucket> bucket;
};

}
