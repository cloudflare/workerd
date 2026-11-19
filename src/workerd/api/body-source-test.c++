// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/body-source.h>
#include <workerd/api/byte-chunker.h>
#include <workerd/api/form-data.h>
#include <workerd/api/http.h>
#include <workerd/api/url-standard.h>
#include <workerd/api/url.h>
#include <workerd/io/compatibility-date.capnp.h>
#include <workerd/tests/test-fixture.h>

#include <capnp/message.h>
#include <kj/test.h>

namespace workerd::api {
namespace {

constexpr uint64_t kLimit = 1024 * 1024;

// "héllo 😀": 1 + 2 + 3 + 1 + 4 bytes of UTF-8 around the ASCII.
constexpr kj::StringPtr kText = "h\xc3\xa9llo \xf0\x9f\x98\x80"_kj;

TestFixture makeFixture(bool typescript) {
  if (!typescript) {
    return TestFixture();
  }
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setTypeScriptImplementedStreams(true);
  return TestFixture({.featureFlags = flags.asReader()});
}

// One source of each kind, all holding kText.
kj::Vector<BodySource> makeSources(jsg::Lock& js) {
  kj::Vector<BodySource> sources;
  sources.add(BodySource(js, kText.asBytes()));
  sources.add(BodySource(js, js.str(kText)));
  sources.add(BodySource(js, jsg::JsBufferSource(jsg::JsUint8Array::create(js, kText.asBytes()))));
  sources.add(BodySource(js.alloc<Blob>(
      js, jsg::JsBufferSource(jsg::JsUint8Array::create(js, kText.asBytes())), kj::str())));
  return sources;
}

KJ_TEST("BodySource reports the UTF-8 length of every kind of source") {
  TestFixture fixture;
  fixture.runInIoContext([](const TestFixture::Environment& env) {
    auto& js = env.js;
    for (auto& source: makeSources(js)) {
      KJ_EXPECT(source.tryGetLength(js, StreamEncoding::IDENTITY) == kText.size());
      KJ_EXPECT(source.tryGetLength(js, StreamEncoding::GZIP) == kj::none);
      KJ_EXPECT(source.clone(js).tryGetLength(js, StreamEncoding::IDENTITY) == kText.size());
    }
  });
}

KJ_TEST("BodySource counts a lone surrogate as the three bytes of U+FFFD") {
  TestFixture fixture;
  fixture.runInIoContext([](const TestFixture::Environment& env) {
    auto& js = env.js;
    const char16_t units[] = {u'a', 0xd800, u'b'};
    BodySource source(js, js.str(kj::ArrayPtr<const char16_t>(units, kj::size(units))));
    KJ_EXPECT(source.tryGetLength(js, StreamEncoding::IDENTITY) == uint64_t{5});
  });
}

KJ_TEST("BodySource creates a new stream over the whole source each time") {
  for (bool typescript: {false, true}) {
    auto fixture = makeFixture(typescript);
    fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
      auto& js = env.js;
      kj::Vector<jsg::Promise<void>> reads;
      for (auto& source: makeSources(js)) {
        auto first = source.createStream(js);
        auto second = source.clone(js).createStream(js);
        auto promise = first.text(js, kLimit)
                           .then(js,
                             JSG_VISITABLE_LAMBDA((source = kj::mv(source), typescript), (source),
                                 (jsg::Lock& js, kj::String text) {
                                   KJ_EXPECT(text == kText, typescript);
                                   // Reading one stream leaves the source whole for the next.
                                   return source.createStream(js).text(js, kLimit);
                                 }))
                           .then(js,
                             JSG_VISITABLE_LAMBDA((second = kj::mv(second), typescript), (second),
                                 (jsg::Lock& js, kj::String text) {
                                   KJ_EXPECT(text == kText, typescript);
                                   return second.text(js, kLimit);
                                 }))
                           .then(js, [typescript](jsg::Lock& js, kj::String text) {
          KJ_EXPECT(text == kText, typescript);
        });
        reads.add(kj::mv(promise));
      }
      kj::Vector<kj::Promise<void>> awaited;
      for (auto& read: reads) {
        awaited.add(env.context.awaitJs(js, kj::mv(read)));
      }
      return kj::joinPromises(awaited.releaseAsArray());
    });
  }
}

KJ_TEST("A FormData or URLSearchParams body is held in memory the isolate accounts for") {
  TestFixture fixture;
  fixture.runInIoContext([](const TestFixture::Environment& env) {
    auto& js = env.js;
    constexpr size_t kValueSize = 64 * 1024;
    auto value = kj::str(kj::repeat('a', kValueSize));
    auto formData = FormData::constructor(js);
    formData->append(js, kj::str("a"), kj::str(value), kj::none);
    auto legacyParams =
        URLSearchParams::constructor(js, URLSearchParams::Initializer(kj::str("a=", value)));
    auto params = url::URLSearchParams::constructor(
        js, url::URLSearchParams::Initializer(jsg::USVString(kj::str("a=", value))));
    // Each is passed by a new reference, so that the value, which the legacy URLSearchParams
    // accounts for, lives on through the measurement.
    auto initializers =
        kj::arr<Body::Initializer>(formData.addRef(), legacyParams.addRef(), params.addRef());
    for (auto i: kj::indices(initializers)) {
      // External memory counts an ArrayBuffer's backing store, and does not count native memory.
      int64_t before = js.v8Isolate->GetExternalMemory();
      auto body = Body::extractBody(js, kj::mv(initializers[i]));
      int64_t held = js.v8Isolate->GetExternalMemory() - before;
      KJ_EXPECT(held >= kValueSize, held, i);
    }
  });
}

// ---------------------------------------------------------------------------------------------
// BodySource::pumpTo()

constexpr size_t kChunk = ByteChunker::kMaxChunkSize;

// The most pumpTo() writes synchronously while it holds the isolate lock once (kSyncWriteBudget in
// body-source.c++).
constexpr size_t kSyncWriteBudget = 8 * kChunk;

// How a TestSink takes writes.
enum class SinkMode {
  // Takes every chunk through tryWriteSync().
  SYNC,
  // Refuses tryWriteSync(), and completes each write() at once.
  ASYNC,
  // Refuses tryWriteSync(), and completes each write() when the test fulfills it.
  STALL,
  // Throws from tryWriteSync(), as a sink does whose synchronous write fails.
  THROW,
  // Refuses tryWriteSync(), and fails each write().
  REJECT,
};

// What a TestSink was given.
struct SinkLog {
  struct Write {
    size_t size;
    bool sync;
    // Whether the sink took the bytes from where the source keeps them.
    bool fromSource;
  };
  kj::Vector<kj::byte> data;
  kj::Vector<Write> writes;
  uint syncOffers = 0;
  bool ended = false;
  // For a STALL sink, one per write, which the test fulfills to complete it.
  kj::Vector<kj::Own<kj::PromiseFulfiller<void>>> stalled;
  kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> onWrite;

  // Resolves at the next write.
  kj::Promise<void> nextWrite() {
    auto paf = kj::newPromiseAndFulfiller<void>();
    onWrite = kj::mv(paf.fulfiller);
    return kj::mv(paf.promise);
  }

  void record(kj::ArrayPtr<const kj::byte> buffer, bool sync, kj::ArrayPtr<const kj::byte> source) {
    data.addAll(buffer);
    auto address = [](const kj::byte* p) { return reinterpret_cast<uintptr_t>(p); };
    bool fromSource = source != nullptr && address(source.begin()) <= address(buffer.begin()) &&
        address(buffer.end()) <= address(source.end());
    writes.add(Write{.size = buffer.size(), .sync = sync, .fromSource = fromSource});
    KJ_IF_SOME(fulfiller, onWrite) {
      fulfiller->fulfill();
      onWrite = kj::none;
    }
  }

  // True if every write was taken as `sync` says, and from the source's memory as `fromSource`
  // says.
  bool allWrites(bool sync, bool fromSource) const {
    for (auto& write: writes) {
      if (write.sync != sync || write.fromSource != fromSource) {
        return false;
      }
    }
    return true;
  }

  bool chunksAtMost(size_t size) const {
    for (auto& write: writes) {
      if (write.size == 0 || write.size > size) {
        return false;
      }
    }
    return true;
  }
};

class TestSink final: public WritableStreamSink {
 public:
  // `source` is where the source keeps its bytes, if it is a BufferSource or Blob.
  TestSink(SinkLog& log, SinkMode mode, kj::ArrayPtr<const kj::byte> source = nullptr)
      : log(log),
        mode(mode),
        source(source) {}

  bool tryWriteSync(kj::ArrayPtr<const kj::byte> buffer) override {
    ++log.syncOffers;
    switch (mode) {
      case SinkMode::SYNC:
        log.record(buffer, true, source);
        return true;
      case SinkMode::THROW:
        kj::throwFatalException(KJ_EXCEPTION(FAILED, "synchronous write failed"));
      case SinkMode::ASYNC:
      case SinkMode::STALL:
      case SinkMode::REJECT:
        return false;
    }
    KJ_UNREACHABLE;
  }

  bool tryWriteSync(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    KJ_FAIL_REQUIRE("pumpTo() writes one chunk at a time");
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::byte> buffer) override {
    log.record(buffer, false, source);
    switch (mode) {
      case SinkMode::STALL: {
        auto paf = kj::newPromiseAndFulfiller<void>();
        log.stalled.add(kj::mv(paf.fulfiller));
        return kj::mv(paf.promise);
      }
      case SinkMode::REJECT:
        return KJ_EXCEPTION(FAILED, "write failed");
      case SinkMode::SYNC:
      case SinkMode::ASYNC:
      case SinkMode::THROW:
        return kj::READY_NOW;
    }
    KJ_UNREACHABLE;
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    KJ_FAIL_REQUIRE("pumpTo() writes one chunk at a time");
  }

  kj::Promise<void> end() override {
    log.ended = true;
    return kj::READY_NOW;
  }

  void abort(kj::Exception reason) override {}

 private:
  SinkLog& log;
  SinkMode mode;
  kj::ArrayPtr<const kj::byte> source;
};

enum class Kind { UINT8_ARRAY, ARRAY_BUFFER, BLOB, STRING };
constexpr Kind kKinds[] = {Kind::UINT8_ARRAY, Kind::ARRAY_BUFFER, Kind::BLOB, Kind::STRING};

kj::StringPtr KJ_STRINGIFY(Kind kind) {
  switch (kind) {
    case Kind::UINT8_ARRAY:
      return "Uint8Array"_kj;
    case Kind::ARRAY_BUFFER:
      return "ArrayBuffer"_kj;
    case Kind::BLOB:
      return "Blob"_kj;
    case Kind::STRING:
      return "string"_kj;
  }
  KJ_UNREACHABLE;
}

// A source of `kind` holding `bytes`, which a string source decodes as UTF-8. Sets `memory` to
// where a BufferSource or Blob source keeps its bytes.
BodySource makeSource(jsg::Lock& js,
    Kind kind,
    kj::ArrayPtr<const kj::byte> bytes,
    kj::ArrayPtr<const kj::byte>& memory) {
  switch (kind) {
    case Kind::UINT8_ARRAY: {
      auto array = jsg::JsUint8Array::create(js, bytes);
      memory = array.asArrayPtr();
      return BodySource(js, jsg::JsBufferSource(array));
    }
    case Kind::ARRAY_BUFFER: {
      auto buffer = jsg::JsArrayBuffer::create(js, bytes);
      memory = buffer.asArrayPtr();
      return BodySource(js, jsg::JsBufferSource(buffer));
    }
    case Kind::BLOB: {
      auto blob =
          js.alloc<Blob>(js, jsg::JsBufferSource(jsg::JsUint8Array::create(js, bytes)), kj::str());
      memory = blob->getData(js);
      return BodySource(kj::mv(blob));
    }
    case Kind::STRING:
      memory = nullptr;
      return BodySource(js, js.str(bytes.asChars()));
  }
  KJ_UNREACHABLE;
}

// `size` bytes that differ from one chunk to the next.
kj::Array<kj::byte> patternBytes(size_t size) {
  auto bytes = kj::heapArray<kj::byte>(size);
  for (auto i: kj::indices(bytes)) {
    bytes[i] = static_cast<kj::byte>(i % 251);
  }
  return bytes;
}

// At least `size` bytes of UTF-8 for a string source: three-byte code points, so that a chunk of
// kChunk bytes would end inside one.
kj::Array<kj::byte> euroBytes(size_t size) {
  kj::Vector<kj::byte> bytes;
  while (bytes.size() < size) {
    bytes.addAll("\xe2\x82\xac"_kj.asBytes());
  }
  return bytes.releaseAsArray();
}

// Resolves once a body's pump has written and ended the sink, deferred proxying included.
kj::Promise<void> finishPump(kj::Promise<DeferredProxy<void>> pump) {
  return pump.then([](DeferredProxy<void> deferred) { return kj::mv(deferred.proxyTask); });
}

KJ_TEST("BodySource::pumpTo writes every kind of source in chunks, then ends the sink") {
  struct Case {
    Kind kind;
    size_t size;
    kj::Array<kj::byte> bytes;
    SinkLog log;
  };
  constexpr size_t kSizes[] = {0, 1, kChunk, kChunk + 1, 3 * kChunk + 7};
  for (auto mode: {SinkMode::SYNC, SinkMode::ASYNC}) {
    kj::Vector<kj::Own<Case>> cases;
    for (auto kind: kKinds) {
      for (auto size: kSizes) {
        auto bytes = kind == Kind::STRING ? euroBytes(size) : patternBytes(size);
        cases.add(kj::heap<Case>(Case{.kind = kind, .size = size, .bytes = kj::mv(bytes)}));
      }
    }
    TestFixture fixture;
    fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
      auto& js = env.js;
      kj::Vector<kj::Promise<void>> pumps;
      for (auto& c: cases) {
        kj::ArrayPtr<const kj::byte> memory;
        auto source = makeSource(js, c->kind, c->bytes, memory);
        auto sink = kj::heap<TestSink>(c->log, mode, memory);
        pumps.add(source.pumpTo(js, kj::mv(sink), TryWriteSync::YES));
      }
      return kj::joinPromises(pumps.releaseAsArray());
    });
    for (auto& c: cases) {
      auto& log = c->log;
      bool sync = mode == SinkMode::SYNC;
      KJ_EXPECT(log.data.asPtr() == c->bytes.asPtr(), c->kind, c->size, sync);
      KJ_EXPECT(log.ended, c->kind, c->size, sync);
      KJ_EXPECT(log.chunksAtMost(kChunk), c->kind, c->size, sync);
      // A synchronous write takes a BufferSource's or Blob's chunk straight from its memory. An
      // asynchronous one, which runs with the lock released, takes a copy.
      bool fromSource = sync && c->kind != Kind::STRING;
      KJ_EXPECT(log.allWrites(sync, fromSource), c->kind, c->size, sync);
      if (c->kind == Kind::STRING) {
        // Each chunk of a string starts on a code point boundary.
        size_t offset = 0;
        for (auto& write: log.writes) {
          KJ_EXPECT((log.data[offset] & 0xc0) != 0x80, offset, c->size, sync);
          offset += write.size;
        }
      } else {
        KJ_EXPECT(log.writes.size() == (c->size + kChunk - 1) / kChunk, c->kind, c->size, sync);
      }
    }
  }
}

KJ_TEST("BodySource::pumpTo offers no chunk synchronously under TryWriteSync::NO") {
  for (auto kind: kKinds) {
    SinkLog log;
    auto bytes = patternBytes(2 * kChunk + 1);
    if (kind == Kind::STRING) {
      bytes = euroBytes(2 * kChunk + 1);
    }
    TestFixture fixture;
    fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
      auto& js = env.js;
      kj::ArrayPtr<const kj::byte> memory;
      auto source = makeSource(js, kind, bytes, memory);
      auto sink = kj::heap<TestSink>(log, SinkMode::SYNC, memory);
      return source.pumpTo(js, kj::mv(sink), TryWriteSync::NO);
    });
    KJ_EXPECT(log.syncOffers == 0, kind);
    KJ_EXPECT(log.allWrites(false, false), kind);
    KJ_EXPECT(log.data.asPtr() == bytes.asPtr(), kind);
    KJ_EXPECT(log.ended, kind);
  }
}

KJ_TEST("BodySource::pumpTo releases the isolate lock after each 1 MiB it writes synchronously") {
  constexpr size_t kSize = 3 * kSyncWriteBudget + 1;
  for (auto kind: {Kind::UINT8_ARRAY, Kind::STRING}) {
    SinkLog log;
    // ASCII, so that a string's chunks are as long as a buffer's.
    auto bytes = kj::heapArray<kj::byte>(kSize);
    bytes.asPtr().fill('a');
    TestFixture fixture;
    fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
      auto& js = env.js;
      kj::ArrayPtr<const kj::byte> memory;
      auto source = makeSource(js, kind, bytes, memory);
      auto pump =
          source.pumpTo(js, kj::heap<TestSink>(log, SinkMode::SYNC, memory), TryWriteSync::YES);
      // The sink would take the whole body synchronously, but pumpTo() returns, releasing the
      // lock, once it has written the budget. Later steps take the lock again for the rest.
      KJ_EXPECT(log.data.size() == kSyncWriteBudget, log.data.size(), kind);
      return kj::mv(pump);
    });
    KJ_EXPECT(log.data.size() == kSize, kind);
    KJ_EXPECT(log.allWrites(true, kind != Kind::STRING), kind);
    KJ_EXPECT(log.ended, kind);
  }
}

KJ_TEST("BodySource::pumpTo fails with the exception of a write that fails") {
  // A synchronous write that fails rejects the pump, as an asynchronous one does, rather than
  // throwing from pumpTo().
  for (auto mode: {SinkMode::THROW, SinkMode::REJECT}) {
    SinkLog log;
    auto bytes = euroBytes(kChunk + 1);
    TestFixture fixture;
    fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
      auto& js = env.js;
      kj::ArrayPtr<const kj::byte> memory;
      auto source = makeSource(js, Kind::STRING, bytes, memory);
      auto pump = source.pumpTo(js, kj::heap<TestSink>(log, mode), TryWriteSync::YES);
      return pump.then(
          []() { KJ_FAIL_EXPECT("the pump should fail"); }, [mode](kj::Exception&& exception) {
        auto expected = mode == SinkMode::THROW ? "synchronous write failed"_kj : "write failed"_kj;
        KJ_EXPECT(exception.getDescription() == expected, exception);
      });
    });
    KJ_EXPECT(!log.ended);
  }
}

kj::Promise<void> expectNoMoreWrites(kj::Promise<void> pump, SinkLog& log) {
  auto release = kj::mv(log.stalled[0]);
  pump = nullptr;
  release->fulfill();
  co_await kj::evalLast([] {});
  KJ_EXPECT(log.writes.size() == 1);
  KJ_EXPECT(!log.ended);
}

KJ_TEST("BodySource::pumpTo stops once it is dropped") {
  SinkLog log;
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;
    auto bytes = patternBytes(2 * kChunk + 1);
    kj::ArrayPtr<const kj::byte> memory;
    auto source = makeSource(js, Kind::UINT8_ARRAY, bytes, memory);
    auto pump = source.pumpTo(js, kj::heap<TestSink>(log, SinkMode::STALL), TryWriteSync::YES);
    KJ_EXPECT(log.stalled.size() == 1);
    return expectNoMoreWrites(kj::mv(pump), log);
  });
}

KJ_TEST("BodySource::pumpTo writes a lone surrogate as string_body_replace_surrogates says") {
  for (bool replace: {false, true}) {
    capnp::MallocMessageBuilder message;
    auto flags = message.initRoot<CompatibilityFlags>();
    flags.setStringBodyReplaceSurrogates(replace);
    TestFixture fixture({.featureFlags = flags.asReader()});
    SinkLog log;
    fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
      auto& js = env.js;
      const char16_t units[] = {u'a', 0xd800, u'b'};
      BodySource source(js, js.str(kj::ArrayPtr<const char16_t>(units, kj::size(units))));
      return source.pumpTo(js, kj::heap<TestSink>(log, SinkMode::SYNC), TryWriteSync::YES);
    });
    // U+FFFD, or the three bytes UTF-8 would give U+D800 (WTF-8).
    auto expected = replace ? "a\xef\xbf\xbd"
                              "b"_kj
                            : "a\xed\xa0\x80"
                              "b"_kj;
    KJ_EXPECT(log.data.asPtr().asConst() == expected.asBytes(), replace);
  }
}

// ---------------------------------------------------------------------------------------------
// Body::tryDirectPump() and Response::send()

jsg::Ref<Response> makeResponse(jsg::Lock& js, kj::Maybe<Body::Initializer> body) {
  return Response::constructor(js, kj::mv(body), kj::none);
}

Body::Initializer stringBody(jsg::Lock& js, kj::StringPtr text) {
  return jsg::JsValue(js.str(text)).addRef(js);
}

KJ_TEST("Body::tryDirectPump sends a body that has neither created its stream nor been read") {
  SinkLog sent;
  SinkLog resent;
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;
    kj::Vector<kj::Promise<void>> pumps;

    auto response = makeResponse(js, stringBody(js, kText));
    kj::Own<WritableStreamSink> sink = kj::heap<TestSink>(sent, SinkMode::SYNC);
    pumps.add(finishPump(KJ_ASSERT_NONNULL(response->tryDirectPump(js, sink, TryWriteSync::YES))));
    KJ_EXPECT(sink.get() == nullptr);
    // Used, as pumping its stream would have left it.
    KJ_EXPECT(response->getBodyUsed(js));
    auto stream = KJ_ASSERT_NONNULL(response->getBody(js));
    KJ_EXPECT(stream.isDisturbed(js));
    KJ_EXPECT(stream.isLocked(js));

    // A rewound body, as for a redirect, is sent from its source again.
    sink = kj::heap<TestSink>(resent, SinkMode::SYNC);
    KJ_EXPECT(response->tryDirectPump(js, sink, TryWriteSync::YES) == kj::none);
    KJ_EXPECT(sink.get() != nullptr);
    response->rewindBody();
    pumps.add(finishPump(KJ_ASSERT_NONNULL(response->tryDirectPump(js, sink, TryWriteSync::YES))));

    // The body's stream is pumped instead: a body that has no source, or has created its stream,
    // or has been read. A null body has nothing to send.
    auto streamed =
        makeResponse(js, Body::Initializer(BodySource(js, js.str(kText)).createStream(js)));
    auto created = makeResponse(js, stringBody(js, kText));
    KJ_UNUSED auto createdStream = created->getBody(js);
    auto read = makeResponse(js, stringBody(js, kText));
    KJ_UNUSED auto text = read->text(js);
    auto null = makeResponse(js, kj::none);
    for (auto* body: {streamed.get(), created.get(), read.get(), null.get()}) {
      SinkLog unused;
      kj::Own<WritableStreamSink> kept = kj::heap<TestSink>(unused, SinkMode::SYNC);
      KJ_EXPECT(body->tryDirectPump(js, kept, TryWriteSync::YES) == kj::none);
      KJ_EXPECT(kept.get() != nullptr);
    }
    return kj::joinPromises(pumps.releaseAsArray());
  });
  KJ_EXPECT(sent.data.asPtr().asConst() == kText.asBytes());
  KJ_EXPECT(resent.data.asPtr().asConst() == kText.asBytes());
}

kj::Promise<void> expectProxyingOnceEnded(
    kj::Promise<void> pump, SinkLog& log, const bool& proxying) {
  KJ_EXPECT(log.stalled.size() == 1);
  auto lastWrite = log.nextWrite();
  log.stalled[0]->fulfill();
  co_await lastWrite;
  co_await kj::evalLast([] {});
  // The last chunk has been read from the source, and its write is pending.
  KJ_EXPECT(!proxying);

  log.stalled[1]->fulfill();
  co_await pump;
  KJ_EXPECT(proxying);
}

KJ_TEST("Body::tryDirectPump begins deferred proxying only once it has ended the sink") {
  // A sink's writes may need the IoContext, as a system stream's do, so no write may outlive it.
  SinkLog log;
  bool proxying = false;
  auto text = kj::str(kj::repeat('x', kChunk + 1));
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;
    auto response = makeResponse(js, stringBody(js, text));
    kj::Own<WritableStreamSink> sink = kj::heap<TestSink>(log, SinkMode::STALL);
    auto pump = KJ_ASSERT_NONNULL(response->tryDirectPump(js, sink, TryWriteSync::YES))
                    .then([&log, &proxying](DeferredProxy<void> deferred) {
      KJ_EXPECT(log.ended);
      proxying = true;
      return kj::mv(deferred.proxyTask);
    }).eagerlyEvaluate(nullptr);
    return expectProxyingOnceEnded(kj::mv(pump), log, proxying);
  });
  KJ_EXPECT(log.data.asPtr().asConst() == text.asBytes());
}

jsg::Ref<Request> makePost(jsg::Lock& js, kj::StringPtr text) {
  return Request::constructor(js, kj::str("https://example.com/"),
      Request::Initializer(Request::InitializerDict{
        .method = kj::str("POST"),
        .body = kj::Maybe<Body::Initializer>(stringBody(js, text)),
      }));
}

KJ_TEST("A Request built from a Request takes over its unread source without a stream") {
  SinkLog sent;
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;
    auto original = makePost(js, kText);
    auto copy = Request::constructor(js, original.addRef(), kj::none);

    // The original is left used, as taking over its stream would have left it.
    KJ_EXPECT(original->getBodyUsed(js));
    auto stream = KJ_ASSERT_NONNULL(original->getBody(js));
    KJ_EXPECT(stream.isDisturbed(js));
    KJ_EXPECT(stream.isLocked(js));
    KJ_EXPECT_THROW_MESSAGE("Cannot reconstruct a Request with a used body.",
        Request::constructor(js, original.addRef(), kj::none));

    // The copy is still sent straight from the source, as fetch(request) sends its own copy.
    KJ_EXPECT(!copy->getBodyUsed(js));
    kj::Own<WritableStreamSink> sink = kj::heap<TestSink>(sent, SinkMode::SYNC);
    auto pump = KJ_ASSERT_NONNULL(copy->tryDirectPump(js, sink, TryWriteSync::YES));

    // A Request that has created its stream hands the stream over, and the copy pumps it.
    auto streamed = makePost(js, kText);
    KJ_UNUSED auto created = streamed->getBody(js);
    auto streamedCopy = Request::constructor(js, streamed.addRef(), kj::none);
    KJ_EXPECT(streamed->getBodyUsed(js));
    SinkLog unused;
    kj::Own<WritableStreamSink> kept = kj::heap<TestSink>(unused, SinkMode::SYNC);
    KJ_EXPECT(streamedCopy->tryDirectPump(js, kept, TryWriteSync::YES) == kj::none);

    return finishPump(kj::mv(pump));
  });
  KJ_EXPECT(sent.data.asPtr().asConst() == kText.asBytes());
}

// Records the response that Response::send() sends.
class RecordingHttpResponse final: public kj::HttpService::Response {
 public:
  RecordingHttpResponse(SinkLog& log, kj::ArrayPtr<const kj::byte> source)
      : log(log),
        source(source) {}

  kj::Maybe<uint64_t> expectedBodySize;

  kj::Own<kj::AsyncOutputStream> send(uint statusCode,
      kj::StringPtr statusText,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize) override {
    this->expectedBodySize = expectedBodySize;
    return kj::heap<BodyStream>(log, source);
  }

  kj::Own<kj::WebSocket> acceptWebSocket(const kj::HttpHeaders& headers) override {
    KJ_UNIMPLEMENTED("not used in this test");
  }

 private:
  // Takes every write synchronously.
  class BodyStream final: public kj::AsyncOutputStream {
   public:
    BodyStream(SinkLog& log, kj::ArrayPtr<const kj::byte> source): log(log), source(source) {}

    bool tryWriteSync(kj::ArrayPtr<const kj::byte> buffer) override {
      ++log.syncOffers;
      log.record(buffer, true, source);
      return true;
    }
    bool tryWriteSync(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
      ++log.syncOffers;
      for (auto piece: pieces) {
        log.record(piece, true, source);
      }
      return true;
    }
    kj::Promise<void> write(kj::ArrayPtr<const kj::byte> buffer) override {
      log.record(buffer, false, source);
      return kj::READY_NOW;
    }
    kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
      for (auto piece: pieces) {
        log.record(piece, false, source);
      }
      return kj::READY_NOW;
    }
    kj::Promise<void> whenWriteDisconnected() override {
      return kj::NEVER_DONE;
    }

   private:
    SinkLog& log;
    kj::ArrayPtr<const kj::byte> source;
  };

  SinkLog& log;
  kj::ArrayPtr<const kj::byte> source;
};

KJ_TEST("Response::send writes an unread in-memory body straight from its source") {
  // Pumping the body's stream would write copies of its chunks, with either implementation.
  for (bool typescript: {false, true}) {
    SinkLog log;
    auto bytes = patternBytes(kChunk + 1);
    kj::Maybe<RecordingHttpResponse> outer;
    auto fixture = makeFixture(typescript);
    fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
      auto& js = env.js;
      auto blob =
          js.alloc<Blob>(js, jsg::JsBufferSource(jsg::JsUint8Array::create(js, bytes)), kj::str());
      auto& recording = outer.emplace(log, blob->getData(js));
      auto response = makeResponse(js, Body::Initializer(kj::mv(blob)));
      return finishPump(response->send(js, recording, {}, kj::none));
    });
    auto& recording = KJ_ASSERT_NONNULL(outer);
    KJ_EXPECT(recording.expectedBodySize == bytes.size(), typescript);
    KJ_EXPECT(log.data.asPtr() == bytes.asPtr(), typescript);
    KJ_EXPECT(log.allWrites(true, true), typescript);
    KJ_EXPECT(log.chunksAtMost(kChunk), typescript);
  }
}

}  // namespace
}  // namespace workerd::api
