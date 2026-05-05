// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "readable.h"
#include "writable.h"

namespace workerd::api {

// A TransformStream is a readable, writable pair in which whatever is written to the writable
// side can be read from the readable side, possibly transformed into a different type of value.
//
// The original version of TransformStream in Workers was nothing more than an identity
// passthrough that only handled byte data. No actual transformation of the value was performed.
// The original version did not conform to the streams standard. That original version has been
// migrated into the IdentityTransformStream class. If the
// transformstream_enable_standard_constructor compatibility flag is not enabled, then
// TransformStream is just an alias for IdentityTransformStream and continues to implement the
// non-standard behavior. With the transformstream_enable_standard_constructor flag set, however,
// the TransformStream implements standardized behavior.
class TransformStream: public jsg::Object {
 public:
  explicit TransformStream(jsg::Ref<ReadableStream> readable, jsg::Ref<WritableStream> writable)
      : readable(kj::mv(readable)),
        writable(kj::mv(writable)) {}

  static jsg::Ref<TransformStream> constructor(jsg::Lock& js,
      jsg::Optional<Transformer> maybeTransformer,
      jsg::Optional<StreamQueuingStrategy> maybeWritableStrategy,
      jsg::Optional<StreamQueuingStrategy> maybeReadableStrategy);

  jsg::Ref<ReadableStream> getReadable() {
    return readable.addRef();
  }
  jsg::Ref<WritableStream> getWritable() {
    return writable.addRef();
  }

  JSG_RESOURCE_TYPE(TransformStream, CompatibilityFlags::Reader flags) {
    if (flags.getJsgPropertyOnPrototypeTemplate()) {
      JSG_READONLY_PROTOTYPE_PROPERTY(readable, getReadable);
      JSG_READONLY_PROTOTYPE_PROPERTY(writable, getWritable);

      JSG_TS_OVERRIDE(<I = any, O = any> {
        constructor(transformer?: Transformer<I, O>, writableStrategy?: QueuingStrategy<I>, readableStrategy?: QueuingStrategy<O>);
        get readable(): ReadableStream<O>;
        get writable(): WritableStream<I>;
      });
    } else {
      JSG_READONLY_INSTANCE_PROPERTY(readable, getReadable);
      JSG_READONLY_INSTANCE_PROPERTY(writable, getWritable);

      JSG_TS_OVERRIDE(<I = any, O = any> {
        constructor(transformer?: Transformer<I, O>, writableStrategy?: QueuingStrategy<I>, readableStrategy?: QueuingStrategy<O>);
        readonly readable: ReadableStream<O>;
        readonly writable: WritableStream<I>;
      });
    }
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("readable", readable);
    tracker.trackField("writable", writable);
  }

 private:
  jsg::Ref<ReadableStream> readable;
  jsg::Ref<WritableStream> writable;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(readable, writable);
  }
};

}  // namespace workerd::api
