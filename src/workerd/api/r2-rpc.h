// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/blob.h>
#include <workerd/jsg/jsg.h>

namespace kj {
class HttpClient;
}

namespace workerd::api {

class ReadableStreamSource;
class ReadableStream;

// NOTE: We don't currently actually use this as a structured object (hence the `kj::Own<R2Error>`
// that we see pop up).
// TODO(soon): Switch to structured objects and use jsg::Ref<R2Error> instead of kj::Own<R2Error>
//   to maintain ownership.
class R2Error: public jsg::Object {
 public:
  R2Error(uint v4Code, kj::String message): v4Code(v4Code), message(kj::mv(message)) {}

  constexpr kj::StringPtr getName() const {
    return "R2Error"_kj;
  }
  uint getV4Code() const {
    return v4Code;
  }
  kj::StringPtr getMessage() const {
    return message;
  }
  kj::StringPtr getAction() const {
    return KJ_ASSERT_NONNULL(action);
  }
  jsg::JsValue getStack(jsg::Lock& js);

  JSG_RESOURCE_TYPE(R2Error) {
    JSG_INHERIT_INTRINSIC(v8::kErrorPrototype);

    JSG_READONLY_INSTANCE_PROPERTY(name, getName);
    JSG_READONLY_INSTANCE_PROPERTY(code, getV4Code);
    JSG_READONLY_INSTANCE_PROPERTY(message, getMessage);
    JSG_READONLY_INSTANCE_PROPERTY(action, getAction);

    JSG_READONLY_INSTANCE_PROPERTY(stack, getStack);
    // See getStack in dom-exception.h

    JSG_TS_ROOT();
  }

 private:
  uint v4Code;
  kj::String message;
  kj::Maybe<kj::String> action;
  // Initialized when thrown.

  kj::Maybe<v8::Global<v8::Object>> errorForStack;
  // See dom-exception.h.

  friend struct R2Result;
};

using R2PutValue = kj::OneOf<jsg::Ref<ReadableStream>,
    kj::Array<kj::byte>,
    jsg::NonCoercible<kj::String>,
    jsg::Ref<Blob>>;

struct R2Result {
  uint httpStatus;

  // Non-null if httpStatus >= 400.
  kj::Maybe<kj::Own<R2Error>> toThrow;

  kj::Maybe<kj::Array<char>> metadataPayload;

  kj::Maybe<kj::Own<workerd::api::ReadableStreamSource>> stream;

  bool objectNotFound() {
    return httpStatus == 404 && v4ErrorCode() == 10007;
  }

  bool preconditionFailed() {
    return httpStatus == 412 && (v4ErrorCode() == 10031 || v4ErrorCode() == 10032);
  }

  kj::Maybe<uint> v4ErrorCode();
  void throwIfError(kj::StringPtr action, const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType);
};

kj::Promise<R2Result> doR2HTTPGetRequest(kj::Own<kj::HttpClient> client,
    kj::String metadataPayload,
    kj::ArrayPtr<kj::StringPtr> path,
    kj::Maybe<kj::StringPtr> jwt,
    CompatibilityFlags::Reader flags);

kj::Promise<R2Result> doR2HTTPPutRequest(kj::Own<kj::HttpClient> client,
    kj::Maybe<R2PutValue> value,
    kj::Maybe<uint64_t> streamSize,
    // Deprecated. For internal beta API only.
    kj::String metadataPayload,
    kj::ArrayPtr<kj::StringPtr> path,
    kj::Maybe<kj::StringPtr> jwt);

}  // namespace workerd::api
