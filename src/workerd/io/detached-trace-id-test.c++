// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/io/trace-stream.h>
#include <workerd/io/tracer.h>
#include <workerd/util/thread-scopes.h>

#include <openssl/siphash.h>

#include <kj/test.h>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

namespace workerd {
namespace {

#if defined(__GLIBC__)
auto allocatedBytes() {
#if __GLIBC_PREREQ(2, 33)
  return mallinfo2().uordblks;
#else
  return mallinfo().uordblks;
#endif
}
#endif

kj::Rc<WorkerTracer> makeTracer() {
  auto trace = kj::refcounted<Trace>(kj::none, kj::none, kj::none, kj::none, kj::none,
      kj::Array<kj::String>(), kj::none, ExecutionModel::STATELESS);
  auto tracer =
      kj::rc<WorkerTracer>(kj::none, kj::mv(trace), PipelineLogLevel::NONE, kj::none, kj::none);
  tracer->markUnused();
  tracer->setMakeUserSpanParentFunc([](tracing::SpanContext identity, DetachedRoot) {
    return SpanParent::fromSpanContext(kj::mv(identity));
  });
  return tracer;
}

class Entropy final: public kj::EntropySource {
 public:
  void generate(kj::ArrayPtr<kj::byte> bytes) override {
    uint64_t word = calls == 0 ? 0x0706050403020100
        : calls == 1           ? 0x0f0e0d0c0b0a0908
                               : 0x0001020304050607 + calls - 2;
    ++calls;
    KJ_REQUIRE(bytes.size() == sizeof(word));
    bytes.write(kj::asBytes(word));
  }
  uint calls = 0;
};

KJ_TEST("self-identifying trace IDs use SipHash-2-4 and reject foreign and modified IDs") {
  Entropy entropy;
  auto tracer = makeTracer();
  KJ_EXPECT(!tracer->isDetachedTraceId(tracing::TraceId(1, 2)));
  auto first = tracer->makeDetachedTraceId(entropy);
  // SipHash's published length-8 vector: key bytes 00..0f, message bytes 00..07.
  KJ_EXPECT(first.toW3C() == "000102030405060793f5f5799a932462");
  KJ_EXPECT(entropy.calls == 3);
  KJ_EXPECT(tracer->isDetachedTraceId(first));
  KJ_EXPECT(!tracer->isDetachedTraceId(tracing::TraceId::nullId));
  KJ_EXPECT(!tracer->isDetachedTraceId(tracing::TraceId(1, 2)));
  KJ_EXPECT(!tracer->isDetachedTraceId(tracing::TraceId(first.getLow() ^ 1, first.getHigh())));
  KJ_EXPECT(!tracer->isDetachedTraceId(tracing::TraceId(first.getLow(), first.getHigh() ^ 1)));
  auto other = makeTracer();
  other->makeDetachedTraceId(entropy);
  KJ_EXPECT(!other->isDetachedTraceId(first));
  for (uint i = 0; i < 100000; ++i) tracer->makeDetachedTraceId(entropy);
  KJ_EXPECT(tracer->isDetachedTraceId(first));
}

KJ_TEST("minting many trace IDs retains constant native memory and always returns W3C IDs") {
  Entropy entropy;
  auto tracer = makeTracer();
  auto first = tracer->makeDetachedTraceId(entropy);  // Warm up the key and crypto library.
#if defined(__GLIBC__)
  auto before = allocatedBytes();
#endif
  for (uint i = 0; i < 100000; ++i) {
    auto id = tracer->makeDetachedTraceId(entropy);
    KJ_EXPECT(id != nullptr);
    KJ_EXPECT(tracer->isDetachedTraceId(id));
  }
#if defined(__GLIBC__)
  KJ_EXPECT(allocatedBytes() == before);
#endif
  KJ_EXPECT(tracer->isDetachedTraceId(first));
  class ZeroEntropy final: public kj::EntropySource {
    void generate(kj::ArrayPtr<kj::byte> bytes) override {
      bytes.fill(0);
    }
  } zero;
  auto zeroTracer = makeTracer();
  auto id = zeroTracer->makeDetachedTraceId(zero);
  KJ_EXPECT(id != nullptr);
  KJ_EXPECT(id.toW3C().size() == 32);
  KJ_EXPECT(zeroTracer->isDetachedTraceId(id));
}

KJ_TEST(
    "predictable roots have a fixed key and deterministic process-wide nonces without entropy") {
  setPredictableModeForTest();
  Entropy entropy;
  const uint64_t key[] = {0x0706050403020100, 0x0f0e0d0c0b0a0908};
  for (uint n = 0; n < 2; ++n) {
    auto tracer = makeTracer();
    auto id = tracer->makeDetachedTraceId(entropy);
    auto nonce = tracing::staticSpanId ^ n;
    uint8_t bytes[8];
    for (uint i = 0; i < sizeof(bytes); ++i) bytes[i] = nonce >> (8 * (7 - i));
    KJ_EXPECT(id == tracing::TraceId(SIPHASH_24(key, bytes, sizeof(bytes)), nonce));
    KJ_EXPECT(tracer->isDetachedTraceId(id));
  }
  KJ_EXPECT(entropy.calls == 0);
}

}  // namespace
}  // namespace workerd
