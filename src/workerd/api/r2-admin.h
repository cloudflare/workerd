// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "r2-bucket.h"
#include <workerd/jsg/jsg.h>

namespace edgeworker::api {
class R2CrossAccount;
}

namespace workerd::api::public_beta {

// A capability to an R2 Admin interface.
class R2Admin: public jsg::Object {

  // A friend tag that grants access to an internal constructor for the R2CrossAccount binding
  struct friend_tag_t {};

  struct FeatureFlags: public R2Bucket::FeatureFlags {
    using R2Bucket::FeatureFlags::FeatureFlags;
  };

public:
  // `subrequestChannel` is what to pass to IoContext::getHttpClient() to get an HttpClient
  // representing this namespace.
  explicit R2Admin(CompatibilityFlags::Reader featureFlags, uint subrequestChannel)
      : featureFlags(featureFlags),
        subrequestChannel(subrequestChannel) {}

  // This constructor is intended to be used by the R2CrossAccount binding, which has access to the
  // friend_tag
  R2Admin(FeatureFlags featureFlags, uint subrequestChannel, kj::String jwt, friend_tag_t)
      : featureFlags(featureFlags),
        subrequestChannel(subrequestChannel),
        jwt(kj::mv(jwt)) {}

  struct ListOptions {
    jsg::Optional<int> limit;
    jsg::Optional<kj::String> cursor;

    JSG_STRUCT(limit, cursor);
  };

  class RetrievedBucket: public R2Bucket {
  public:
    RetrievedBucket(R2Bucket::FeatureFlags featureFlags,
        uint subrequestChannel,
        kj::String name,
        kj::Date created)
        : R2Bucket(featureFlags, subrequestChannel, kj::mv(name), R2Bucket::friend_tag_t{}),
          created(created) {}

    kj::String getName() const {
      return kj::str(KJ_ASSERT_NONNULL(adminBucketName()));
    }
    kj::Date getCreated() const {
      return created;
    }

    JSG_RESOURCE_TYPE(RetrievedBucket) {
      JSG_INHERIT(R2Bucket);
      JSG_READONLY_INSTANCE_PROPERTY(name, getName);
      JSG_READONLY_INSTANCE_PROPERTY(created, getCreated);
    }

  private:
    kj::Date created;

    friend class R2Admin;
  };

  struct ListResult {
    jsg::JsRef<jsg::JsMap> buckets;
    bool truncated = false;
    jsg::Optional<kj::String> cursor;

    JSG_STRUCT(buckets, truncated, cursor);
  };

  jsg::Promise<jsg::Ref<R2Bucket>> create(
      jsg::Lock& js, kj::String name, const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType);
  jsg::Ref<R2Bucket> get(jsg::Lock& js, kj::String name);
  jsg::Promise<void> delete_(
      jsg::Lock& js, kj::String bucketName, const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType);
  jsg::Promise<ListResult> list(jsg::Lock& js,
      jsg::Optional<ListOptions> options,
      const jsg::TypeHandler<jsg::Ref<RetrievedBucket>>& retrievedBucketType,
      const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType,
      CompatibilityFlags::Reader flags);

  JSG_RESOURCE_TYPE(R2Admin) {
    JSG_METHOD(create);
    JSG_METHOD(get);
    JSG_METHOD_NAMED(delete, delete_);
    JSG_METHOD(list);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("jwt", jwt);
  }

private:
  R2Bucket::FeatureFlags featureFlags;
  uint subrequestChannel;
  kj::Maybe<kj::String> jwt;

  friend class edgeworker::api::R2CrossAccount;
};

#define EW_R2_PUBLIC_BETA_ADMIN_ISOLATE_TYPES                                                      \
  api::public_beta::R2Admin, api::public_beta::R2Admin::RetrievedBucket,                           \
      api::public_beta::R2Admin::ListOptions, api::public_beta::R2Admin::ListResult

// The list of r2-admin.h types that are added to worker.c++'s JSG_DECLARE_ISOLATE_TYPE

}  // namespace workerd::api::public_beta
