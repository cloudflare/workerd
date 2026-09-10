// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Benchmarks for the kj::AsyncInputStream::tryReadSync() / kj::AsyncOutputStream::tryWriteSync()
// synchronous stream fast paths.
//
// Each scenario is measured in paired variants within the same binary:
//   *_Async:   forces the promise-based path (identical to the behavior before the synchronous
//              fast paths existed, since the async code paths were not modified)
//   *_TrySync: uses the try-sync-then-fallback pattern
// so a direct comparison requires no baseline build.
//
// Scenarios:
//   - PipeRendezvousWrite: write into an in-memory pipe with a pending read. The write is
//     already a memcpy either way; the delta is pure promise machinery.
//   - PipeRendezvousRead: read from an in-memory pipe with a pending (blocked) write.
//   - Pump: kj::unoptimizedPumpTo() between an in-memory chunked source and a discarding sink,
//     in three flavors: fully async (both fast paths hidden), sync reads + async writes, and
//     fully sync.
//   - GzipDecode: decompress a payload whose compressed bytes are served from memory. The
//     TrySync variant decompresses from the decompressor's internal buffer synchronously.
//
// Usage:
//   bazel run --config=opt //src/workerd/tests:bench-try-sync
//
// Key metrics: bytes_per_second (throughput), plus syncReads/asyncReads counters on the gzip
// benchmarks showing how often the fast path engaged.

#include <workerd/tests/bench-tools.h>

#include <kj/async-io.h>
#include <kj/compat/gzip.h>
#include <kj/debug.h>
#include <kj/io.h>

namespace workerd {
namespace {

// =============================================================================
// Test utilities
// =============================================================================

// An in-memory input stream that serves at most `chunkSize` bytes per read, to force pump
// loops to actually iterate. Supports the synchronous fast path.
class ChunkedSource final: public kj::AsyncInputStream {
 public:
  ChunkedSource(kj::ArrayPtr<const kj::byte> data, size_t chunkSize)
      : data(data),
        chunkSize(chunkSize) {}

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    return readImpl(kj::arrayPtr(static_cast<kj::byte*>(buffer), maxBytes));
  }

  kj::Maybe<size_t> tryReadSync(kj::ArrayPtr<kj::byte> buffer, size_t minBytes) override {
    return readImpl(buffer);
  }

 private:
  size_t readImpl(kj::ArrayPtr<kj::byte> buffer) {
    size_t n = kj::min(kj::min(chunkSize, buffer.size()), data.size());
    buffer.write(data.first(n));
    data = data.slice(n);
    return n;
  }

  kj::ArrayPtr<const kj::byte> data;
  size_t chunkSize;
};

// Wrapper which hides an inner stream's synchronous fast path (and custom pumpTo), forcing the
// fully-asynchronous code path, for baseline comparison.
class HideSync final: public kj::AsyncInputStream {
 public:
  explicit HideSync(kj::AsyncInputStream& inner): inner(inner) {}

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    return inner.tryRead(buffer, minBytes, maxBytes);
  }

  // tryReadSync() is deliberately not overridden; the inherited default always declines.

 private:
  kj::AsyncInputStream& inner;
};

// A discarding sink without a synchronous fast path. Its write() returns an
// already-resolved promise, so this represents the best possible async-path sink.
struct DiscardingSink: public kj::AsyncOutputStream {
  size_t bytesWritten = 0;

  kj::Promise<void> write(kj::ArrayPtr<const kj::byte> buffer) override {
    bytesWritten += buffer.size();
    return kj::READY_NOW;
  }
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    for (auto& piece: pieces) bytesWritten += piece.size();
    return kj::READY_NOW;
  }
  kj::Promise<void> whenWriteDisconnected() override {
    return kj::NEVER_DONE;
  }
};

// A discarding sink with the synchronous fast path.
struct SyncDiscardingSink final: public DiscardingSink {
  bool tryWriteSync(kj::ArrayPtr<const kj::byte> buffer) override {
    bytesWritten += buffer.size();
    return true;
  }
  bool tryWriteSync(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    for (auto& piece: pieces) bytesWritten += piece.size();
    return true;
  }
};

kj::Array<kj::byte> makeCompressiblePayload(size_t size) {
  constexpr kj::StringPtr PATTERN = "The quick brown fox jumps over the lazy dog. "_kj;
  auto data = kj::heapArray<kj::byte>(size);
  for (auto i: kj::indices(data)) {
    data[i] = PATTERN[i % PATTERN.size()];
  }
  return data;
}

kj::Array<kj::byte> gzipCompress(kj::ArrayPtr<const kj::byte> data) {
  kj::VectorOutputStream out;
  {
    kj::GzipOutputStream gzip(out);
    gzip.write(data);
    // Flushed and finished by the destructor.
  }
  return kj::heapArray<kj::byte>(out.getArray());
}

// =============================================================================
// Pipe rendezvous: write into a pending read
// =============================================================================

void pipeRendezvousWrite(
    benchmark::State& state, size_t chunkSize, size_t numChunks, bool useSync) {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  auto pipe = kj::newOneWayPipe();
  auto chunk = kj::heapArray<kj::byte>(chunkSize);
  chunk.asPtr().fill(0xAB);
  auto readBuf = kj::heapArray<kj::byte>(chunkSize);

  for (auto _: state) {
    for (size_t i = 0; i < numChunks; i++) {
      auto readPromise = pipe.in->tryRead(readBuf.begin(), chunkSize, chunkSize);
      if (useSync) {
        if (!pipe.out->tryWriteSync(chunk)) {
          pipe.out->write(chunk).wait(ws);
        }
      } else {
        pipe.out->write(chunk).wait(ws);
      }
      KJ_ASSERT(readPromise.wait(ws) == chunkSize);
    }
  }
  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(chunkSize * numChunks));
}

void PipeRendezvousWrite_64B_Async(benchmark::State& state) {
  pipeRendezvousWrite(state, 64, 4096, false);
}
void PipeRendezvousWrite_64B_TrySync(benchmark::State& state) {
  pipeRendezvousWrite(state, 64, 4096, true);
}
void PipeRendezvousWrite_4KB_Async(benchmark::State& state) {
  pipeRendezvousWrite(state, 4096, 256, false);
}
void PipeRendezvousWrite_4KB_TrySync(benchmark::State& state) {
  pipeRendezvousWrite(state, 4096, 256, true);
}

// =============================================================================
// Pipe rendezvous: read from a pending (blocked) write
// =============================================================================

void pipeRendezvousRead(benchmark::State& state, size_t chunkSize, size_t numChunks, bool useSync) {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  auto pipe = kj::newOneWayPipe();
  auto chunk = kj::heapArray<kj::byte>(chunkSize);
  chunk.asPtr().fill(0xAB);
  auto readBuf = kj::heapArray<kj::byte>(chunkSize);

  for (auto _: state) {
    for (size_t i = 0; i < numChunks; i++) {
      auto writePromise = pipe.out->write(chunk);
      if (useSync) {
        KJ_IF_SOME(n, pipe.in->tryReadSync(readBuf, 1)) {
          KJ_ASSERT(n == chunkSize);
        } else {
          KJ_ASSERT(pipe.in->tryRead(readBuf.begin(), 1, chunkSize).wait(ws) == chunkSize);
        }
      } else {
        KJ_ASSERT(pipe.in->tryRead(readBuf.begin(), 1, chunkSize).wait(ws) == chunkSize);
      }
      writePromise.wait(ws);
    }
  }
  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(chunkSize * numChunks));
}

void PipeRendezvousRead_64B_Async(benchmark::State& state) {
  pipeRendezvousRead(state, 64, 4096, false);
}
void PipeRendezvousRead_64B_TrySync(benchmark::State& state) {
  pipeRendezvousRead(state, 64, 4096, true);
}
void PipeRendezvousRead_4KB_Async(benchmark::State& state) {
  pipeRendezvousRead(state, 4096, 256, false);
}
void PipeRendezvousRead_4KB_TrySync(benchmark::State& state) {
  pipeRendezvousRead(state, 4096, 256, true);
}

// =============================================================================
// unoptimizedPumpTo: 1 MiB through the naive pump loop
// =============================================================================

enum class PumpMode {
  FULL_ASYNC,  // read fast path hidden, sink has no fast path
  SYNC_READ,   // synchronous reads, async writes
  FULL_SYNC,   // synchronous reads and writes
};

void pumpBench(benchmark::State& state, PumpMode mode) {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  constexpr size_t CHUNK_SIZE = 4096;  // matches the pump loop's internal buffer
  constexpr size_t TOTAL = 1 << 20;    // 1 MiB

  auto payload = kj::heapArray<kj::byte>(TOTAL);
  payload.asPtr().fill(0xAB);

  DiscardingSink asyncSink;
  SyncDiscardingSink syncSink;
  DiscardingSink& sink =
      mode == PumpMode::FULL_SYNC ? static_cast<DiscardingSink&>(syncSink) : asyncSink;

  for (auto _: state) {
    ChunkedSource source(payload, CHUNK_SIZE);
    if (mode == PumpMode::FULL_ASYNC) {
      HideSync hidden(source);
      KJ_ASSERT(kj::unoptimizedPumpTo(hidden, sink, TOTAL).wait(ws) == TOTAL);
    } else {
      KJ_ASSERT(kj::unoptimizedPumpTo(source, sink, TOTAL).wait(ws) == TOTAL);
    }
  }
  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(TOTAL));
}

void Pump_1MB_FullAsync(benchmark::State& state) {
  pumpBench(state, PumpMode::FULL_ASYNC);
}
void Pump_1MB_SyncRead(benchmark::State& state) {
  pumpBench(state, PumpMode::SYNC_READ);
}
void Pump_1MB_FullSync(benchmark::State& state) {
  pumpBench(state, PumpMode::FULL_SYNC);
}

// =============================================================================
// Gzip decode of buffered compressed data
// =============================================================================

void gzipDecodeBench(benchmark::State& state, bool useSync) {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  constexpr size_t TOTAL = 1 << 20;  // 1 MiB decompressed
  auto raw = makeCompressiblePayload(TOTAL);
  auto compressed = gzipCompress(raw);

  kj::byte buf[16384];
  size_t syncReads = 0;
  size_t asyncReads = 0;

  for (auto _: state) {
    ChunkedSource source(compressed, kj::maxValue);
    kj::GzipAsyncInputStream gzip(source);

    uint64_t total = 0;
    for (;;) {
      size_t n;
      if (useSync) {
        KJ_IF_SOME(sn, gzip.tryReadSync(kj::arrayPtr(buf), 1)) {
          n = sn;
          syncReads++;
        } else {
          n = gzip.tryRead(buf, 1, sizeof(buf)).wait(ws);
          asyncReads++;
        }
      } else {
        n = gzip.tryRead(buf, 1, sizeof(buf)).wait(ws);
        asyncReads++;
      }
      if (n == 0) break;
      total += n;
    }
    KJ_ASSERT(total == TOTAL);
  }
  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(TOTAL));
  state.counters["syncReads"] = benchmark::Counter(syncReads, benchmark::Counter::kAvgIterations);
  state.counters["asyncReads"] = benchmark::Counter(asyncReads, benchmark::Counter::kAvgIterations);
}

void GzipDecode_1MB_Async(benchmark::State& state) {
  gzipDecodeBench(state, false);
}
void GzipDecode_1MB_TrySync(benchmark::State& state) {
  gzipDecodeBench(state, true);
}

// =============================================================================
// Register benchmarks
// =============================================================================

WD_BENCHMARK(PipeRendezvousWrite_64B_Async);
WD_BENCHMARK(PipeRendezvousWrite_64B_TrySync);
WD_BENCHMARK(PipeRendezvousWrite_4KB_Async);
WD_BENCHMARK(PipeRendezvousWrite_4KB_TrySync);

WD_BENCHMARK(PipeRendezvousRead_64B_Async);
WD_BENCHMARK(PipeRendezvousRead_64B_TrySync);
WD_BENCHMARK(PipeRendezvousRead_4KB_Async);
WD_BENCHMARK(PipeRendezvousRead_4KB_TrySync);

WD_BENCHMARK(Pump_1MB_FullAsync);
WD_BENCHMARK(Pump_1MB_SyncRead);
WD_BENCHMARK(Pump_1MB_FullSync);

WD_BENCHMARK(GzipDecode_1MB_Async);
WD_BENCHMARK(GzipDecode_1MB_TrySync);

}  // namespace
}  // namespace workerd
