// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "blob.h"
#include "streams.h"
#include "util.h"
#include <workerd/util/mimetype.h>

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
  // TODO(soon): Add temporary logging if the type is not a valid mime type.
  // This does not properly parse mime types. We have the new workerd::MimeType impl
  // but that handles mime types a bit more strictly than this. We can/should switch
  // over to it but there's a non-zero risk of breaking running code. We might need
  // a compat flag to switch. To know for sure, we temporarily add logging here to
  // see if anyone in the wild is relying on the incorrect parsing.
  // If we see this log even once in production then we cannot switch normalizeType
  // for MimeType::tryParse without a compatibility flag.
  if (MimeType::tryParse(type) == nullptr) {
    LOG_WARNING_ONCE("Blob created with invalid/unparseable content type");
  }

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
    // Attempt to read a maximum of maxBytes from the remaining unread content of the blob
    // into the given buffer. It is the caller's responsibility to ensure that buffer has
    // enough capacity for at least maxBytes.
    // The minBytes argument is ignored in this implementation of tryRead.
    // The buffer must be kept alive by the caller until the returned promise is fulfilled.
    // The returned promise is fulfilled with the actual number of bytes read.
    size_t amount = kj::min(maxBytes, unread.size());
    if (amount > 0) {
      memcpy(buffer, unread.begin(), amount);
      unread = unread.slice(amount, unread.size());
    }
    return amount;
  }

  kj::Maybe<uint64_t> tryGetLength(StreamEncoding encoding) override {
    // Returns the number of bytes remaining to be read for the given encoding if that
    // encoding is supported. This implementation only supports StreamEncoding::IDENTITY.
    if (encoding == StreamEncoding::IDENTITY) {
      return unread.size();
    } else {
      return nullptr;
    }
  }

  kj::Promise<DeferredProxy<void>> pumpTo(WritableStreamSink& output, bool end) override {
    // Write all of the remaining unread content of the blob to output.
    // If end is true, output.end() will be called once the write has been completed.
    // Importantly, the WritableStreamSink must be kept alive by the caller until the
    // returned promise is fulfilled.
    if (unread.size() != 0) {
      auto promise = output.write(unread.begin(), unread.size());
      unread = nullptr;

      co_await promise;

      if (end) co_await output.end();
    }

    // We can't defer the write to the proxy stage since it depends on `blob` which lives in the
    // isolate.
    co_return newNoopDeferredProxy();
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
