// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Benchmark to compare stream piping implementations:
// 1. Existing approach (ReadableStream::pumpTo via PumpToReader) - uses JS promise-based loop
// 2. New approach (ReadableSourceKjAdapter::pumpTo) - uses adaptive buffer sizing and vectored writes
//
// Run with: bazel run --config=opt //src/workerd/tests:bench-stream-piping

#include <workerd/api/streams/readable-source-adapter.h>
#include <workerd/api/streams/standard.h>
#include <workerd/api/streams/writable-sink.h>
#include <workerd/api/system-streams.h>
#include <workerd/tests/bench-tools.h>
#include <workerd/tests/test-fixture.h>

#include <kj/compat/http.h>

namespace workerd::api::streams {
namespace {

// =============================================================================
// Stream configuration types
// =============================================================================

enum class StreamType {
  VALUE,             // Default ReadableStreamDefaultController
  BYTE,              // ReadableByteStreamController
  SLOW_VALUE,        // Value stream that produces one chunk per microtask (async)
  IO_LATENCY_VALUE,  // Value stream that yields to KJ event loop between chunks
  IO_LATENCY_BYTE,   // Byte stream that yields to KJ event loop between chunks
  TIMED_VALUE,       // Value stream with configurable timer delay between chunks
};

struct StreamConfig {
  StreamType type = StreamType::VALUE;
  kj::Maybe<size_t> autoAllocateChunkSize;         // Only valid for BYTE streams
  kj::Duration chunkDelay = 0 * kj::MILLISECONDS;  // Delay between chunks for TIMED_* streams
  double highWaterMark = 0;                        // 0 means default (pull on demand)
  bool includeExpectedLength = true;               // If false, stream won't report length
};

// =============================================================================
// Test utilities
// =============================================================================

// A discarding sink that just counts bytes written (more representative of real network I/O).
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

// A sink that simulates network backpressure with configurable latency per write.
// This represents real-world scenarios where the downstream connection is slower
// than the upstream source (e.g., slow client, congested network).
struct LatencySink final: public kj::AsyncOutputStream {
  kj::Timer& timer;
  kj::Duration writeLatency;
  size_t bytesWritten = 0;
  size_t writeCount = 0;

  LatencySink(kj::Timer& timer, kj::Duration writeLatency)
      : timer(timer),
        writeLatency(writeLatency) {}

  kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override {
    writeCount++;
    bytesWritten += buffer.size();
    if (writeLatency > 0 * kj::MILLISECONDS) {
      co_await timer.afterDelay(writeLatency);
    }
    co_return;
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
    writeCount++;
    for (auto piece: pieces) {
      bytesWritten += piece.size();
    }
    if (writeLatency > 0 * kj::MILLISECONDS) {
      co_await timer.afterDelay(writeLatency);
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

// Creates a JS-backed ReadableStream with the specified configuration.
// Uses a counter pointer similar to the unit tests in readable-source-adapter-test.c++.
static size_t benchChunkCounterStatic = 0;

jsg::Ref<ReadableStream> createValueStream(jsg::Lock& js,
    size_t chunkSize,
    size_t numChunks,
    double highWaterMark,
    size_t* counter,
    bool includeExpectedLength = true) {
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
        .expectedLength = includeExpectedLength ? kj::Maybe<uint64_t>(chunkSize * numChunks)
                                                : kj::Maybe<uint64_t>(kj::none),
      },
      StreamQueuingStrategy{
        .highWaterMark = highWaterMark,
      });
}

jsg::Ref<ReadableStream> createByteStream(jsg::Lock& js,
    size_t chunkSize,
    size_t numChunks,
    kj::Maybe<size_t> autoAllocateChunkSize,
    double highWaterMark,
    size_t* counter) {
  return ReadableStream::constructor(js,
      UnderlyingSource{
        .type = kj::str("bytes"),
        .autoAllocateChunkSize = autoAllocateChunkSize,
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
        .highWaterMark = highWaterMark,
      });
}

// Creates a "slow" value stream that produces one chunk per microtask.
// This simulates a stream where pull() has async work to do before data is ready.
// The pull() function returns a promise that resolves on the next microtask,
// and only enqueues data WHEN the promise resolves.
//
// NOTE: This does NOT prevent batching or trigger the adaptive read policy!
// Microtask delays execute synchronously within the JS event loop turn, so
// readInternal's promise chain runs to completion before returning to KJ.
// The buffer still fills completely, achieving full batching (100 chunks → 1 write).
// See PUMP_PERFORMANCE_ANALYSIS.md section 9 for detailed analysis.
jsg::Ref<ReadableStream> createSlowValueStream(
    jsg::Lock& js, size_t chunkSize, size_t numChunks, double highWaterMark, size_t* counter) {
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

    // Return a promise that enqueues data on the next microtask.
    // This adds a tiny delay per chunk, but does NOT prevent batching -
    // the entire promise chain still runs within one JS event loop turn.
    auto cRef = c.addRef();
    return js.resolvedPromise().then(js,
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
              return js.resolvedPromise();
            }));
  },
        .expectedLength = chunkSize * numChunks,
      },
      StreamQueuingStrategy{
        .highWaterMark = highWaterMark,
      });
}

// Creates a value stream that yields to the KJ event loop between chunks.
// This simulates a network stream (like fetch response body) where data arrives with real
// I/O latency. Unlike the "slow" stream that uses microtask delays, this stream's pull()
// returns a promise that only resolves after a KJ event loop iteration.
//
// This WILL cause pumpReadImpl to return early, potentially triggering the adaptive read policy.
// See PUMP_PERFORMANCE_ANALYSIS.md section 9 for why this is different from microtask delays.
jsg::Ref<ReadableStream> createIoLatencyValueStream(
    jsg::Lock& js, size_t chunkSize, size_t numChunks, double highWaterMark, size_t* counter) {
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
    // This simulates real network I/O latency where we yield to KJ between chunks.
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
        .highWaterMark = highWaterMark,
      });
}

// Creates a byte stream that yields to the KJ event loop between chunks.
// Same as createIoLatencyValueStream but uses ReadableByteStreamController.
jsg::Ref<ReadableStream> createIoLatencyByteStream(
    jsg::Lock& js, size_t chunkSize, size_t numChunks, double highWaterMark, size_t* counter) {
  return ReadableStream::constructor(js,
      UnderlyingSource{
        .type = kj::str("bytes"),
        .pull =
            [chunkSize, numChunks, counter](jsg::Lock& js, auto controller) {
    auto& c =
        KJ_ASSERT_NONNULL(controller.template tryGet<jsg::Ref<ReadableByteStreamController>>());

    if (*counter >= numChunks) {
      c->close(js);
      return js.resolvedPromise();
    }

    auto& ioContext = IoContext::current();
    auto cRef = c.addRef();
    return ioContext.awaitIo(js, kj::evalLater([]() {}),
        JSG_VISITABLE_LAMBDA(
            (cRef = kj::mv(cRef), chunkSize, numChunks, counter), (cRef), (jsg::Lock & js) mutable {
              if ((*counter)++ < numChunks) {
              auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, chunkSize);
              jsg::BufferSource buffer(js, kj::mv(backing));
              buffer.asArrayPtr().fill(0xAB);
              cRef->enqueue(js, kj::mv(buffer));
              }
              if (*counter == numChunks) {
              cRef->close(js);
              }
            }));
  },
        .expectedLength = chunkSize * numChunks,
      },
      StreamQueuingStrategy{
        .highWaterMark = highWaterMark,
      });
}

// Creates a value stream with actual timer-based delays between chunks.
// This simulates real network I/O where data arrives with measurable latency.
// Unlike evalLater() which resumes immediately, timer delays represent real wall-clock time.
//
// With delays, we can observe:
// 1. How throughput scales with I/O latency
// 2. The true cost of per-chunk I/O operations
jsg::Ref<ReadableStream> createTimedValueStream(jsg::Lock& js,
    size_t chunkSize,
    size_t numChunks,
    double highWaterMark,
    kj::Duration delay,
    size_t* counter) {
  return ReadableStream::constructor(js,
      UnderlyingSource{
        .pull =
            [chunkSize, numChunks, delay, counter](jsg::Lock& js, auto controller) {
    auto& c =
        KJ_ASSERT_NONNULL(controller.template tryGet<jsg::Ref<ReadableStreamDefaultController>>());

    if (*counter >= numChunks) {
      c->close(js);
      return js.resolvedPromise();
    }

    // Use afterLimitTimeout for actual timer-based delay
    auto& ioContext = IoContext::current();
    auto cRef = c.addRef();
    return ioContext.awaitIo(js, ioContext.afterLimitTimeout(delay),
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
        .highWaterMark = highWaterMark,
      });
}

jsg::Ref<ReadableStream> createConfiguredStream(
    jsg::Lock& js, size_t chunkSize, size_t numChunks, const StreamConfig& config) {
  benchChunkCounterStatic = 0;
  size_t* counter = &benchChunkCounterStatic;

  switch (config.type) {
    case StreamType::VALUE:
      return createValueStream(
          js, chunkSize, numChunks, config.highWaterMark, counter, config.includeExpectedLength);
    case StreamType::BYTE:
      return createByteStream(
          js, chunkSize, numChunks, config.autoAllocateChunkSize, config.highWaterMark, counter);
    case StreamType::SLOW_VALUE:
      return createSlowValueStream(js, chunkSize, numChunks, config.highWaterMark, counter);
    case StreamType::IO_LATENCY_VALUE:
      return createIoLatencyValueStream(js, chunkSize, numChunks, config.highWaterMark, counter);
    case StreamType::IO_LATENCY_BYTE:
      return createIoLatencyByteStream(js, chunkSize, numChunks, config.highWaterMark, counter);
    case StreamType::TIMED_VALUE:
      return createTimedValueStream(
          js, chunkSize, numChunks, config.highWaterMark, config.chunkDelay, counter);
  }
  KJ_UNREACHABLE;
}

// =============================================================================
// Benchmark: New approach using ReadableSourceKjAdapter::pumpTo
// =============================================================================

static void benchNewApproachPumpTo(
    benchmark::State& state, size_t chunkSize, size_t numChunks, const StreamConfig& config) {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  // Enable real timers for streams that need actual timer functionality (e.g., TIMED_VALUE).
  bool needsRealTimers = config.type == StreamType::TIMED_VALUE;
  TestFixture fixture({.featureFlags = flags.asReader(), .useRealTimers = needsRealTimers});

  DiscardingSink sink;

  for (auto _: state) {
    sink.reset();
    kj::Own<kj::AsyncOutputStream> fakeOwn(&sink, kj::NullDisposer::instance);
    auto writableSink = newWritableSink(kj::mv(fakeOwn));

    fixture.runInIoContext([&](const TestFixture::Environment& env) {
      auto stream = createConfiguredStream(env.js, chunkSize, numChunks, config);
      auto adapter = kj::heap<ReadableSourceKjAdapter>(env.js, env.context, stream.addRef());
      return adapter->pumpTo(*writableSink, EndAfterPump::YES).attach(kj::mv(adapter));
    });
  }

  state.SetBytesProcessed(
      state.iterations() * static_cast<int64_t>(chunkSize) * static_cast<int64_t>(numChunks));
  state.counters["WriteOps"] =
      benchmark::Counter(sink.writeCount, benchmark::Counter::kAvgIterations);
}

// =============================================================================
// Benchmark: Existing approach using ReadableStream::pumpTo (PumpToReader)
// =============================================================================

static void benchExistingApproachPumpTo(
    benchmark::State& state, size_t chunkSize, size_t numChunks, const StreamConfig& config) {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  // Enable real timers for streams that need actual timer functionality (e.g., TIMED_VALUE).
  bool needsRealTimers = config.type == StreamType::TIMED_VALUE;
  TestFixture fixture({.featureFlags = flags.asReader(), .useRealTimers = needsRealTimers});

  DiscardingSink sink;

  for (auto _: state) {
    sink.reset();

    fixture.runInIoContext([&](const TestFixture::Environment& env) {
      auto stream = createConfiguredStream(env.js, chunkSize, numChunks, config);

      kj::Own<kj::AsyncOutputStream> fakeOwn(&sink, kj::NullDisposer::instance);
      auto writableSink = newSystemStream(kj::mv(fakeOwn), StreamEncoding::IDENTITY, env.context);

      return env.context.waitForDeferredProxy(stream->pumpTo(env.js, kj::mv(writableSink), true));
    });
  }

  state.SetBytesProcessed(
      state.iterations() * static_cast<int64_t>(chunkSize) * static_cast<int64_t>(numChunks));
  state.counters["WriteOps"] =
      benchmark::Counter(sink.writeCount, benchmark::Counter::kAvgIterations);
}

// =============================================================================
// Stream configurations to benchmark
// =============================================================================

// Value stream with default highWaterMark (0)
static const StreamConfig VALUE_DEFAULT{
  .type = StreamType::VALUE,
  .autoAllocateChunkSize = kj::none,
  .highWaterMark = 0,
};

// Value stream with 16KB highWaterMark
static const StreamConfig VALUE_HWM_16K{
  .type = StreamType::VALUE,
  .autoAllocateChunkSize = kj::none,
  .highWaterMark = 16 * 1024,
};

// Value stream without expectedLength - forces default buffer size (32KB)
// Used to test leftover mechanism when chunks > buffer
static const StreamConfig VALUE_NO_LENGTH{
  .type = StreamType::VALUE,
  .autoAllocateChunkSize = kj::none,
  .highWaterMark = 0,
  .includeExpectedLength = false,
};

// Byte stream without autoAllocateChunkSize, default highWaterMark
static const StreamConfig BYTE_DEFAULT{
  .type = StreamType::BYTE,
  .autoAllocateChunkSize = kj::none,
  .highWaterMark = 0,
};

// Byte stream with autoAllocateChunkSize=64KB (fixed), default highWaterMark
static const StreamConfig BYTE_AUTO_64K{
  .type = StreamType::BYTE,
  .autoAllocateChunkSize = 65536,
  .highWaterMark = 0,
};

// Byte stream without autoAllocateChunkSize, 16KB highWaterMark
static const StreamConfig BYTE_HWM_16K{
  .type = StreamType::BYTE,
  .autoAllocateChunkSize = kj::none,
  .highWaterMark = 16 * 1024,
};

// Byte stream with autoAllocateChunkSize=64KB, 16KB highWaterMark
static const StreamConfig BYTE_AUTO_64K_HWM_16K{
  .type = StreamType::BYTE,
  .autoAllocateChunkSize = 65536,
  .highWaterMark = 16 * 1024,
};

// Slow value stream (async, one chunk per microtask) - does NOT trigger adaptive read policy
// because microtasks execute synchronously within the JS event loop turn.
static const StreamConfig SLOW_VALUE_DEFAULT{
  .type = StreamType::SLOW_VALUE,
  .autoAllocateChunkSize = kj::none,
  .highWaterMark = 0,
};

// I/O latency value stream - yields to KJ event loop between chunks, simulating network I/O.
// This DOES trigger early returns from pumpReadImpl and may activate the adaptive policy.
static const StreamConfig IO_LATENCY_VALUE_DEFAULT{
  .type = StreamType::IO_LATENCY_VALUE,
  .autoAllocateChunkSize = kj::none,
  .highWaterMark = 0,
};

// I/O latency byte stream - same as above but using ReadableByteStreamController.
// Tests how byte streams interact with I/O latency patterns.
static const StreamConfig IO_LATENCY_BYTE_DEFAULT{
  .type = StreamType::IO_LATENCY_BYTE,
  .autoAllocateChunkSize = kj::none,
  .highWaterMark = 0,
};

// Timed value streams - actual timer-based delays between chunks.
// These simulate real network I/O with measurable latency.
// The delay represents the time waiting for the next chunk from the network.

// 10μs delay - fast network, minimal latency (e.g., local network)
static const StreamConfig TIMED_VALUE_10US{
  .type = StreamType::TIMED_VALUE,
  .autoAllocateChunkSize = kj::none,
  .chunkDelay = 10 * kj::MICROSECONDS,
  .highWaterMark = 0,
};

// 100μs delay - typical datacenter latency
static const StreamConfig TIMED_VALUE_100US{
  .type = StreamType::TIMED_VALUE,
  .autoAllocateChunkSize = kj::none,
  .chunkDelay = 100 * kj::MICROSECONDS,
  .highWaterMark = 0,
};

// 1ms delay - typical internet latency / slow upstream
static const StreamConfig TIMED_VALUE_1MS{
  .type = StreamType::TIMED_VALUE,
  .autoAllocateChunkSize = kj::none,
  .chunkDelay = 1 * kj::MILLISECONDS,
  .highWaterMark = 0,
};

// =============================================================================
// Chunk size configurations
// =============================================================================

// Tiny chunks (worst case for JS overhead): 64 * 256 = 16,384 bytes
static constexpr size_t TINY_CHUNK_SIZE = 64;
static constexpr size_t TINY_NUM_CHUNKS = 256;

// Small chunks (chatty protocol pattern): 256 * 100 = 25,600 bytes
static constexpr size_t SMALL_CHUNK_SIZE = 256;
static constexpr size_t SMALL_NUM_CHUNKS = 100;

// Medium chunks (typical HTTP response): 4096 * 100 = 409,600 bytes (~400KB)
static constexpr size_t MEDIUM_CHUNK_SIZE = 4096;
static constexpr size_t MEDIUM_NUM_CHUNKS = 100;

// Large chunks (file transfer pattern): 65536 * 16 = 1,048,576 bytes (1MB)
static constexpr size_t LARGE_CHUNK_SIZE = 65536;
static constexpr size_t LARGE_NUM_CHUNKS = 16;

// Huge chunks (exercises leftover mechanism): 524288 * 4 = 2,097,152 bytes (2MB)
// These chunks (512KB each) are larger than the max buffer size (256KB * 2 = 512KB),
// so each chunk will produce leftover data that needs to be handled.
static constexpr size_t HUGE_CHUNK_SIZE = 524288;
static constexpr size_t HUGE_NUM_CHUNKS = 4;

// =============================================================================
// Benchmark functions - Value streams
// =============================================================================

// Value stream, default HWM
static void New_Tiny_Value(benchmark::State& state) {
  benchNewApproachPumpTo(state, TINY_CHUNK_SIZE, TINY_NUM_CHUNKS, VALUE_DEFAULT);
}
static void Existing_Tiny_Value(benchmark::State& state) {
  benchExistingApproachPumpTo(state, TINY_CHUNK_SIZE, TINY_NUM_CHUNKS, VALUE_DEFAULT);
}
static void New_Small_Value(benchmark::State& state) {
  benchNewApproachPumpTo(state, SMALL_CHUNK_SIZE, SMALL_NUM_CHUNKS, VALUE_DEFAULT);
}
static void Existing_Small_Value(benchmark::State& state) {
  benchExistingApproachPumpTo(state, SMALL_CHUNK_SIZE, SMALL_NUM_CHUNKS, VALUE_DEFAULT);
}
static void New_Medium_Value(benchmark::State& state) {
  benchNewApproachPumpTo(state, MEDIUM_CHUNK_SIZE, MEDIUM_NUM_CHUNKS, VALUE_DEFAULT);
}
static void Existing_Medium_Value(benchmark::State& state) {
  benchExistingApproachPumpTo(state, MEDIUM_CHUNK_SIZE, MEDIUM_NUM_CHUNKS, VALUE_DEFAULT);
}
static void New_Large_Value(benchmark::State& state) {
  benchNewApproachPumpTo(state, LARGE_CHUNK_SIZE, LARGE_NUM_CHUNKS, VALUE_DEFAULT);
}
static void Existing_Large_Value(benchmark::State& state) {
  benchExistingApproachPumpTo(state, LARGE_CHUNK_SIZE, LARGE_NUM_CHUNKS, VALUE_DEFAULT);
}

// Value stream, 16KB HWM
static void New_Tiny_Value_HWM16K(benchmark::State& state) {
  benchNewApproachPumpTo(state, TINY_CHUNK_SIZE, TINY_NUM_CHUNKS, VALUE_HWM_16K);
}
static void Existing_Tiny_Value_HWM16K(benchmark::State& state) {
  benchExistingApproachPumpTo(state, TINY_CHUNK_SIZE, TINY_NUM_CHUNKS, VALUE_HWM_16K);
}
static void New_Small_Value_HWM16K(benchmark::State& state) {
  benchNewApproachPumpTo(state, SMALL_CHUNK_SIZE, SMALL_NUM_CHUNKS, VALUE_HWM_16K);
}
static void Existing_Small_Value_HWM16K(benchmark::State& state) {
  benchExistingApproachPumpTo(state, SMALL_CHUNK_SIZE, SMALL_NUM_CHUNKS, VALUE_HWM_16K);
}
static void New_Medium_Value_HWM16K(benchmark::State& state) {
  benchNewApproachPumpTo(state, MEDIUM_CHUNK_SIZE, MEDIUM_NUM_CHUNKS, VALUE_HWM_16K);
}
static void Existing_Medium_Value_HWM16K(benchmark::State& state) {
  benchExistingApproachPumpTo(state, MEDIUM_CHUNK_SIZE, MEDIUM_NUM_CHUNKS, VALUE_HWM_16K);
}
static void New_Large_Value_HWM16K(benchmark::State& state) {
  benchNewApproachPumpTo(state, LARGE_CHUNK_SIZE, LARGE_NUM_CHUNKS, VALUE_HWM_16K);
}
static void Existing_Large_Value_HWM16K(benchmark::State& state) {
  benchExistingApproachPumpTo(state, LARGE_CHUNK_SIZE, LARGE_NUM_CHUNKS, VALUE_HWM_16K);
}

// Huge chunks - exercises leftover mechanism (512KB chunks > buffer size)
// Uses VALUE_NO_LENGTH to force default buffer size (32KB), ensuring leftover occurs
static void New_Huge_Value(benchmark::State& state) {
  benchNewApproachPumpTo(state, HUGE_CHUNK_SIZE, HUGE_NUM_CHUNKS, VALUE_NO_LENGTH);
}
static void Existing_Huge_Value(benchmark::State& state) {
  benchExistingApproachPumpTo(state, HUGE_CHUNK_SIZE, HUGE_NUM_CHUNKS, VALUE_NO_LENGTH);
}

// =============================================================================
// Benchmark functions - Byte streams, no autoAllocate
// =============================================================================

// Byte stream, default HWM, no autoAllocate
static void New_Tiny_Byte(benchmark::State& state) {
  benchNewApproachPumpTo(state, TINY_CHUNK_SIZE, TINY_NUM_CHUNKS, BYTE_DEFAULT);
}
static void Existing_Tiny_Byte(benchmark::State& state) {
  benchExistingApproachPumpTo(state, TINY_CHUNK_SIZE, TINY_NUM_CHUNKS, BYTE_DEFAULT);
}
static void New_Small_Byte(benchmark::State& state) {
  benchNewApproachPumpTo(state, SMALL_CHUNK_SIZE, SMALL_NUM_CHUNKS, BYTE_DEFAULT);
}
static void Existing_Small_Byte(benchmark::State& state) {
  benchExistingApproachPumpTo(state, SMALL_CHUNK_SIZE, SMALL_NUM_CHUNKS, BYTE_DEFAULT);
}
static void New_Medium_Byte(benchmark::State& state) {
  benchNewApproachPumpTo(state, MEDIUM_CHUNK_SIZE, MEDIUM_NUM_CHUNKS, BYTE_DEFAULT);
}
static void Existing_Medium_Byte(benchmark::State& state) {
  benchExistingApproachPumpTo(state, MEDIUM_CHUNK_SIZE, MEDIUM_NUM_CHUNKS, BYTE_DEFAULT);
}
static void New_Large_Byte(benchmark::State& state) {
  benchNewApproachPumpTo(state, LARGE_CHUNK_SIZE, LARGE_NUM_CHUNKS, BYTE_DEFAULT);
}
static void Existing_Large_Byte(benchmark::State& state) {
  benchExistingApproachPumpTo(state, LARGE_CHUNK_SIZE, LARGE_NUM_CHUNKS, BYTE_DEFAULT);
}

// Byte stream, 16KB HWM, no autoAllocate
static void New_Tiny_Byte_HWM16K(benchmark::State& state) {
  benchNewApproachPumpTo(state, TINY_CHUNK_SIZE, TINY_NUM_CHUNKS, BYTE_HWM_16K);
}
static void Existing_Tiny_Byte_HWM16K(benchmark::State& state) {
  benchExistingApproachPumpTo(state, TINY_CHUNK_SIZE, TINY_NUM_CHUNKS, BYTE_HWM_16K);
}
static void New_Small_Byte_HWM16K(benchmark::State& state) {
  benchNewApproachPumpTo(state, SMALL_CHUNK_SIZE, SMALL_NUM_CHUNKS, BYTE_HWM_16K);
}
static void Existing_Small_Byte_HWM16K(benchmark::State& state) {
  benchExistingApproachPumpTo(state, SMALL_CHUNK_SIZE, SMALL_NUM_CHUNKS, BYTE_HWM_16K);
}
static void New_Medium_Byte_HWM16K(benchmark::State& state) {
  benchNewApproachPumpTo(state, MEDIUM_CHUNK_SIZE, MEDIUM_NUM_CHUNKS, BYTE_HWM_16K);
}
static void Existing_Medium_Byte_HWM16K(benchmark::State& state) {
  benchExistingApproachPumpTo(state, MEDIUM_CHUNK_SIZE, MEDIUM_NUM_CHUNKS, BYTE_HWM_16K);
}
static void New_Large_Byte_HWM16K(benchmark::State& state) {
  benchNewApproachPumpTo(state, LARGE_CHUNK_SIZE, LARGE_NUM_CHUNKS, BYTE_HWM_16K);
}
static void Existing_Large_Byte_HWM16K(benchmark::State& state) {
  benchExistingApproachPumpTo(state, LARGE_CHUNK_SIZE, LARGE_NUM_CHUNKS, BYTE_HWM_16K);
}

// =============================================================================
// Benchmark functions - Byte streams with autoAllocate=64KB
// =============================================================================

// Byte stream, default HWM, autoAllocate=64KB
static void New_Tiny_Byte_Auto64K(benchmark::State& state) {
  benchNewApproachPumpTo(state, TINY_CHUNK_SIZE, TINY_NUM_CHUNKS, BYTE_AUTO_64K);
}
static void Existing_Tiny_Byte_Auto64K(benchmark::State& state) {
  benchExistingApproachPumpTo(state, TINY_CHUNK_SIZE, TINY_NUM_CHUNKS, BYTE_AUTO_64K);
}
static void New_Small_Byte_Auto64K(benchmark::State& state) {
  benchNewApproachPumpTo(state, SMALL_CHUNK_SIZE, SMALL_NUM_CHUNKS, BYTE_AUTO_64K);
}
static void Existing_Small_Byte_Auto64K(benchmark::State& state) {
  benchExistingApproachPumpTo(state, SMALL_CHUNK_SIZE, SMALL_NUM_CHUNKS, BYTE_AUTO_64K);
}
static void New_Medium_Byte_Auto64K(benchmark::State& state) {
  benchNewApproachPumpTo(state, MEDIUM_CHUNK_SIZE, MEDIUM_NUM_CHUNKS, BYTE_AUTO_64K);
}
static void Existing_Medium_Byte_Auto64K(benchmark::State& state) {
  benchExistingApproachPumpTo(state, MEDIUM_CHUNK_SIZE, MEDIUM_NUM_CHUNKS, BYTE_AUTO_64K);
}
static void New_Large_Byte_Auto64K(benchmark::State& state) {
  benchNewApproachPumpTo(state, LARGE_CHUNK_SIZE, LARGE_NUM_CHUNKS, BYTE_AUTO_64K);
}
static void Existing_Large_Byte_Auto64K(benchmark::State& state) {
  benchExistingApproachPumpTo(state, LARGE_CHUNK_SIZE, LARGE_NUM_CHUNKS, BYTE_AUTO_64K);
}

// Byte stream, 16KB HWM, autoAllocate=64KB
static void New_Tiny_Byte_Auto64K_HWM16K(benchmark::State& state) {
  benchNewApproachPumpTo(state, TINY_CHUNK_SIZE, TINY_NUM_CHUNKS, BYTE_AUTO_64K_HWM_16K);
}
static void Existing_Tiny_Byte_Auto64K_HWM16K(benchmark::State& state) {
  benchExistingApproachPumpTo(state, TINY_CHUNK_SIZE, TINY_NUM_CHUNKS, BYTE_AUTO_64K_HWM_16K);
}
static void New_Small_Byte_Auto64K_HWM16K(benchmark::State& state) {
  benchNewApproachPumpTo(state, SMALL_CHUNK_SIZE, SMALL_NUM_CHUNKS, BYTE_AUTO_64K_HWM_16K);
}
static void Existing_Small_Byte_Auto64K_HWM16K(benchmark::State& state) {
  benchExistingApproachPumpTo(state, SMALL_CHUNK_SIZE, SMALL_NUM_CHUNKS, BYTE_AUTO_64K_HWM_16K);
}
static void New_Medium_Byte_Auto64K_HWM16K(benchmark::State& state) {
  benchNewApproachPumpTo(state, MEDIUM_CHUNK_SIZE, MEDIUM_NUM_CHUNKS, BYTE_AUTO_64K_HWM_16K);
}
static void Existing_Medium_Byte_Auto64K_HWM16K(benchmark::State& state) {
  benchExistingApproachPumpTo(state, MEDIUM_CHUNK_SIZE, MEDIUM_NUM_CHUNKS, BYTE_AUTO_64K_HWM_16K);
}
static void New_Large_Byte_Auto64K_HWM16K(benchmark::State& state) {
  benchNewApproachPumpTo(state, LARGE_CHUNK_SIZE, LARGE_NUM_CHUNKS, BYTE_AUTO_64K_HWM_16K);
}
static void Existing_Large_Byte_Auto64K_HWM16K(benchmark::State& state) {
  benchExistingApproachPumpTo(state, LARGE_CHUNK_SIZE, LARGE_NUM_CHUNKS, BYTE_AUTO_64K_HWM_16K);
}

// =============================================================================
// Benchmark functions - Slow value streams (async with microtask delays)
// =============================================================================

// Slow value stream - these produce one chunk per microtask, adding processing overhead.
// Note: This does NOT trigger the adaptive read policy because microtask delays don't
// cause early returns from pumpReadImpl. The policy would only activate with real I/O
// latency that blocks the KJ event loop. See PUMP_PERFORMANCE_ANALYSIS.md section 9.
static void New_Small_SlowValue(benchmark::State& state) {
  benchNewApproachPumpTo(state, SMALL_CHUNK_SIZE, SMALL_NUM_CHUNKS, SLOW_VALUE_DEFAULT);
}
static void Existing_Small_SlowValue(benchmark::State& state) {
  benchExistingApproachPumpTo(state, SMALL_CHUNK_SIZE, SMALL_NUM_CHUNKS, SLOW_VALUE_DEFAULT);
}
static void New_Medium_SlowValue(benchmark::State& state) {
  benchNewApproachPumpTo(state, MEDIUM_CHUNK_SIZE, MEDIUM_NUM_CHUNKS, SLOW_VALUE_DEFAULT);
}
static void Existing_Medium_SlowValue(benchmark::State& state) {
  benchExistingApproachPumpTo(state, MEDIUM_CHUNK_SIZE, MEDIUM_NUM_CHUNKS, SLOW_VALUE_DEFAULT);
}

// =============================================================================
// Benchmark functions - I/O latency streams (real KJ event loop yields)
// =============================================================================

// I/O latency streams yield to the KJ event loop between chunks, simulating real network I/O.
// This tests how the pump behaves with streams that have actual I/O latency,
// unlike microtask-based "slow" streams which complete within one JS event loop turn.
//
// Key differences from SlowValue:
// - Each chunk requires a KJ event loop iteration (not just a microtask)
// - pumpReadImpl returns early after each chunk

// I/O latency value streams
static void New_Small_IoLatencyValue(benchmark::State& state) {
  benchNewApproachPumpTo(state, SMALL_CHUNK_SIZE, SMALL_NUM_CHUNKS, IO_LATENCY_VALUE_DEFAULT);
}
static void Existing_Small_IoLatencyValue(benchmark::State& state) {
  benchExistingApproachPumpTo(state, SMALL_CHUNK_SIZE, SMALL_NUM_CHUNKS, IO_LATENCY_VALUE_DEFAULT);
}
static void New_Medium_IoLatencyValue(benchmark::State& state) {
  benchNewApproachPumpTo(state, MEDIUM_CHUNK_SIZE, MEDIUM_NUM_CHUNKS, IO_LATENCY_VALUE_DEFAULT);
}
static void Existing_Medium_IoLatencyValue(benchmark::State& state) {
  benchExistingApproachPumpTo(
      state, MEDIUM_CHUNK_SIZE, MEDIUM_NUM_CHUNKS, IO_LATENCY_VALUE_DEFAULT);
}
static void New_Large_IoLatencyValue(benchmark::State& state) {
  benchNewApproachPumpTo(state, LARGE_CHUNK_SIZE, LARGE_NUM_CHUNKS, IO_LATENCY_VALUE_DEFAULT);
}
static void Existing_Large_IoLatencyValue(benchmark::State& state) {
  benchExistingApproachPumpTo(state, LARGE_CHUNK_SIZE, LARGE_NUM_CHUNKS, IO_LATENCY_VALUE_DEFAULT);
}

// I/O latency byte streams
static void New_Small_IoLatencyByte(benchmark::State& state) {
  benchNewApproachPumpTo(state, SMALL_CHUNK_SIZE, SMALL_NUM_CHUNKS, IO_LATENCY_BYTE_DEFAULT);
}
static void Existing_Small_IoLatencyByte(benchmark::State& state) {
  benchExistingApproachPumpTo(state, SMALL_CHUNK_SIZE, SMALL_NUM_CHUNKS, IO_LATENCY_BYTE_DEFAULT);
}
static void New_Medium_IoLatencyByte(benchmark::State& state) {
  benchNewApproachPumpTo(state, MEDIUM_CHUNK_SIZE, MEDIUM_NUM_CHUNKS, IO_LATENCY_BYTE_DEFAULT);
}
static void Existing_Medium_IoLatencyByte(benchmark::State& state) {
  benchExistingApproachPumpTo(state, MEDIUM_CHUNK_SIZE, MEDIUM_NUM_CHUNKS, IO_LATENCY_BYTE_DEFAULT);
}
static void New_Large_IoLatencyByte(benchmark::State& state) {
  benchNewApproachPumpTo(state, LARGE_CHUNK_SIZE, LARGE_NUM_CHUNKS, IO_LATENCY_BYTE_DEFAULT);
}
static void Existing_Large_IoLatencyByte(benchmark::State& state) {
  benchExistingApproachPumpTo(state, LARGE_CHUNK_SIZE, LARGE_NUM_CHUNKS, IO_LATENCY_BYTE_DEFAULT);
}

// =============================================================================
// Benchmark functions - Timed value streams (real timer-based delays)
// =============================================================================

// These benchmarks use actual timer delays to simulate real network I/O.
// Unlike evalLater() which resumes immediately, these represent real wall-clock time.
// We test with small chunks to see how latency affects batching behavior.

// 10μs delay per chunk (1ms total for 100 chunks)
static void New_Small_Timed10us(benchmark::State& state) {
  benchNewApproachPumpTo(state, SMALL_CHUNK_SIZE, SMALL_NUM_CHUNKS, TIMED_VALUE_10US);
}
static void Existing_Small_Timed10us(benchmark::State& state) {
  benchExistingApproachPumpTo(state, SMALL_CHUNK_SIZE, SMALL_NUM_CHUNKS, TIMED_VALUE_10US);
}

// 100μs delay per chunk (10ms total for 100 chunks)
static void New_Small_Timed100us(benchmark::State& state) {
  benchNewApproachPumpTo(state, SMALL_CHUNK_SIZE, SMALL_NUM_CHUNKS, TIMED_VALUE_100US);
}
static void Existing_Small_Timed100us(benchmark::State& state) {
  benchExistingApproachPumpTo(state, SMALL_CHUNK_SIZE, SMALL_NUM_CHUNKS, TIMED_VALUE_100US);
}

// 1ms delay per chunk (100ms total for 100 chunks) - very slow, representative of slow network
static void New_Small_Timed1ms(benchmark::State& state) {
  benchNewApproachPumpTo(state, SMALL_CHUNK_SIZE, SMALL_NUM_CHUNKS, TIMED_VALUE_1MS);
}
static void Existing_Small_Timed1ms(benchmark::State& state) {
  benchExistingApproachPumpTo(state, SMALL_CHUNK_SIZE, SMALL_NUM_CHUNKS, TIMED_VALUE_1MS);
}

// Medium chunks with 100μs delay - tests larger chunk batching with latency
static void New_Medium_Timed100us(benchmark::State& state) {
  benchNewApproachPumpTo(state, MEDIUM_CHUNK_SIZE, MEDIUM_NUM_CHUNKS, TIMED_VALUE_100US);
}
static void Existing_Medium_Timed100us(benchmark::State& state) {
  benchExistingApproachPumpTo(state, MEDIUM_CHUNK_SIZE, MEDIUM_NUM_CHUNKS, TIMED_VALUE_100US);
}

// =============================================================================
// Register benchmarks - organized by chunk size for easy comparison
// =============================================================================

// Tiny chunks - all configurations
WD_BENCHMARK(New_Tiny_Value);
WD_BENCHMARK(Existing_Tiny_Value);
WD_BENCHMARK(New_Tiny_Value_HWM16K);
WD_BENCHMARK(Existing_Tiny_Value_HWM16K);
WD_BENCHMARK(New_Tiny_Byte);
WD_BENCHMARK(Existing_Tiny_Byte);
WD_BENCHMARK(New_Tiny_Byte_HWM16K);
WD_BENCHMARK(Existing_Tiny_Byte_HWM16K);
WD_BENCHMARK(New_Tiny_Byte_Auto64K);
WD_BENCHMARK(Existing_Tiny_Byte_Auto64K);
WD_BENCHMARK(New_Tiny_Byte_Auto64K_HWM16K);
WD_BENCHMARK(Existing_Tiny_Byte_Auto64K_HWM16K);

// Small chunks - all configurations
WD_BENCHMARK(New_Small_Value);
WD_BENCHMARK(Existing_Small_Value);
WD_BENCHMARK(New_Small_Value_HWM16K);
WD_BENCHMARK(Existing_Small_Value_HWM16K);
WD_BENCHMARK(New_Small_Byte);
WD_BENCHMARK(Existing_Small_Byte);
WD_BENCHMARK(New_Small_Byte_HWM16K);
WD_BENCHMARK(Existing_Small_Byte_HWM16K);
WD_BENCHMARK(New_Small_Byte_Auto64K);
WD_BENCHMARK(Existing_Small_Byte_Auto64K);
WD_BENCHMARK(New_Small_Byte_Auto64K_HWM16K);
WD_BENCHMARK(Existing_Small_Byte_Auto64K_HWM16K);

// Medium chunks - all configurations
WD_BENCHMARK(New_Medium_Value);
WD_BENCHMARK(Existing_Medium_Value);
WD_BENCHMARK(New_Medium_Value_HWM16K);
WD_BENCHMARK(Existing_Medium_Value_HWM16K);
WD_BENCHMARK(New_Medium_Byte);
WD_BENCHMARK(Existing_Medium_Byte);
WD_BENCHMARK(New_Medium_Byte_HWM16K);
WD_BENCHMARK(Existing_Medium_Byte_HWM16K);
WD_BENCHMARK(New_Medium_Byte_Auto64K);
WD_BENCHMARK(Existing_Medium_Byte_Auto64K);
WD_BENCHMARK(New_Medium_Byte_Auto64K_HWM16K);
WD_BENCHMARK(Existing_Medium_Byte_Auto64K_HWM16K);

// Large chunks - all configurations
WD_BENCHMARK(New_Large_Value);
WD_BENCHMARK(Existing_Large_Value);
WD_BENCHMARK(New_Large_Value_HWM16K);
WD_BENCHMARK(Existing_Large_Value_HWM16K);
WD_BENCHMARK(New_Large_Byte);
WD_BENCHMARK(Existing_Large_Byte);
WD_BENCHMARK(New_Large_Byte_HWM16K);
WD_BENCHMARK(Existing_Large_Byte_HWM16K);
WD_BENCHMARK(New_Large_Byte_Auto64K);
WD_BENCHMARK(Existing_Large_Byte_Auto64K);
WD_BENCHMARK(New_Large_Byte_Auto64K_HWM16K);
WD_BENCHMARK(Existing_Large_Byte_Auto64K_HWM16K);

// Huge chunks - exercises leftover mechanism (512KB chunks > buffer size)
WD_BENCHMARK(New_Huge_Value);
WD_BENCHMARK(Existing_Huge_Value);

// Slow value stream - async streams with microtask delays (tests batching overhead)
WD_BENCHMARK(New_Small_SlowValue);
WD_BENCHMARK(Existing_Small_SlowValue);
WD_BENCHMARK(New_Medium_SlowValue);
WD_BENCHMARK(Existing_Medium_SlowValue);

// I/O latency streams - real KJ event loop yields (simulates network I/O)
// These test how the adaptive read policy behaves with actual I/O latency
WD_BENCHMARK(New_Small_IoLatencyValue);
WD_BENCHMARK(Existing_Small_IoLatencyValue);
WD_BENCHMARK(New_Medium_IoLatencyValue);
WD_BENCHMARK(Existing_Medium_IoLatencyValue);
WD_BENCHMARK(New_Large_IoLatencyValue);
WD_BENCHMARK(Existing_Large_IoLatencyValue);
WD_BENCHMARK(New_Small_IoLatencyByte);
WD_BENCHMARK(Existing_Small_IoLatencyByte);
WD_BENCHMARK(New_Medium_IoLatencyByte);
WD_BENCHMARK(Existing_Medium_IoLatencyByte);
WD_BENCHMARK(New_Large_IoLatencyByte);
WD_BENCHMARK(Existing_Large_IoLatencyByte);

// Timed stream benchmarks - uses real timers via useRealTimers=true in SetupParams.
// These simulate actual blocking I/O with timer delays between chunks.
WD_BENCHMARK(New_Small_Timed10us);
WD_BENCHMARK(Existing_Small_Timed10us);
WD_BENCHMARK(New_Small_Timed100us);
WD_BENCHMARK(Existing_Small_Timed100us);
WD_BENCHMARK(New_Small_Timed1ms);
WD_BENCHMARK(Existing_Small_Timed1ms);
WD_BENCHMARK(New_Medium_Timed100us);
WD_BENCHMARK(Existing_Medium_Timed100us);

}  // namespace
}  // namespace workerd::api::streams
