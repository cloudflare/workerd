// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "blob.h"

#include <workerd/api/streams/readable-source.h>
#include <workerd/api/streams/readable.h>
#include <workerd/io/features.h>
#include <workerd/io/observer.h>
#include <workerd/util/mimetype.h>
#include <workerd/util/stream-utils.h>

namespace workerd::api {

namespace {
// Concatenate an array of segments (parameter to Blob constructor).
kj::Maybe<jsg::JsBufferSource> concat(jsg::Lock& js, jsg::Optional<Blob::Bits> maybeBits) {
  auto bits = kj::mv(maybeBits).orDefault(nullptr);
  if (bits.size() == 0) {
    return kj::none;
  }

  auto rejectResizable = FeatureFlags::get(js).getNoResizableArrayBufferInBlob();
  auto maxBlobSize = Worker::Isolate::from(js).getLimitEnforcer().getBlobSizeLimit();
  static constexpr int kMaxInt KJ_UNUSED = kj::maxValue;
  KJ_DASSERT(maxBlobSize <= kMaxInt, "Blob size limit exceeds int range");
  size_t size = 0;
  kj::SmallArray<size_t, 8> cachedPartSizes(bits.size());
  size_t index = 0;
  for (auto& part: bits) {
    size_t partSize = 0;
    KJ_SWITCH_ONEOF(part) {
      KJ_CASE_ONEOF(bytes, jsg::JsBufferSource) {
        if (rejectResizable) {
          JSG_REQUIRE(
              !bytes.isResizable(), TypeError, "Cannot create a Blob with a resizable ArrayBuffer");
        }
        partSize = bytes.size();
      }
      KJ_CASE_ONEOF(text, kj::String) {
        partSize = text.asBytes().size();
      }
      KJ_CASE_ONEOF(blob, jsg::Ref<Blob>) {
        partSize = blob->getData().size();
      }
    }
    cachedPartSizes[index++] = partSize;

    // We can skip the remaining checks if the part is empty.
    if (partSize == 0) continue;

    // While overflow is *extremely* unlikely to ever be a problem here, let's
    // be extra cautious and check for it anyway.
    static constexpr size_t kOverflowLimit = kj::maxValue;
    // Upper limit the max number of bytes we can add to size to avoid overflow.
    // partSize must be less than or equal to upperLimit. Practically speaking,
    // however, it is practically impossible to reach this limit in any real-world
    // scenario given the size limit check below.
    size_t upperLimit = kOverflowLimit - size;
    JSG_REQUIRE(
        partSize <= upperLimit, RangeError, kj::str("Blob part too large: ", partSize, " bytes"));

    // Checks for oversize
    JSG_REQUIRE(size + partSize <= maxBlobSize, RangeError,
        kj::str("Blob size ", size + partSize, " exceeds limit ", maxBlobSize));
    size += partSize;
  }

  if (size == 0) {
    return kj::none;
  }

  auto u8 = jsg::JsUint8Array::create(js, size);

  auto view = u8.asArrayPtr();

  index = 0;
  for (auto& part: bits) {
    KJ_SWITCH_ONEOF(part) {
      KJ_CASE_ONEOF(bytes, jsg::JsBufferSource) {
        size_t cachedSize = cachedPartSizes[index++];
        // If the ArrayBuffer was resized larger, we'll ignore the additional bytes.
        // If the ArrayBuffer was resized smaller, we'll copy up to the current size.
        // In either case, data is packed tightly — any unused space from shrunk
        // buffers ends up as zeros at the end of the output rather than as gaps
        // in the middle.
        size_t toCopy = kj::min(bytes.size(), cachedSize);
        if (toCopy > 0) {
          KJ_ASSERT(view.size() >= toCopy);
          view.first(toCopy).copyFrom(bytes.asArrayPtr().first(toCopy));
        }
        view = view.slice(toCopy);
      }
      KJ_CASE_ONEOF(text, kj::String) {
        auto byteLength = text.asBytes().size();
        KJ_ASSERT(byteLength == cachedPartSizes[index++]);
        if (byteLength == 0) continue;
        KJ_ASSERT(view.size() >= byteLength);
        view.first(byteLength).copyFrom(text.asBytes());
        view = view.slice(byteLength);
      }
      KJ_CASE_ONEOF(blob, jsg::Ref<Blob>) {
        auto data = blob->getData();
        KJ_ASSERT(data.size() == cachedPartSizes[index++]);
        if (data.size() == 0) continue;
        KJ_ASSERT(view.size() >= data.size());
        view.first(data.size()).copyFrom(data);
        view = view.slice(data.size());
      }
    }
  }

  // view.size() will be non-zero if one or more resizable ArrayBuffers were shrunk
  // between the size-computation pass and the copy pass. In that case, create a
  // trimmed view over just the bytes that were actually written.
  size_t bytesWritten = size - view.size();
  if (bytesWritten == 0) {
    return kj::none;
  }
  if (bytesWritten < size) {
    return jsg::JsBufferSource(u8.slice(js, bytesWritten));
  }

  KJ_ASSERT(view == nullptr);
  return jsg::JsBufferSource(u8);
}

kj::String normalizeType(kj::String type) {
  // This does not properly parse mime types. We have the new workerd::MimeType impl
  // but that handles mime types a bit more strictly than this. Ideally we'd be able to
  // switch over to it but there's a non-zero risk of breaking running code. We might need
  // a compat flag to switch at some point but for now we'll keep this as it is.

  // https://www.w3.org/TR/FileAPI/#constructorBlob step 3 inexplicably insists that if the
  // type contains non-printable-ASCII characters we should discard it, and otherwise we should
  // lower-case it.
  for (char& c: type) {
    if (static_cast<signed char>(c) < 0x20) {
      // Throw it away.
      return nullptr;
    } else if ('A' <= c && c <= 'Z') {
      c = c - 'A' + 'a';
    }
  }

  return kj::mv(type);
}

}  // namespace

Blob::Blob(kj::String type): ownData(Empty{}), data(nullptr), type(kj::mv(type)) {}

Blob::Blob(jsg::Lock& js, jsg::JsBufferSource data, kj::String type)
    : ownData(data.addRef(js)),
      data(data.asArrayPtr()),
      type(kj::mv(type)) {
  if (FeatureFlags::get(js).getNoResizableArrayBufferInBlob()) {
    JSG_REQUIRE(
        !data.isResizable(), TypeError, "Cannot create a Blob with a resizable ArrayBuffer");
  }
}

Blob::Blob(jsg::Ref<Blob> parent, kj::ArrayPtr<const byte> data, kj::String type)
    : ownData(kj::mv(parent)),
      data(data),
      type(kj::mv(type)) {}

jsg::Ref<Blob> Blob::constructor(
    jsg::Lock& js, jsg::Optional<Bits> bits, jsg::Optional<Options> options) {
  kj::String type;  // note: default value is intentionally empty string
  KJ_IF_SOME(o, options) {
    KJ_IF_SOME(t, o.type) {
      type = normalizeType(kj::mv(t));
    }
  }

  KJ_IF_SOME(b, bits) {
    // Optimize for the case where the input is a single Blob, where we can just
    // return a new view on the existing data without copying.
    if (b.size() == 1) {
      KJ_IF_SOME(parent, b[0].template tryGet<jsg::Ref<Blob>>()) {
        if (parent->getSize() == 0) {
          return js.alloc<Blob>(kj::mv(type));
        }
        auto ptr = parent->data;
        KJ_IF_SOME(root, parent->ownData.template tryGet<jsg::Ref<Blob>>()) {
          parent = root.addRef();
        }
        return js.alloc<Blob>(kj::mv(parent), ptr, kj::mv(type));
      }
    }
  }

  KJ_IF_SOME(data, concat(js, kj::mv(bits))) {
    return js.alloc<Blob>(js, data, kj::mv(type));
  }
  return js.alloc<Blob>(kj::mv(type));
}

kj::ArrayPtr<const byte> Blob::getData() const {
  return data;
}

jsg::Ref<Blob> Blob::slice(jsg::Lock& js,
    jsg::Optional<int> maybeStart,
    jsg::Optional<int> maybeEnd,
    jsg::Optional<kj::String> type) {

  auto normalizedType = normalizeType(kj::mv(type).orDefault(nullptr));
  if (data.size() == 0) {
    // Blob is empty, there's nothing to slice.
    return js.alloc<Blob>(kj::mv(normalizedType));
  }

  int start = maybeStart.orDefault(0);
  int end = maybeEnd.orDefault(data.size());

  if (start < 0) {
    // Negative value interpreted as offset from end.
    start += data.size();
  }
  if (end < 0) {
    // Negative value interpreted as offset from end.
    end += data.size();
  }

  // Clamp start and end to range.
  start = kj::max(0, kj::min(start, static_cast<int>(data.size())));
  end = kj::max(start, kj::min(end, static_cast<int>(data.size())));

  // We run with KJ_IREQUIRE checks enabled in production, which will catch
  // out of bounds start/end ... but since we're clamping them above, this
  // should never actually be a problem.
  auto slicedData = data.slice(start, end);

  // If the slice is empty, we can just return a new empty Blob without worrying about
  // referencing the original data at all. Super minor optimization that avoids an
  // unnecessary refcount.
  if (slicedData.size() == 0) {
    return js.alloc<Blob>(kj::mv(normalizedType));
  }

  KJ_SWITCH_ONEOF(ownData) {
    KJ_CASE_ONEOF(_, Empty) {
      // Handled at the beginning of the function with the zero-length check.
      KJ_FAIL_ASSERT("Empty blob should have been handled at the beginning of the function");
    }
    KJ_CASE_ONEOF(parent, jsg::Ref<Blob>) {
      // If this blob is itself a slice (backed by a Ref<Blob>), reference the
      // root data-owning blob directly. This prevents unbounded chain depth —
      // every slice always points to the root, so depth is always ≤ 1.
      return js.alloc<Blob>(parent.addRef(), slicedData, kj::mv(normalizedType));
    }
    KJ_CASE_ONEOF(_, jsg::JsRef<jsg::JsBufferSource>) {
      return js.alloc<Blob>(JSG_THIS, slicedData, kj::mv(normalizedType));
    }
  }
  KJ_UNREACHABLE;
}

jsg::Promise<jsg::JsRef<jsg::JsArrayBuffer>> Blob::arrayBuffer(jsg::Lock& js) {
  FeatureObserver::maybeRecordUse(FeatureObserver::Feature::BLOB_AS_ARRAY_BUFFER);
  auto ret = jsg::JsArrayBuffer::create(js, data);
  return js.resolvedPromise(ret.addRef(js));
}

jsg::Promise<jsg::JsRef<jsg::JsUint8Array>> Blob::bytes(jsg::Lock& js) {
  FeatureObserver::maybeRecordUse(FeatureObserver::Feature::BLOB_AS_ARRAY_BUFFER);
  auto ret = jsg::JsUint8Array::create(js, data);
  return js.resolvedPromise(ret.addRef(js));
}

jsg::Promise<jsg::JsRef<jsg::JsString>> Blob::text(jsg::Lock& js) {
  FeatureObserver::maybeRecordUse(FeatureObserver::Feature::BLOB_AS_TEXT);
  // Using js.str here instead of returning kj::String avoids an additional
  // intermediate allocation and copy of the string data.
  return js.resolvedPromise(js.str(data.asChars()).addRef(js));
}

jsg::Ref<ReadableStream> Blob::stream(jsg::Lock& js) {
  FeatureObserver::maybeRecordUse(FeatureObserver::Feature::BLOB_AS_STREAM);
  return js.alloc<ReadableStream>(
      IoContext::current(), streams::newMemorySource(data, kj::heap(JSG_THIS)));
}

// =======================================================================================

File::File(kj::String name, kj::String type, double lastModified)
    : Blob(kj::mv(type)),
      name(kj::mv(name)),
      lastModified(lastModified) {}

File::File(
    jsg::Lock& js, jsg::JsBufferSource data, kj::String name, kj::String type, double lastModified)
    : Blob(js, kj::mv(data), kj::mv(type)),
      name(kj::mv(name)),
      lastModified(lastModified) {}

File::File(jsg::Ref<Blob> parent,
    kj::ArrayPtr<const byte> data,
    kj::String name,
    kj::String type,
    double lastModified)
    : Blob(kj::mv(parent), data, kj::mv(type)),
      name(kj::mv(name)),
      lastModified(lastModified) {}

jsg::Ref<File> File::constructor(
    jsg::Lock& js, jsg::Optional<Bits> bits, kj::String name, jsg::Optional<Options> options) {
  kj::String type;  // note: default value is intentionally empty string
  kj::Maybe<double> maybeLastModified;
  KJ_IF_SOME(o, options) {
    KJ_IF_SOME(t, o.type) {
      type = normalizeType(kj::mv(t));
    }
    maybeLastModified = o.lastModified;
  }

  double lastModified;
  KJ_IF_SOME(m, maybeLastModified) {
    lastModified = kj::isNaN(m) ? 0 : m;
  } else {
    lastModified = dateNow();
  }

  KJ_IF_SOME(b, bits) {
    // Optimize for the case where the input is a single Blob, where we can just
    // return a new view on the existing data without copying.
    if (b.size() == 1) {
      KJ_IF_SOME(parent, b[0].template tryGet<jsg::Ref<Blob>>()) {
        if (parent->getSize() == 0) {
          return js.alloc<File>(kj::mv(name), kj::mv(type), lastModified);
        }
        auto ptr = parent->data;
        KJ_IF_SOME(root, parent->ownData.template tryGet<jsg::Ref<Blob>>()) {
          parent = root.addRef();
        }
        return js.alloc<File>(kj::mv(parent), ptr, kj::mv(name), kj::mv(type), lastModified);
      }
    }
  }

  KJ_IF_SOME(data, concat(js, kj::mv(bits))) {
    return js.alloc<File>(js, data, kj::mv(name), kj::mv(type), lastModified);
  }
  return js.alloc<File>(kj::mv(name), kj::mv(type), lastModified);
}

}  // namespace workerd::api
