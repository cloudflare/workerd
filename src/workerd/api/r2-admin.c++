// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "r2-admin.h"
#include "r2-rpc.h"
#include <kj/compat/http.h>
#include <capnp/compat/json.h>
#include <workerd/util/http-util.h>
#include <workerd/api/r2-api.capnp.h>

namespace workerd::api::public_beta {
jsg::Ref<R2Bucket> R2Admin::get(jsg::Lock& js, kj::String bucketName) {
  KJ_IF_MAYBE(j, jwt) {
    return jsg::alloc<R2Bucket>(featureFlags, subrequestChannel, kj::mv(bucketName),
                                kj::str(*j), R2Bucket::friend_tag_t{});
  }
  return jsg::alloc<R2Bucket>(featureFlags, subrequestChannel, kj::mv(bucketName), R2Bucket::friend_tag_t{});
}

jsg::Promise<jsg::Ref<R2Bucket>> R2Admin::create(jsg::Lock& js, kj::String name,
    const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType) {
  auto& context = IoContext::current();
  auto client = context.getHttpClient(subrequestChannel, true, nullptr, "r2_delete"_kj);

  capnp::JsonCodec json;
  json.handleByAnnotation<R2BindingRequest>();
  capnp::MallocMessageBuilder requestMessage;

  auto requestBuilder = requestMessage.initRoot<R2BindingRequest>();
  requestBuilder.setVersion(VERSION_PUBLIC_BETA);
  auto payloadBuilder = requestBuilder.initPayload();
  auto createBucketBuilder = payloadBuilder.initCreateBucket();
  createBucketBuilder.setBucket(name);

  auto requestJson = json.encode(requestBuilder);
  auto promise = doR2HTTPPutRequest(js, kj::mv(client), nullptr, nullptr,
                                    kj::mv(requestJson), nullptr, jwt);

  return context.awaitIo(kj::mv(promise),
      [this, subrequestChannel = subrequestChannel, name = kj::mv(name), &errorType]
      (R2Result r2Result) mutable {
    r2Result.throwIfError("createBucket", errorType);
    return jsg::alloc<R2Bucket>(featureFlags, subrequestChannel, kj::mv(name),
        R2Bucket::friend_tag_t{});
  });
}

jsg::Promise<R2Admin::ListResult> R2Admin::list(jsg::Lock& js,
    jsg::Optional<ListOptions> options,
    const jsg::TypeHandler<jsg::Ref<RetrievedBucket>>& retrievedBucketType,
    const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType, CompatibilityFlags::Reader flags) {
  auto& context = IoContext::current();
  auto client = context.getHttpClient(subrequestChannel, true, nullptr, "r2_delete"_kj);

  capnp::JsonCodec json;
  json.handleByAnnotation<R2BindingRequest>();
  json.setHasMode(capnp::HasMode::NON_DEFAULT);
  capnp::MallocMessageBuilder requestMessage;

  auto requestBuilder = requestMessage.initRoot<R2BindingRequest>();
  requestBuilder.setVersion(VERSION_PUBLIC_BETA);
  auto payloadBuilder = requestBuilder.initPayload();
  auto listBucketBuilder = payloadBuilder.initListBucket();
  KJ_IF_MAYBE(o, options) {
    KJ_IF_MAYBE(l, o->limit) {
      listBucketBuilder.setLimit(*l);
    }
    KJ_IF_MAYBE(c, o->cursor) {
      listBucketBuilder.setCursor(*c);
    }
  }

  auto requestJson = json.encode(requestBuilder);
  auto promise = doR2HTTPGetRequest(kj::mv(client), kj::mv(requestJson), nullptr, jwt, flags);

  return context.awaitIo(js, kj::mv(promise),
      [this, &retrievedBucketType, &errorType](jsg::Lock& js, R2Result r2Result) mutable {
    r2Result.throwIfError("listBucket", errorType);

    auto isolate = js.v8Isolate;
    auto context = js.v8Context();

    capnp::MallocMessageBuilder responseMessage;
    capnp::JsonCodec json;
    json.handleByAnnotation<R2ListResponse>();
    auto responseBuilder = responseMessage.initRoot<R2ListBucketResponse>();
    json.decode(KJ_ASSERT_NONNULL(r2Result.metadataPayload), responseBuilder);

    auto buckets = v8::Map::New(isolate);
    for(auto b: responseBuilder.getBuckets()) {
      auto bucket = jsg::alloc<RetrievedBucket>(featureFlags, subrequestChannel,
          kj::str(b.getName()),
          kj::UNIX_EPOCH + b.getCreatedMillisecondsSinceEpoch() * kj::MILLISECONDS);
      jsg::check(buckets->Set(
          context, jsg::v8Str(isolate, b.getName()),
          retrievedBucketType.wrap(js, kj::mv(bucket))));
    }

    ListResult result {
      .buckets = jsg::Value(isolate, kj::mv(buckets)),
      .truncated = responseBuilder.getTruncated(),
    };
    if (responseBuilder.hasCursor()) {
      result.cursor = kj::str(responseBuilder.getCursor());
    }

    return kj::mv(result);
  });
}

jsg::Promise<void> R2Admin::delete_(jsg::Lock& js, kj::String name,
    const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType) {
  auto& context = IoContext::current();
  auto client = context.getHttpClient(subrequestChannel, true, nullptr, "r2_delete"_kj);

  capnp::JsonCodec json;
  json.handleByAnnotation<R2BindingRequest>();
  capnp::MallocMessageBuilder requestMessage;

  auto requestBuilder = requestMessage.initRoot<R2BindingRequest>();
  requestBuilder.setVersion(VERSION_PUBLIC_BETA);
  auto payloadBuilder = requestBuilder.initPayload();
  auto deleteBucketBuilder = payloadBuilder.initDeleteBucket();
  deleteBucketBuilder.setBucket(name);

  auto requestJson = json.encode(requestBuilder);
  auto promise = doR2HTTPPutRequest(js, kj::mv(client), nullptr, nullptr,
                                    kj::mv(requestJson), nullptr, jwt);

  return context.awaitIo(kj::mv(promise), [&errorType](R2Result r2Result) mutable {
    r2Result.throwIfError("deleteBucket", errorType);
  });
}

} // namespace workerd::api
