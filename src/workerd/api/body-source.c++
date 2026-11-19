// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "body-source.h"

#include <workerd/api/byte-chunker.h>
#include <workerd/api/encoding.h>
#include <workerd/api/streams/common.h>
#include <workerd/io/features.h>
#include <workerd/io/io-context.h>

namespace workerd::api {

namespace {

// The most a pump writes through tryWriteSync() while it holds the isolate lock once. It then
// releases the lock and takes it again, letting other work run on the isolate.
constexpr size_t kSyncWriteBudget = 8 * ByteChunker::kMaxChunkSize;

// What a pump does once a step under the isolate lock returns.
enum class PumpNext : uint8_t {
  // Write the chunk the sink did not take synchronously, then take the lock for the next step.
  WRITE,
  // Take the lock again for the next step: this one used up the synchronous-write budget.
  YIELD,
  // Every byte has been read from the source, which the pump no longer holds. Write the last
  // chunk, if the sink did not take it synchronously, then end the sink.
  FINISH,
};

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

// The state of a pumpTo(), shared by its steps under the isolate lock and the coroutine that
// writes asynchronously between them.
class BodySource::Pump final: public kj::Refcounted {
 public:
  Pump(jsg::Lock& js,
      BodySource source,
      kj::Own<WritableStreamSink> sink,
      TryWriteSync tryWriteSync);

  // Under the isolate lock: offers chunks to the sink until one is not taken synchronously, the
  // budget runs out, or the source has no more.
  PumpNext step(jsg::Lock& js);

  // Takes the lock for each step after the first, in `criticalSection`, writing between steps.
  static kj::Promise<void> run(IoContext& context,
      kj::Maybe<kj::Rc<InputGate::CriticalSection>> criticalSection,
      kj::Rc<Pump> pump,
      PumpNext next);

 private:
  // Holds a chunk the sink did not take synchronously, and a string's chunks as they are encoded.
  // Allocated when first needed, as long as the longest chunk.
  kj::Array<kj::byte> scratch;

  // The chunk in `scratch` to write asynchronously, if any.
  kj::ArrayPtr<const kj::byte> pending;

  kj::Own<WritableStreamSink> sink;

  // Dropped, under the lock, once every byte has been read from it.
  kj::Maybe<BodySource> source;

  // The bytes still to read from the source, and the size of the longest chunk.
  uint64_t remaining;
  size_t maxChunkSize;

  // Where the next chunk starts: in UTF-16 code units for a string, in bytes otherwise.
  size_t offset = 0;

  TryWriteSync tryWriteSync;
  LoneSurrogates loneSurrogates;

  // Reads the next chunk from the source: a view of a BufferSource's or Blob's memory, valid while
  // the lock is held, or a string's UTF-8 in `scratch`.
  kj::ArrayPtr<const kj::byte> nextChunk(jsg::Lock& js);

  kj::ArrayPtr<kj::byte> getScratch();

  kj::Promise<void> writePending();
};

BodySource::Pump::Pump(
    jsg::Lock& js, BodySource source, kj::Own<WritableStreamSink> sink, TryWriteSync tryWriteSync)
    : sink(kj::mv(sink)),
      remaining(KJ_ASSERT_NONNULL(source.tryGetLength(js, StreamEncoding::IDENTITY))),
      maxChunkSize(kj::min(remaining, ByteChunker::kMaxChunkSize)),
      tryWriteSync(tryWriteSync),
      loneSurrogates(stringBodyLoneSurrogates(js)) {
  if (remaining > 0) {
    this->source = kj::mv(source);
  }
}

PumpNext BodySource::Pump::step(jsg::Lock& js) {
  KJ_DASSERT(pending == nullptr);
  size_t writtenSync = 0;
  while (remaining > 0) {
    if (writtenSync >= kSyncWriteBudget) {
      return PumpNext::YIELD;
    }
    auto chunk = nextChunk(js);
    if (tryWriteSync == TryWriteSync::YES && sink->tryWriteSync(chunk)) {
      writtenSync += chunk.size();
      continue;
    }
    if (chunk.begin() == scratch.begin()) {
      pending = chunk;
    } else {
      // A view of the source, which the write cannot read once the lock is released.
      auto copy = getScratch().first(chunk.size());
      copy.copyFrom(chunk);
      pending = copy;
    }
    if (remaining > 0) {
      return PumpNext::WRITE;
    }
  }
  source = kj::none;
  return PumpNext::FINISH;
}

kj::ArrayPtr<const kj::byte> BodySource::Pump::nextChunk(jsg::Lock& js) {
  auto& current = KJ_ASSERT_NONNULL(source);
  size_t size = kj::min(remaining, maxChunkSize);
  KJ_IF_SOME(bytes, current.tryGetBytes(js)) {
    auto chunk = bytes.slice(offset).first(size);
    offset += size;
    remaining -= size;
    return chunk;
  }
  auto& text = KJ_ASSERT_NONNULL(current.value.tryGet<jsg::JsRef<jsg::JsString>>());
  auto output = getScratch().first(size);
  auto encoded = encodeUtf8Prefix(js, text.getHandle(js), offset, output.asChars(), loneSurrogates);
  // Any code point fits in four bytes, and a chunk shorter than that holds the rest of the string,
  // so every chunk has something in it.
  KJ_ASSERT(encoded.written > 0, "a string produced fewer bytes than its UTF-8 length");
  offset += encoded.read;
  remaining -= encoded.written;
  return output.first(encoded.written);
}

kj::ArrayPtr<kj::byte> BodySource::Pump::getScratch() {
  if (scratch == nullptr) {
    scratch = kj::heapArray<kj::byte>(maxChunkSize);
  }
  return scratch;
}

kj::Promise<void> BodySource::Pump::writePending() {
  auto bytes = pending;
  pending = nullptr;
  if (bytes.size() == 0) {
    return kj::READY_NOW;
  }
  return sink->write(bytes);
}

kj::Promise<void> BodySource::Pump::run(IoContext& context,
    kj::Maybe<kj::Rc<InputGate::CriticalSection>> criticalSection,
    kj::Rc<Pump> pump,
    PumpNext next) {
  while (next != PumpNext::FINISH) {
    if (next == PumpNext::WRITE) {
      co_await pump->writePending();
    }
    next = co_await context.run([pump = pump.addRef()](Worker::Lock& lock) mutable {
      return pump->step(lock);
    }, criticalSection.map([](kj::Rc<InputGate::CriticalSection>& section) {
      return section.addRef();
    }));
  }

  co_await pump->writePending();
  co_await pump->sink->end();
}

kj::Promise<void> BodySource::pumpTo(
    jsg::Lock& js, kj::Own<WritableStreamSink> sink, TryWriteSync tryWriteSync) {
  auto& context = IoContext::current();
  auto pump = kj::rc<Pump>(js, clone(js), kj::mv(sink), tryWriteSync);
  KJ_TRY {
    auto next = pump->step(js);
    return Pump::run(context, context.getCriticalSection(), kj::mv(pump), next);
  }
  KJ_CATCH(exception) {
    // tryWriteSync() throws when it could write synchronously but the write failed: fail as the
    // asynchronous write would have.
    return kj::mv(exception);
  }
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
