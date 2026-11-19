// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "body-source.h"

#include <workerd/api/encoding.h>
#include <workerd/api/js-streams-bridge.h>
#include <workerd/api/streams/common.h>
#include <workerd/io/features.h>
#include <workerd/io/io-context.h>

namespace workerd::api {

namespace {

// How a string body writes a lone surrogate; see string_body_replace_surrogates.
LoneSurrogates stringBodyLoneSurrogates(jsg::Lock& js) {
  return FeatureFlags::get(js).getStringBodyReplaceSurrogates() ? LoneSurrogates::REPLACE
                                                                : LoneSurrogates::WTF8;
}

}  // namespace

BodySource::BodySource(kj::Array<const kj::byte> bytes)
    : value(kj::rc<kj::Array<const kj::byte>>(kj::mv(bytes))) {}

BodySource::BodySource(kj::String text): BodySource(webstreams::stringToBytes(kj::mv(text))) {}

BodySource::BodySource(jsg::Lock& js, jsg::JsString text): value(text.addRef(js)) {}

BodySource::BodySource(jsg::Lock& js, jsg::JsBufferSource buffer): value(buffer.addRef(js)) {}

BodySource::BodySource(jsg::Ref<Blob> blob): value(kj::mv(blob)) {}

BodySource BodySource::clone(jsg::Lock& js) {
  KJ_SWITCH_ONEOF(value) {
    KJ_CASE_ONEOF(bytes, Bytes) {
      return BodySource(bytes.addRef(), kj::none);
    }
    KJ_CASE_ONEOF(text, jsg::JsRef<jsg::JsString>) {
      return BodySource(text.addRef(js), stringLength);
    }
    KJ_CASE_ONEOF(buffer, jsg::JsRef<jsg::JsBufferSource>) {
      return BodySource(buffer.addRef(js), kj::none);
    }
    KJ_CASE_ONEOF(blob, jsg::Ref<Blob>) {
      return BodySource(blob.addRef(), kj::none);
    }
  }
  KJ_UNREACHABLE;
}

JsReadableStream BodySource::createStream(jsg::Lock& js) {
  KJ_SWITCH_ONEOF(value) {
    KJ_CASE_ONEOF(bytes, Bytes) {
      // newMemorySource() reads the shared bytes in place, keeping them alive as its backing.
      auto data = bytes->asPtr();
      return JsReadableStream::create(
          js, IoContext::current(), newMemorySource(data, bytes.addRef().toOwn()));
    }
    KJ_CASE_ONEOF(text, jsg::JsRef<jsg::JsString>) {
      return JsReadableStream::create(js, text.getHandle(js), stringBodyLoneSurrogates(js));
    }
    KJ_CASE_ONEOF(buffer, jsg::JsRef<jsg::JsBufferSource>) {
      return JsReadableStream::create(js, buffer.getHandle(js));
    }
    KJ_CASE_ONEOF(blob, jsg::Ref<Blob>) {
      return JsReadableStream::create(js, blob.addRef());
    }
  }
  KJ_UNREACHABLE;
}

kj::Maybe<uint64_t> BodySource::tryGetLength(jsg::Lock& js, StreamEncoding encoding) {
  if (encoding != StreamEncoding::IDENTITY) {
    return kj::none;
  }
  KJ_SWITCH_ONEOF(value) {
    KJ_CASE_ONEOF(bytes, Bytes) {
      return bytes->size();
    }
    KJ_CASE_ONEOF(text, jsg::JsRef<jsg::JsString>) {
      KJ_IF_SOME(length, stringLength) {
        return length;
      }
      uint64_t length = text.getHandle(js).utf8Length(js);
      stringLength = length;
      return length;
    }
    KJ_CASE_ONEOF(buffer, jsg::JsRef<jsg::JsBufferSource>) {
      return buffer.getHandle(js).size();
    }
    KJ_CASE_ONEOF(blob, jsg::Ref<Blob>) {
      return blob->getSize();
    }
  }
  KJ_UNREACHABLE;
}

void BodySource::visitForGc(jsg::GcVisitor& visitor) {
  KJ_SWITCH_ONEOF(value) {
    KJ_CASE_ONEOF(bytes, Bytes) {}
    KJ_CASE_ONEOF(text, jsg::JsRef<jsg::JsString>) {
      visitor.visit(text);
    }
    KJ_CASE_ONEOF(buffer, jsg::JsRef<jsg::JsBufferSource>) {
      visitor.visit(buffer);
    }
    KJ_CASE_ONEOF(blob, jsg::Ref<Blob>) {
      visitor.visit(blob);
    }
  }
}

void BodySource::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  KJ_SWITCH_ONEOF(value) {
    KJ_CASE_ONEOF(bytes, Bytes) {
      tracker.trackFieldWithSize("bytes", bytes->size());
    }
    KJ_CASE_ONEOF(text, jsg::JsRef<jsg::JsString>) {
      tracker.trackField("text", text);
    }
    KJ_CASE_ONEOF(buffer, jsg::JsRef<jsg::JsBufferSource>) {
      tracker.trackField("buffer", buffer);
    }
    KJ_CASE_ONEOF(blob, jsg::Ref<Blob>) {
      tracker.trackField("blob", blob);
    }
  }
}

}  // namespace workerd::api
