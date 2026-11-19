// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "body-source.h"

#include <workerd/api/encoding.h>
#include <workerd/api/streams/common.h>
#include <workerd/io/features.h>

namespace workerd::api {

namespace {

// How a string body writes a lone surrogate; see string_body_replace_surrogates.
LoneSurrogates stringBodyLoneSurrogates(jsg::Lock& js) {
  return FeatureFlags::get(js).getStringBodyReplaceSurrogates() ? LoneSurrogates::REPLACE
                                                                : LoneSurrogates::WTF8;
}

// Whether JsReadableStream::text() removes a leading UTF-8 byte order mark: the TypeScript
// implementation always does, and the C++ one does under strip_bom_in_read_all_text.
bool textStripsBom(jsg::Lock& js) {
  auto flags = FeatureFlags::get(js);
  return flags.getTypeScriptImplementedStreams() || flags.getStripBomInReadAllText();
}

}  // namespace

BodySource::BodySource(jsg::Lock& js, jsg::JsString text): value(text.addRef(js)) {}

BodySource::BodySource(jsg::Lock& js, jsg::JsBufferSource buffer): value(buffer.addRef(js)) {}

BodySource::BodySource(jsg::Lock& js, kj::ArrayPtr<const kj::byte> bytes)
    : BodySource(js, jsg::JsBufferSource(jsg::JsUint8Array::create(js, bytes))) {}

BodySource::BodySource(jsg::Ref<Blob> blob): value(kj::mv(blob)) {}

BodySource BodySource::clone(jsg::Lock& js) {
  KJ_SWITCH_ONEOF(value) {
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

jsg::JsArrayBuffer BodySource::readArrayBuffer(jsg::Lock& js) {
  KJ_IF_SOME(bytes, tryGetBytes(js)) {
    return jsg::JsArrayBuffer::create(js, bytes);
  }
  auto& text = KJ_ASSERT_NONNULL(value.tryGet<jsg::JsRef<jsg::JsString>>());
  // Allocate the buffer first: encoding must not cause a V8 GC.
  auto buffer =
      jsg::JsArrayBuffer::create(js, KJ_ASSERT_NONNULL(tryGetLength(js, StreamEncoding::IDENTITY)));
  encodeText(js, text.getHandle(js), buffer.asArrayPtr().asChars());
  return buffer;
}

kj::String BodySource::readText(jsg::Lock& js) {
  kj::String text;
  KJ_IF_SOME(bytes, tryGetBytes(js)) {
    text = kj::heapString(bytes.asChars());
  } else {
    auto& string = KJ_ASSERT_NONNULL(value.tryGet<jsg::JsRef<jsg::JsString>>());
    text = kj::heapString(KJ_ASSERT_NONNULL(tryGetLength(js, StreamEncoding::IDENTITY)));
    encodeText(js, string.getHandle(js), text.asArray());
  }
  if (textStripsBom(js) && hasUtf8Bom(text.asBytes())) {
    return kj::heapString(text.slice(UTF8_BOM_SIZE));
  }
  return text;
}

kj::Maybe<kj::ArrayPtr<const kj::byte>> BodySource::tryGetBytes(jsg::Lock& js) {
  KJ_SWITCH_ONEOF(value) {
    KJ_CASE_ONEOF(text, jsg::JsRef<jsg::JsString>) {
      return kj::none;
    }
    KJ_CASE_ONEOF(buffer, jsg::JsRef<jsg::JsBufferSource>) {
      return buffer.getHandle(js).asArrayPtr().asConst();
    }
    KJ_CASE_ONEOF(blob, jsg::Ref<Blob>) {
      return blob->getData(js);
    }
  }
  KJ_UNREACHABLE;
}

void BodySource::encodeText(jsg::Lock& js, jsg::JsString text, kj::ArrayPtr<char> output) {
  auto result = encodeUtf8Prefix(js, text, 0, output, stringBodyLoneSurrogates(js));
  KJ_ASSERT(result.written == output.size(), "a string produced fewer bytes than its UTF-8 length");
}

kj::Maybe<uint64_t> BodySource::tryGetLength(jsg::Lock& js, StreamEncoding encoding) {
  if (encoding != StreamEncoding::IDENTITY) {
    return kj::none;
  }
  KJ_SWITCH_ONEOF(value) {
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
