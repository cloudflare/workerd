// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "blob.h"
#include "streams.h"
#include "util.h"

namespace workerd::api {

static kj::Array<byte> concat(jsg::Optional<Blob::Bits> maybeBits) {
  // Concatenate an array of segments (parameter to Blob constructor).
  //
  // TODO(perf): Make it so that a Blob can keep references to the input data rather than copy it.
  //   Note that we can't keep references to ArrayBuffers since they are mutable, but we can
  //   reference other Blobs in the input.

  auto bits = kj::mv(maybeBits).orDefault(nullptr);

  size_t size = 0;
  for (auto& part: bits) {
    KJ_SWITCH_ONEOF(part) {
      KJ_CASE_ONEOF(bytes, kj::Array<const byte>) {
        size += bytes.size();
      }
      KJ_CASE_ONEOF(text, kj::String) {
        size += text.size();
      }
      KJ_CASE_ONEOF(blob, jsg::Ref<Blob>) {
        size += blob->getData().size();
      }
    }
  }

  if (size == 0) return nullptr;

  auto result = kj::heapArray<byte>(size);
  byte* ptr = result.begin();

  for (auto& part: bits) {
    KJ_SWITCH_ONEOF(part) {
      KJ_CASE_ONEOF(bytes, kj::Array<const byte>) {
        memcpy(ptr, bytes.begin(), bytes.size());
        ptr += bytes.size();
      }
      KJ_CASE_ONEOF(text, kj::String) {
        memcpy(ptr, text.begin(), text.size());
        ptr += text.size();
      }
      KJ_CASE_ONEOF(blob, jsg::Ref<Blob>) {
        memcpy(ptr, blob->getData().begin(), blob->getData().size());
        ptr += blob->getData().size();
      }
    }
  }

  KJ_ASSERT(ptr == result.end());
  return result;
}

static kj::String normalizeType(kj::String type) {
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

jsg::Ref<Blob> Blob::constructor(jsg::Optional<Bits> bits, jsg::Optional<Options> options) {
  kj::String type;  // note: default value is intentionally empty string
  KJ_IF_MAYBE(o, options) {
    KJ_IF_MAYBE(t, o->type) {
      type = normalizeType(kj::mv(*t));
    }
  }

  return jsg::alloc<Blob>(concat(kj::mv(bits)), kj::mv(type));
}

jsg::Ref<Blob> Blob::slice(jsg::Optional<int> maybeStart, jsg::Optional<int> maybeEnd,
                            jsg::Optional<kj::String> type) {
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

  return jsg::alloc<Blob>(JSG_THIS, data.slice(start, end),
      normalizeType(kj::mv(type).orDefault(nullptr)));
}

jsg::Promise<kj::Array<kj::byte>> Blob::arrayBuffer(v8::Isolate* isolate) {
  // TODO(perf): Find a way to avoid the copy.
  return jsg::resolvedPromise(isolate, kj::heapArray<byte>(data));
}
jsg::Promise<kj::String> Blob::text(v8::Isolate* isolate) {
  return jsg::resolvedPromise(isolate, kj::str(data.asChars()));
}

class Blob::BlobInputStream final: public ReadableStreamSource {
public:
  BlobInputStream(jsg::Ref<Blob> blob)
      : unread(blob->data),
        blob(kj::mv(blob)) {}

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    size_t amount = kj::min(maxBytes, unread.size());
    memcpy(buffer, unread.begin(), amount);
    unread = unread.slice(amount, unread.size());
    return amount;
  }

  kj::Maybe<uint64_t> tryGetLength(StreamEncoding encoding) override {
    if (encoding == StreamEncoding::IDENTITY) {
      return unread.size();
    } else {
      return nullptr;
    }
  }

  kj::Promise<DeferredProxy<void>> pumpTo(WritableStreamSink& output, bool end) override {
    if (unread.size() == 0) {
      return addNoopDeferredProxy(kj::READY_NOW);
    }

    auto promise = output.write(unread.begin(), unread.size());
    unread = nullptr;

    if (end) {
      promise = promise.then([&output]() { return output.end(); });
    }

    // We can't defer the write to the proxy stage since it depends on `blob` which lives in the
    // isolate.
    return addNoopDeferredProxy(kj::mv(promise));
  }

private:
  kj::ArrayPtr<const byte> unread;
  jsg::Ref<Blob> blob;
};

jsg::Ref<ReadableStream> Blob::stream(v8::Isolate* isolate) {
  return jsg::alloc<ReadableStream>(
      IoContext::current(),
      kj::heap<BlobInputStream>(JSG_THIS));
}

// =======================================================================================

jsg::Ref<File> File::constructor(jsg::Optional<Bits> bits,
    kj::String name, jsg::Optional<Options> options) {
  kj::String type;  // note: default value is intentionally empty string
  kj::Maybe<double> maybeLastModified;
  KJ_IF_MAYBE(o, options) {
    KJ_IF_MAYBE(t, o->type) {
      type = normalizeType(kj::mv(*t));
    }
    maybeLastModified = o->lastModified;
  }

  double lastModified;
  KJ_IF_MAYBE(m, maybeLastModified) {
    lastModified = *m;
  } else {
    lastModified = dateNow();
  }

  return jsg::alloc<File>(concat(kj::mv(bits)), kj::mv(name), kj::mv(type), lastModified);
}

}  // namespace workerd::api
