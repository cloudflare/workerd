// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Benchmark for PumpToReader in standard.c++.
//
// Measures the performance of ReadableStream::pumpTo() which routes through
// ReadableStreamJsController::pumpTo() → PumpToReader::pumpLoop().
//
// This benchmark establishes a baseline before the DrainingReader adoption,
// then the same benchmarks are re-run after the change to quantify improvement.
//
// Usage:
//   # Capture baseline (before changes):
//   bazel run --config=opt //src/workerd/tests:bench-pumpto \
//       -- --benchmark_format=json --benchmark_out=baseline.json
//
//   # Capture comparison (after changes):
//   bazel run --config=opt //src/workerd/tests:bench-pumpto \
//       -- --benchmark_format=json --benchmark_out=after.json
//
// Key metrics:
//   - bytes_per_second: Primary throughput metric.
//   - WriteOps: Average sink write calls per iteration. Directly measures batching.
//     Before DrainingReader adoption: WriteOps ≈ numChunks (one write per chunk).
//     After: WriteOps ≪ numChunks (one vectored write per drain cycle).

#include <workerd/api/streams/standard.h>
#include <workerd/api/system-streams.h>
#include <workerd/tests/bench-tools.h>
#include <workerd/tests/test-fixture.h>

namespace workerd::api::streams {
namespace {

// =============================================================================
// Stream configuration
// =============================================================================

enum class StreamType {
  VALUE,             // Default ReadableStreamDefaultController
  BYTE,              // ReadableByteStreamController
  IO_LATENCY_VALUE,  // Value stream that yields to KJ event loop between chunks
};

struct StreamConfig {
  StreamType type = StreamType::VALUE;
};

// =============================================================================
// Test utilities
// =============================================================================

// A discarding sink that counts bytes written and number of write operations.
struct DiscardingSink final: public kj::AsyncOutputStream {
  size_t bytesWritten = 0;
  size_t writeCount = 0;

  kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override {
    writeCount++;
    bytesWritten += buffer.size();
    co_return;
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
    writeCount++;
    for (auto piece: pieces) {
      bytesWritten += piece.size();
    }
    co_return;
  }

  kj::Promise<void> whenWriteDisconnected() override {
    return kj::NEVER_DONE;
  }

  void reset() {
    bytesWritten = 0;
    writeCount = 0;
  }
};

// =============================================================================
// Stream creation helpers
// =============================================================================

static size_t benchChunkCounterStatic = 0;

// Creates a JS-backed value ReadableStream that produces data synchronously in pull().
jsg::Ref<ReadableStream> createValueStream(
    jsg::Lock& js, size_t chunkSize, size_t numChunks, size_t* counter) {
  return ReadableStream::constructor(js,
      UnderlyingSource{
        .pull =
            [chunkSize, numChunks, counter](jsg::Lock& js, auto controller) {
    auto& c =
        KJ_ASSERT_NONNULL(controller.template tryGet<jsg::Ref<ReadableStreamDefaultController>>());

    if ((*counter)++ < numChunks) {
      auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, chunkSize);
      jsg::BufferSource buffer(js, kj::mv(backing));
      buffer.asArrayPtr().fill(0xAB);
      c->enqueue(js, buffer.getHandle(js));
    }
    if (*counter == numChunks) {
      c->close(js);
    }
    return js.resolvedPromise();
  },
        .expectedLength = chunkSize * numChunks,
      },
      StreamQueuingStrategy{
        .highWaterMark = 0,
      });
}

// Creates a JS-backed byte ReadableStream that produces data synchronously in pull().
jsg::Ref<ReadableStream> createByteStream(
    jsg::Lock& js, size_t chunkSize, size_t numChunks, size_t* counter) {
  return ReadableStream::constructor(js,
      UnderlyingSource{
        .type = kj::str("bytes"),
        .pull =
            [chunkSize, numChunks, counter](jsg::Lock& js, auto controller) {
    auto& c =
        KJ_ASSERT_NONNULL(controller.template tryGet<jsg::Ref<ReadableByteStreamController>>());

    if ((*counter)++ < numChunks) {
      auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, chunkSize);
      jsg::BufferSource buffer(js, kj::mv(backing));
      buffer.asArrayPtr().fill(0xAB);
      c->enqueue(js, kj::mv(buffer));
    }
    if (*counter == numChunks) {
      c->close(js);
    }
    return js.resolvedPromise();
  },
        .expectedLength = chunkSize * numChunks,
      },
      StreamQueuingStrategy{
        .highWaterMark = 0,
      });
}

// Creates a value stream that yields to the KJ event loop between chunks.
// Simulates a network stream where data arrives with real I/O latency.
// Each chunk requires a KJ event loop iteration, so DrainingReader cannot batch them.
jsg::Ref<ReadableStream> createIoLatencyValueStream(
    jsg::Lock& js, size_t chunkSize, size_t numChunks, size_t* counter) {
  return ReadableStream::constructor(js,
      UnderlyingSource{
        .pull =
            [chunkSize, numChunks, counter](jsg::Lock& js, auto controller) {
    auto& c =
        KJ_ASSERT_NONNULL(controller.template tryGet<jsg::Ref<ReadableStreamDefaultController>>());

    if (*counter >= numChunks) {
      c->close(js);
      return js.resolvedPromise();
    }

    // Use IoContext.awaitIo() to wait for a KJ event loop yield.
    // kj::evalLater() schedules on the next KJ event loop iteration.
    auto& ioContext = IoContext::current();
    auto cRef = c.addRef();
    return ioContext.awaitIo(js, kj::evalLater([]() {}),
        JSG_VISITABLE_LAMBDA(
            (cRef = kj::mv(cRef), chunkSize, numChunks, counter), (cRef), (jsg::Lock & js) mutable {
              if ((*counter)++ < numChunks) {
              auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, chunkSize);
              jsg::BufferSource buffer(js, kj::mv(backing));
              buffer.asArrayPtr().fill(0xAB);
              cRef->enqueue(js, buffer.getHandle(js));
              }
              if (*counter == numChunks) {
              cRef->close(js);
              }
            }));
  },
        .expectedLength = chunkSize * numChunks,
      },
      StreamQueuingStrategy{
        .highWaterMark = 0,
      });
}

jsg::Ref<ReadableStream> createConfiguredStream(
    jsg::Lock& js, size_t chunkSize, size_t numChunks, const StreamConfig& config) {
  benchChunkCounterStatic = 0;
  size_t* counter = &benchChunkCounterStatic;

  switch (config.type) {
    case StreamType::VALUE:
      return createValueStream(js, chunkSize, numChunks, counter);
    case StreamType::BYTE:
      return createByteStream(js, chunkSize, numChunks, counter);
    case StreamType::IO_LATENCY_VALUE:
      return createIoLatencyValueStream(js, chunkSize, numChunks, counter);
  }
  KJ_UNREACHABLE;
}

// =============================================================================
// Core benchmark function
// =============================================================================

// Exercises: ReadableStream::pumpTo() → ReadableStreamJsController::pumpTo() → PumpToReader
static void benchPumpTo(
    benchmark::State& state, size_t chunkSize, size_t numChunks, const StreamConfig& config) {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  TestFixture fixture({.featureFlags = flags.asReader()});

  DiscardingSink sink;
  size_t expectedBytes = chunkSize * numChunks;

  for (auto _: state) {
    sink.reset();

    fixture.runInIoContext([&](const TestFixture::Environment& env) {
      auto stream = createConfiguredStream(env.js, chunkSize, numChunks, config);

      // Wrap DiscardingSink as a WritableStreamSink via newSystemStream.
      // This is the production path: PumpToReader receives a WritableStreamSink.
      kj::Own<kj::AsyncOutputStream> fakeOwn(&sink, kj::NullDisposer::instance);
      auto writableSink = newSystemStream(kj::mv(fakeOwn), StreamEncoding::IDENTITY, env.context);

      return env.context.waitForDeferredProxy(stream->pumpTo(env.js, kj::mv(writableSink), true));
    });

    KJ_ASSERT(sink.bytesWritten == expectedBytes, "Expected", expectedBytes, "bytes but got",
        sink.bytesWritten);
  }

  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(expectedBytes));
  state.counters["WriteOps"] =
      benchmark::Counter(sink.writeCount, benchmark::Counter::kAvgIterations);
}

// =============================================================================
// Stream configs
// =============================================================================

static const StreamConfig VALUE_DEFAULT{.type = StreamType::VALUE};
static const StreamConfig BYTE_DEFAULT{.type = StreamType::BYTE};
static const StreamConfig IO_LATENCY_VALUE_DEFAULT{.type = StreamType::IO_LATENCY_VALUE};

// =============================================================================
// Synchronous streams — 1 MiB total payload
// =============================================================================
// These are the primary benchmarks. Data is produced synchronously in the pull
// callback. DrainingReader (post-change) can drain all chunks in a single lock
// acquisition, so small-chunk benchmarks should see large improvement.

// Value streams
static void PumpTo_64B_Value(benchmark::State& state) {
  benchPumpTo(state, 64, 16384, VALUE_DEFAULT);
}
static void PumpTo_256B_Value(benchmark::State& state) {
  benchPumpTo(state, 256, 4096, VALUE_DEFAULT);
}
static void PumpTo_1KB_Value(benchmark::State& state) {
  benchPumpTo(state, 1024, 1024, VALUE_DEFAULT);
}
static void PumpTo_4KB_Value(benchmark::State& state) {
  benchPumpTo(state, 4096, 256, VALUE_DEFAULT);
}
static void PumpTo_16KB_Value(benchmark::State& state) {
  benchPumpTo(state, 16384, 64, VALUE_DEFAULT);
}
static void PumpTo_64KB_Value(benchmark::State& state) {
  benchPumpTo(state, 65536, 16, VALUE_DEFAULT);
}

// Byte streams
static void PumpTo_64B_Byte(benchmark::State& state) {
  benchPumpTo(state, 64, 16384, BYTE_DEFAULT);
}
static void PumpTo_256B_Byte(benchmark::State& state) {
  benchPumpTo(state, 256, 4096, BYTE_DEFAULT);
}
static void PumpTo_1KB_Byte(benchmark::State& state) {
  benchPumpTo(state, 1024, 1024, BYTE_DEFAULT);
}
static void PumpTo_4KB_Byte(benchmark::State& state) {
  benchPumpTo(state, 4096, 256, BYTE_DEFAULT);
}
static void PumpTo_16KB_Byte(benchmark::State& state) {
  benchPumpTo(state, 16384, 64, BYTE_DEFAULT);
}
static void PumpTo_64KB_Byte(benchmark::State& state) {
  benchPumpTo(state, 65536, 16, BYTE_DEFAULT);
}

// =============================================================================
// I/O latency streams — 64 KiB total payload
// =============================================================================
// Each chunk requires a KJ event loop yield, simulating real network I/O.
// DrainingReader cannot batch these (at most 1 chunk per drain cycle).
// These verify no regression from the PumpToReader change.
// Smaller total payload because each chunk incurs real event loop overhead.

static void PumpTo_256B_IoLatency(benchmark::State& state) {
  benchPumpTo(state, 256, 256, IO_LATENCY_VALUE_DEFAULT);
}
static void PumpTo_4KB_IoLatency(benchmark::State& state) {
  benchPumpTo(state, 4096, 16, IO_LATENCY_VALUE_DEFAULT);
}
static void PumpTo_64KB_IoLatency(benchmark::State& state) {
  benchPumpTo(state, 65536, 1, IO_LATENCY_VALUE_DEFAULT);
}

// =============================================================================
// Large payload — 10 MiB total, sync value streams
// =============================================================================
// Sustained throughput test with small chunks. More data amortizes fixture
// setup cost, yielding more stable measurements.

static void PumpTo_64B_10MB_Value(benchmark::State& state) {
  benchPumpTo(state, 64, 163840, VALUE_DEFAULT);
}
static void PumpTo_256B_10MB_Value(benchmark::State& state) {
  benchPumpTo(state, 256, 40960, VALUE_DEFAULT);
}
static void PumpTo_1KB_10MB_Value(benchmark::State& state) {
  benchPumpTo(state, 1024, 10240, VALUE_DEFAULT);
}

// =============================================================================
// Register benchmarks
// =============================================================================

// Sync 1 MiB — value streams
WD_BENCHMARK(PumpTo_64B_Value);
WD_BENCHMARK(PumpTo_256B_Value);
WD_BENCHMARK(PumpTo_1KB_Value);
WD_BENCHMARK(PumpTo_4KB_Value);
WD_BENCHMARK(PumpTo_16KB_Value);
WD_BENCHMARK(PumpTo_64KB_Value);

// Sync 1 MiB — byte streams
WD_BENCHMARK(PumpTo_64B_Byte);
WD_BENCHMARK(PumpTo_256B_Byte);
WD_BENCHMARK(PumpTo_1KB_Byte);
WD_BENCHMARK(PumpTo_4KB_Byte);
WD_BENCHMARK(PumpTo_16KB_Byte);
WD_BENCHMARK(PumpTo_64KB_Byte);

// I/O latency — 64 KiB (no-regression check)
WD_BENCHMARK(PumpTo_256B_IoLatency);
WD_BENCHMARK(PumpTo_4KB_IoLatency);
WD_BENCHMARK(PumpTo_64KB_IoLatency);

// Large payload — 10 MiB value streams
WD_BENCHMARK(PumpTo_64B_10MB_Value);
WD_BENCHMARK(PumpTo_256B_10MB_Value);
WD_BENCHMARK(PumpTo_1KB_10MB_Value);

}  // namespace
}  // namespace workerd::api::streams
