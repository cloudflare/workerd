// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "blob.h"

#include "util.h"

#include <workerd/api/streams/readable.h>
#include <workerd/io/observer.h>
#include <workerd/util/mimetype.h>

namespace workerd::api {

namespace {
// Concatenate an array of segments (parameter to Blob constructor).
kj::Array<byte> concat(jsg::Lock& js, jsg::Optional<Blob::Bits> maybeBits) {
  // TODO(perf): Make it so that a Blob can keep references to the input data rather than copy it.
  //   Note that we can't keep references to ArrayBuffers since they are mutable, but we can
  //   reference other Blobs in the input.

  auto bits = kj::mv(maybeBits).orDefault(nullptr);

  auto maxBlobSize = Worker::Isolate::from(js).getLimitEnforcer().getBlobSizeLimit();
  size_t size = 0;
  for (auto& part: bits) {
    size_t partSize = 0;
    KJ_SWITCH_ONEOF(part) {
      KJ_CASE_ONEOF(bytes, kj::Array<const byte>) {
        partSize = bytes.size();
      }
      KJ_CASE_ONEOF(text, kj::String) {
        partSize = text.size();
      }
      KJ_CASE_ONEOF(blob, jsg::Ref<Blob>) {
        partSize = blob->getData().size();
      }
    }

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
    if (size + partSize > maxBlobSize) {
      // TODO(soon): This logging is just to help us determine further how common
      // this case is. We can and should remove the logging once we have enough data.
      LOG_WARNING_PERIODICALLY(
          kj::str("NOSENTRY Attempt to create a Blob with size ", size + partSize));
    }
    JSG_REQUIRE(size + partSize <= maxBlobSize, RangeError,
        kj::str("Blob size ", size + partSize, " exceeds limit ", maxBlobSize));
    size += partSize;
  }

  if (size == 0) return nullptr;

  auto result = kj::heapArray<byte>(size);
  auto view = result.asPtr();

  for (auto& part: bits) {
    KJ_SWITCH_ONEOF(part) {
      KJ_CASE_ONEOF(bytes, kj::Array<const byte>) {
        if (bytes.size() == 0) continue;
        KJ_ASSERT(view.size() >= bytes.size());
        view.first(bytes.size()).copyFrom(bytes);
        view = view.slice(bytes.size());
      }
      KJ_CASE_ONEOF(text, kj::String) {
        auto byteLength = text.asBytes().size();
        if (byteLength == 0) continue;
        KJ_ASSERT(view.size() >= byteLength);
        view.first(byteLength).copyFrom(text.asBytes());
        view = view.slice(byteLength);
      }
      KJ_CASE_ONEOF(blob, jsg::Ref<Blob>) {
        auto data = blob->getData();
        if (data.size() == 0) continue;
        KJ_ASSERT(view.size() >= data.size());
        view.first(data.size()).copyFrom(data);
        view = view.slice(data.size());
      }
    }
  }

  KJ_ASSERT(view == nullptr);

  return result;
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

jsg::BufferSource wrap(jsg::Lock& js, kj::Array<byte> data) {
  auto buf = JSG_REQUIRE_NONNULL(jsg::BufferSource::tryAlloc(js, data.size()), Error,
      "Unable to allocate space for Blob data");
  buf.asArrayPtr().copyFrom(data);
  return kj::mv(buf);

  // TODO(perf): Ideally we could just wrap the data like this, in which
  // the underlying v8::BackingStore is supposed to free the buffer when
  // it is done with it. Unfortunately ASAN complains about a leak that
  // will require more investigation.
  // return jsg::BufferSource(js, jsg::BackingStore::from(kj::mv(data)));
}

kj::ArrayPtr<const kj::byte> getPtr(jsg::BufferSource& source) {
  return source.asArrayPtr();
}
}  // namespace

Blob::Blob(kj::Array<byte> data, kj::String type)
    : ownData(kj::mv(data)),
      data(ownData.get<kj::Array<kj::byte>>()),
      type(kj::mv(type)) {}

Blob::Blob(jsg::Lock& js, jsg::BufferSource data, kj::String type)
    : ownData(kj::mv(data)),
      data(getPtr(ownData.get<jsg::BufferSource>())),
      type(kj::mv(type)) {}

Blob::Blob(jsg::Lock& js, kj::Array<byte> data, kj::String type)
    : ownData(wrap(js, kj::mv(data))),
      data(getPtr(ownData.get<jsg::BufferSource>())),
      type(kj::mv(type)) {}

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

  return jsg::alloc<Blob>(js, concat(js, kj::mv(bits)), kj::mv(type));
}

kj::ArrayPtr<const byte> Blob::getData() const {
  FeatureObserver::maybeRecordUse(FeatureObserver::Feature::BLOB_GET_DATA);
  return data;
}

jsg::Ref<Blob> Blob::slice(
    jsg::Optional<int> maybeStart, jsg::Optional<int> maybeEnd, jsg::Optional<kj::String> type) {
  int start = maybeStart.orDefault(0);
  int end = maybeEnd.orDefault(data.size());

  if (start < 0) {
    // Negative value interpreted as offset from end.
    start += data.size();
  }
  // Clamp start to range.
  if (start < 0) {
    start = 0;
  } else if (start > data.size()) {
    start = data.size();
  }

  if (end < 0) {
    // Negative value interpreted as offset from end.
    end += data.size();
  }
  // Clamp end to range.
  if (end < start) {
    end = start;
  } else if (end > data.size()) {
    end = data.size();
  }

  return jsg::alloc<Blob>(
      JSG_THIS, data.slice(start, end), normalizeType(kj::mv(type).orDefault(nullptr)));
}

jsg::Promise<jsg::BufferSource> Blob::arrayBuffer(jsg::Lock& js) {
  FeatureObserver::maybeRecordUse(FeatureObserver::Feature::BLOB_AS_ARRAY_BUFFER);
  // We use BufferSource here instead of kj::Array<kj::byte> to ensure that the
  // resulting backing store is associated with the isolate, which is necessary
  // for when we start making use of v8 sandboxing.
  auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, data.size());
  backing.asArrayPtr().copyFrom(data);
  return js.resolvedPromise(jsg::BufferSource(js, kj::mv(backing)));
}

jsg::Promise<jsg::BufferSource> Blob::bytes(jsg::Lock& js) {
  // We use BufferSource here instead of kj::Array<kj::byte> to ensure that the
  // resulting backing store is associated with the isolate, which is necessary
  // for when we start making use of v8 sandboxing.
  auto backing = jsg::BackingStore::alloc<v8::Uint8Array>(js, data.size());
  backing.asArrayPtr().copyFrom(data);
  return js.resolvedPromise(jsg::BufferSource(js, kj::mv(backing)));
}

jsg::Promise<kj::String> Blob::text(jsg::Lock& js) {
  FeatureObserver::maybeRecordUse(FeatureObserver::Feature::BLOB_AS_TEXT);
  return js.resolvedPromise(kj::str(data.asChars()));
}

class Blob::BlobInputStream final: public ReadableStreamSource {
public:
  BlobInputStream(jsg::Ref<Blob> blob): unread(blob->data), blob(kj::mv(blob)) {}

  // Attempt to read a maximum of maxBytes from the remaining unread content of the blob
  // into the given buffer. It is the caller's responsibility to ensure that buffer has
  // enough capacity for at least maxBytes.
  // The minBytes argument is ignored in this implementation of tryRead.
  // The buffer must be kept alive by the caller until the returned promise is fulfilled.
  // The returned promise is fulfilled with the actual number of bytes read.
  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    size_t amount = kj::min(maxBytes, unread.size());
    if (amount > 0) {
      memcpy(buffer, unread.begin(), amount);
      unread = unread.slice(amount, unread.size());
    }
    return amount;
  }

  // Returns the number of bytes remaining to be read for the given encoding if that
  // encoding is supported. This implementation only supports StreamEncoding::IDENTITY.
  kj::Maybe<uint64_t> tryGetLength(StreamEncoding encoding) override {
    if (encoding == StreamEncoding::IDENTITY) {
      return unread.size();
    } else {
      return kj::none;
    }
  }

  // Write all of the remaining unread content of the blob to output.
  // If end is true, output.end() will be called once the write has been completed.
  // Importantly, the WritableStreamSink must be kept alive by the caller until the
  // returned promise is fulfilled.
  kj::Promise<DeferredProxy<void>> pumpTo(WritableStreamSink& output, bool end) override {
    if (unread.size() != 0) {
      auto promise = output.write(unread);
      unread = nullptr;

      co_await promise;

      if (end) co_await output.end();
    }

    // We can't defer the write to the proxy stage since it depends on `blob` which lives in the
    // isolate, so we don't `KJ_CO_MAGIC BEGIN_DEFERRED_PROXYING`.
    co_return;
  }

private:
  kj::ArrayPtr<const byte> unread;
  jsg::Ref<Blob> blob;
};

jsg::Ref<ReadableStream> Blob::stream() {
  FeatureObserver::maybeRecordUse(FeatureObserver::Feature::BLOB_AS_STREAM);
  return jsg::alloc<ReadableStream>(IoContext::current(), kj::heap<BlobInputStream>(JSG_THIS));
}

// =======================================================================================

File::File(kj::Array<byte> data, kj::String name, kj::String type, double lastModified)
    : Blob(kj::mv(data), kj::mv(type)),
      name(kj::mv(name)),
      lastModified(lastModified) {}

File::File(
    jsg::Lock& js, kj::Array<byte> data, kj::String name, kj::String type, double lastModified)
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

  return jsg::alloc<File>(js, concat(js, kj::mv(bits)), kj::mv(name), kj::mv(type), lastModified);
}

}  // namespace workerd::api
