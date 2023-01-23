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

    R2MultipartUpload(kj::String key, kj::String uploadId, jsg::Ref<R2Bucket> bucket):
      key(kj::mv(key)), uploadId(kj::mv(uploadId)), bucket(kj::mv(bucket)) {}

    kj::StringPtr getKey() const { return kj::StringPtr(key); }
    kj::StringPtr getUploadId() const { return kj::StringPtr(uploadId); }

    jsg::Promise<UploadedPart> uploadPart(
        jsg::Lock& js, int partNumber, R2PutValue value,
        const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType,
        CompatibilityFlags::Reader featureFlags
    );
    jsg::Promise<void> abort(
        jsg::Lock& js, const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType,
        CompatibilityFlags::Reader featureFlags
    );
    jsg::Promise<jsg::Ref<R2Bucket::HeadResult>> complete(
        jsg::Lock& js, kj::Array<UploadedPart> uploadedParts,
        const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType,
        CompatibilityFlags::Reader featureFlags
    );

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

  private:
    void visitForGc(jsg::GcVisitor& visitor) {
      visitor.visit(bucket);
    }
};

}
