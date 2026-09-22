// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Benchmark for ReadableStream::pumpTo() in standard.c++.
//
// Measures the performance of ReadableStream::pumpTo() which routes through
// ReadableStreamJsController::pumpTo() and DrainingReader.
//
// This benchmark was originally written to measure improvement from DrainingReader
// adoption, but remains broadly useful for tracking pumpTo throughput and batching.
//
// Usage:
//   # Capture baseline:
//   bazel run --config=opt //src/workerd/tests:bench-pumpto \
//       -- --benchmark_format=json --benchmark_out=baseline.json
//
//   # Capture comparison:
//   bazel run --config=opt //src/workerd/tests:bench-pumpto \
//       -- --benchmark_format=json --benchmark_out=after.json
//
//   # Benchmarks run once per sink write mode (sync:1 = the sink accepts
//   # synchronous writes via tryWriteSync(); sync:0 = the sink declines, forcing
//   # the pump onto the asynchronous write path). The value/byte benchmarks
//   # additionally run once per source batch size (batch = chunksPerPull, one of
//   # 1/16/32/64). To run a subset:
//   bazel run --config=opt //src/workerd/tests:bench-pumpto \
//       -- --benchmark_filter='batch:64/sync:1$'
//
// Key metrics:
//   - bytes_per_second: Primary throughput metric.
//   - WriteOps: Average sink write calls per iteration (sync + async). Directly
//     measures batching. With synchronous streams, WriteOps should be much lower
//     than numChunks because pumpTo writes one vectored batch per drain cycle.
//   - SyncWriteOps / AsyncWriteOps: Average tryWriteSync() vs. async write() calls
//     per iteration. Measures how often the pump hands data to the sink
//     synchronously. In sync:1 variants the sink always accepts sync writes, so
//     AsyncWriteOps should be ~0 when the fast path is in effect; in sync:0
//     variants SyncWriteOps is always 0.
//   - SingleWriteOps / PiecesWriteOps: Average calls to the single-buffer vs.
//     pieces (vectored) overloads per iteration, counting both write() and
//     tryWriteSync() calls.
//   - ChunksPerWrite: Average number of chunks delivered per write call. 1.0
//     means each write carried a single chunk (i.e. no vectored batching);
//     higher values mean drain cycles batch multiple chunks into one write.

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
  // Number of chunks enqueued per pull() call for VALUE and BYTE streams. Each
  // draining read collects the synchronously available data from one pull, so this
  // directly sets the batch size available to each drain cycle (and thus the
  // expected ChunksPerWrite). The value/byte benchmarks set this from their "batch"
  // benchmark argument. Not used by IO_LATENCY_VALUE, which always produces one
  // chunk per pull.
  size_t chunksPerPull = 1;
  // Whether the DiscardingSink accepts synchronous writes (tryWriteSync() consumes
  // the bytes and returns true) or declines them (returns false), forcing the pump
  // onto the asynchronous write path. Set from the "sync" benchmark argument.
  bool sinkSyncWrites = true;
};

// =============================================================================
// Test utilities
// =============================================================================

// A discarding sink that counts bytes written and number of write operations,
// tracking asynchronous (write) and synchronous (tryWriteSync) calls separately,
// which overload shape is hit (single buffer vs. pieces), and the total number of
// chunks received so the benchmark can report the average chunks per write.
struct DiscardingSink final: public kj::AsyncOutputStream {
  // When false, tryWriteSync() declines (returns false without consuming any
  // bytes), modeling a sink that cannot complete writes synchronously and forcing
  // callers onto the asynchronous write() path. This is the "sync" benchmark
  // dimension: sync:1 measures the tryWriteSync fast path, sync:0 the fallback.
  // Note that a declined tryWriteSync() is a probe, not a write, so it does not
  // count toward any of the counters below.
  bool syncWritesEnabled = true;

  size_t bytesWritten = 0;
  size_t asyncWriteCount = 0;
  size_t syncWriteCount = 0;
  // Calls to the single-buffer overloads (both write() and tryWriteSync()).
  size_t singleWriteCount = 0;
  // Calls to the pieces (vectored) overloads (both write() and tryWriteSync()).
  size_t piecesWriteCount = 0;
  // Total chunks received: 1 per single-buffer call, pieces.size() per pieces call.
  size_t chunksWritten = 0;

  kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override {
    asyncWriteCount++;
    singleWriteCount++;
    chunksWritten++;
    bytesWritten += buffer.size();
    co_return;
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
    asyncWriteCount++;
    piecesWriteCount++;
    chunksWritten += pieces.size();
    for (auto piece: pieces) {
      bytesWritten += piece.size();
    }
    co_return;
  }

  bool tryWriteSync(kj::ArrayPtr<const byte> buffer) override {
    if (!syncWritesEnabled) return false;
    syncWriteCount++;
    singleWriteCount++;
    chunksWritten++;
    bytesWritten += buffer.size();
    return true;
  }

  bool tryWriteSync(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
    if (!syncWritesEnabled) return false;
    syncWriteCount++;
    piecesWriteCount++;
    chunksWritten += pieces.size();
    for (auto piece: pieces) {
      bytesWritten += piece.size();
    }
    return true;
  }

  kj::Promise<void> whenWriteDisconnected() override {
    return kj::NEVER_DONE;
  }

  void reset() {
    bytesWritten = 0;
    asyncWriteCount = 0;
    syncWriteCount = 0;
    singleWriteCount = 0;
    piecesWriteCount = 0;
    chunksWritten = 0;
  }
};

// =============================================================================
// Stream creation helpers
// =============================================================================

static size_t benchChunkCounterStatic = 0;

// Creates a JS-backed value ReadableStream that produces data synchronously in pull(),
// enqueuing up to chunksPerPull chunks per pull call. The stream is closed by the same
// pull call that enqueues the final chunk.
jsg::Ref<ReadableStream> createValueStream(
    jsg::Lock& js, size_t chunkSize, size_t numChunks, size_t chunksPerPull, size_t* counter) {
  return ReadableStream::constructor(js,
      UnderlyingSource{
        .pull =
            [chunkSize, numChunks, chunksPerPull, counter](jsg::Lock& js, auto controller) {
    auto& c =
        KJ_ASSERT_NONNULL(controller.template tryGet<jsg::Ref<ReadableStreamDefaultController>>());

    for (size_t i = 0; i < chunksPerPull && *counter < numChunks; i++, (*counter)++) {
      auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, chunkSize);
      jsg::BufferSource buffer(js, kj::mv(backing));
      buffer.asArrayPtr().fill(0xAB);
      c->enqueue(js, jsg::JsValue(buffer.getHandle(js)));
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

// Creates a JS-backed byte ReadableStream that produces data synchronously in pull(),
// enqueuing up to chunksPerPull chunks per pull call. The stream is closed by the same
// pull call that enqueues the final chunk.
jsg::Ref<ReadableStream> createByteStream(
    jsg::Lock& js, size_t chunkSize, size_t numChunks, size_t chunksPerPull, size_t* counter) {
  return ReadableStream::constructor(js,
      UnderlyingSource{
        .type = kj::str("bytes"),
        .pull =
            [chunkSize, numChunks, chunksPerPull, counter](jsg::Lock& js, auto controller) {
    auto& c =
        KJ_ASSERT_NONNULL(controller.template tryGet<jsg::Ref<ReadableByteStreamController>>());

    for (size_t i = 0; i < chunksPerPull && *counter < numChunks; i++, (*counter)++) {
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
              cRef->enqueue(js, jsg::JsValue(buffer.getHandle(js)));
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
      return createValueStream(js, chunkSize, numChunks, config.chunksPerPull, counter);
    case StreamType::BYTE:
      return createByteStream(js, chunkSize, numChunks, config.chunksPerPull, counter);
    case StreamType::IO_LATENCY_VALUE:
      return createIoLatencyValueStream(js, chunkSize, numChunks, counter);
  }
  KJ_UNREACHABLE;
}

// =============================================================================
// Core benchmark function
// =============================================================================

// Exercises: ReadableStream::pumpTo() → ReadableStreamJsController::pumpTo().
static void benchPumpTo(
    benchmark::State& state, size_t chunkSize, size_t numChunks, const StreamConfig& config) {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  TestFixture fixture({.featureFlags = flags.asReader()});

  DiscardingSink sink;
  sink.syncWritesEnabled = config.sinkSyncWrites;
  size_t expectedBytes = chunkSize * numChunks;
  size_t totalAsyncWrites = 0;
  size_t totalSyncWrites = 0;
  size_t totalSingleWrites = 0;
  size_t totalPiecesWrites = 0;
  size_t totalChunks = 0;

  for (auto _: state) {
    sink.reset();

    fixture.runInIoContext([&](const TestFixture::Environment& env) {
      auto stream = createConfiguredStream(env.js, chunkSize, numChunks, config);

      // Wrap DiscardingSink as a WritableStreamSink via newSystemStream.
      // This is the production path: pumpTo receives a WritableStreamSink.
      kj::Own<kj::AsyncOutputStream> fakeOwn(&sink, kj::NullDisposer::instance);
      auto writableSink = newSystemStream(kj::mv(fakeOwn), StreamEncoding::IDENTITY, env.context);

      return env.context.waitForDeferredProxy(stream->pumpTo(env.js, kj::mv(writableSink), true));
    });

    KJ_ASSERT(sink.bytesWritten == expectedBytes, "Expected", expectedBytes, "bytes but got",
        sink.bytesWritten);

    // The sink is reset each iteration, so accumulate totals here for the
    // per-iteration averages reported below.
    totalAsyncWrites += sink.asyncWriteCount;
    totalSyncWrites += sink.syncWriteCount;
    totalSingleWrites += sink.singleWriteCount;
    totalPiecesWrites += sink.piecesWriteCount;
    totalChunks += sink.chunksWritten;
  }

  size_t totalWrites = totalSyncWrites + totalAsyncWrites;
  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(expectedBytes));
  state.counters["WriteOps"] = benchmark::Counter(totalWrites, benchmark::Counter::kAvgIterations);
  state.counters["SyncWriteOps"] =
      benchmark::Counter(totalSyncWrites, benchmark::Counter::kAvgIterations);
  state.counters["AsyncWriteOps"] =
      benchmark::Counter(totalAsyncWrites, benchmark::Counter::kAvgIterations);
  state.counters["SingleWriteOps"] =
      benchmark::Counter(totalSingleWrites, benchmark::Counter::kAvgIterations);
  state.counters["PiecesWriteOps"] =
      benchmark::Counter(totalPiecesWrites, benchmark::Counter::kAvgIterations);
  // Already a per-write average; reported as-is (no per-iteration scaling).
  state.counters["ChunksPerWrite"] = benchmark::Counter(
      totalWrites == 0 ? 0.0 : static_cast<double>(totalChunks) / static_cast<double>(totalWrites));
}

// =============================================================================
// Stream configs
// =============================================================================

// Every benchmark is registered with a "sync" argument selecting whether the sink
// accepts synchronous writes (sync:1, exercising the tryWriteSync fast path) or
// declines them (sync:0, exercising the asynchronous fallback). The value/byte
// benchmarks additionally take a "batch" argument (chunksPerPull of 1, 16, 32, or
// 64). batch:1 is the worst case — one chunk per drain cycle and one write op per
// chunk — and is the most sensitive to per-write overhead, so it shows the largest
// sync:1-vs-sync:0 delta. Larger batches shift the cost toward per-chunk work and
// vectored writes. A drain cycle collects at most kMaxReadPerCycle (256KB), so
// batches beyond 256KB/chunkSize split across multiple writes.
StreamConfig valueConfig(const benchmark::State& state) {
  return {.type = StreamType::VALUE,
    .chunksPerPull = static_cast<size_t>(state.range(0)),
    .sinkSyncWrites = state.range(1) != 0};
}
StreamConfig byteConfig(const benchmark::State& state) {
  return {.type = StreamType::BYTE,
    .chunksPerPull = static_cast<size_t>(state.range(0)),
    .sinkSyncWrites = state.range(1) != 0};
}
StreamConfig ioLatencyConfig(const benchmark::State& state) {
  return {.type = StreamType::IO_LATENCY_VALUE, .sinkSyncWrites = state.range(0) != 0};
}

// =============================================================================
// Synchronous streams — 1 MiB total payload
// =============================================================================
// These are the primary benchmarks. Data is produced synchronously in the pull
// callback, chunksPerPull (the "batch" argument) chunks at a time.

// Value streams
static void PumpTo_64B_Value(benchmark::State& state) {
  benchPumpTo(state, 64, 16384, valueConfig(state));
}
static void PumpTo_256B_Value(benchmark::State& state) {
  benchPumpTo(state, 256, 4096, valueConfig(state));
}
static void PumpTo_1KB_Value(benchmark::State& state) {
  benchPumpTo(state, 1024, 1024, valueConfig(state));
}
static void PumpTo_4KB_Value(benchmark::State& state) {
  benchPumpTo(state, 4096, 256, valueConfig(state));
}
static void PumpTo_16KB_Value(benchmark::State& state) {
  benchPumpTo(state, 16384, 64, valueConfig(state));
}
static void PumpTo_64KB_Value(benchmark::State& state) {
  benchPumpTo(state, 65536, 16, valueConfig(state));
}

// Byte streams
static void PumpTo_64B_Byte(benchmark::State& state) {
  benchPumpTo(state, 64, 16384, byteConfig(state));
}
static void PumpTo_256B_Byte(benchmark::State& state) {
  benchPumpTo(state, 256, 4096, byteConfig(state));
}
static void PumpTo_1KB_Byte(benchmark::State& state) {
  benchPumpTo(state, 1024, 1024, byteConfig(state));
}
static void PumpTo_4KB_Byte(benchmark::State& state) {
  benchPumpTo(state, 4096, 256, byteConfig(state));
}
static void PumpTo_16KB_Byte(benchmark::State& state) {
  benchPumpTo(state, 16384, 64, byteConfig(state));
}
static void PumpTo_64KB_Byte(benchmark::State& state) {
  benchPumpTo(state, 65536, 16, byteConfig(state));
}

// =============================================================================
// I/O latency streams — 64 KiB total payload
// =============================================================================
// Each chunk requires a KJ event loop yield, simulating real network I/O.
// DrainingReader cannot batch these (at most 1 chunk per drain cycle).
// These verify no regression when the stream cannot be batched.
// Smaller total payload because each chunk incurs real event loop overhead.

static void PumpTo_256B_IoLatency(benchmark::State& state) {
  benchPumpTo(state, 256, 256, ioLatencyConfig(state));
}
static void PumpTo_4KB_IoLatency(benchmark::State& state) {
  benchPumpTo(state, 4096, 16, ioLatencyConfig(state));
}
static void PumpTo_64KB_IoLatency(benchmark::State& state) {
  benchPumpTo(state, 65536, 1, ioLatencyConfig(state));
}

// =============================================================================
// Large payload — 10 MiB total, sync value streams
// =============================================================================
// Sustained throughput test with small chunks. More data amortizes fixture
// setup cost, yielding more stable measurements. Note: the 64B/batch:1 variant
// pumps 163,840 unbatched chunks and takes on the order of 10s per iteration;
// use --benchmark_filter to skip it when not needed.

static void PumpTo_64B_10MB_Value(benchmark::State& state) {
  benchPumpTo(state, 64, 163840, valueConfig(state));
}
static void PumpTo_256B_10MB_Value(benchmark::State& state) {
  benchPumpTo(state, 256, 40960, valueConfig(state));
}
static void PumpTo_1KB_10MB_Value(benchmark::State& state) {
  benchPumpTo(state, 1024, 10240, valueConfig(state));
}

// =============================================================================
// Register benchmarks
// =============================================================================

// Registers a sync stream benchmark once per source batch size (chunksPerPull)
// and sink write mode. Both appear in the benchmark name, e.g.
// PumpTo_64B_Value/batch:16/sync:1.
#define WD_PUMPTO_BENCHMARK(X)                                                                     \
  WD_BENCHMARK(X)->ArgNames({"batch", "sync"})->ArgsProduct({{1, 16, 32, 64}, {0, 1}})

// Sync 1 MiB — value streams
WD_PUMPTO_BENCHMARK(PumpTo_64B_Value);
WD_PUMPTO_BENCHMARK(PumpTo_256B_Value);
WD_PUMPTO_BENCHMARK(PumpTo_1KB_Value);
WD_PUMPTO_BENCHMARK(PumpTo_4KB_Value);
WD_PUMPTO_BENCHMARK(PumpTo_16KB_Value);
WD_PUMPTO_BENCHMARK(PumpTo_64KB_Value);

// Sync 1 MiB — byte streams
WD_PUMPTO_BENCHMARK(PumpTo_64B_Byte);
WD_PUMPTO_BENCHMARK(PumpTo_256B_Byte);
WD_PUMPTO_BENCHMARK(PumpTo_1KB_Byte);
WD_PUMPTO_BENCHMARK(PumpTo_4KB_Byte);
WD_PUMPTO_BENCHMARK(PumpTo_16KB_Byte);
WD_PUMPTO_BENCHMARK(PumpTo_64KB_Byte);

// I/O latency — 64 KiB (always one chunk per pull, so no batch dimension; the
// sync dimension matters most here since every chunk is a write op)
WD_BENCHMARK(PumpTo_256B_IoLatency)->ArgName("sync")->ArgsProduct({{0, 1}});
WD_BENCHMARK(PumpTo_4KB_IoLatency)->ArgName("sync")->ArgsProduct({{0, 1}});
WD_BENCHMARK(PumpTo_64KB_IoLatency)->ArgName("sync")->ArgsProduct({{0, 1}});

// Large payload — 10 MiB value streams
WD_PUMPTO_BENCHMARK(PumpTo_64B_10MB_Value);
WD_PUMPTO_BENCHMARK(PumpTo_256B_10MB_Value);
WD_PUMPTO_BENCHMARK(PumpTo_1KB_10MB_Value);

}  // namespace
}  // namespace workerd::api::streams
