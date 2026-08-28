// Benchmarks the native (C) brotli against rust-brotli on the kj-http content-encoding
// streaming workload: 1MB of compressible data in 16KB chunks with kj's default parameters
// (quality 5, lgwin 19). The native arm goes through the unprefixed (routed) API, which
// resolves to the C implementation since benchmarks run with the compression-rs autogate off;
// the rust arm calls rust-brotli directly through its brotli_rs_* symbols.

#include <workerd/tests/bench-tools.h>

#include <brotli/decode.h>
#include <brotli/encode.h>

#include <kj/array.h>
#include <kj/debug.h>

extern "C" {
void* brotli_rs_BrotliEncoderCreateInstance(void*, void*, void*);
void brotli_rs_BrotliEncoderDestroyInstance(void* state);
int brotli_rs_BrotliEncoderSetParameter(void* state, int param, uint32_t value);
int brotli_rs_BrotliEncoderCompressStream(void* state,
    int op,
    size_t* availableIn,
    const uint8_t** nextIn,
    size_t* availableOut,
    uint8_t** nextOut,
    size_t* totalOut);
int brotli_rs_BrotliEncoderIsFinished(void* state);
void* brotli_rs_BrotliDecoderCreateInstance(void*, void*, void*);
void brotli_rs_BrotliDecoderDestroyInstance(void* state);
int brotli_rs_BrotliDecoderDecompressStream(void* state,
    size_t* availableIn,
    const uint8_t** nextIn,
    size_t* availableOut,
    uint8_t** nextOut,
    size_t* totalOut);
}

namespace workerd {
namespace {

constexpr size_t DATA_SIZE = 1 << 20;
constexpr size_t CHUNK = 16 * 1024;
constexpr int QUALITY = 5;
constexpr int LGWIN = 19;

kj::Array<kj::byte> makeCompressibleData() {
  static constexpr kj::StringPtr words[] = {"the "_kj, "quick "_kj, "compression "_kj, "of "_kj,
    "brotli "_kj, "streaming "_kj, "workers "_kj, "runtime "_kj};
  auto data = kj::heapArray<kj::byte>(DATA_SIZE);
  uint32_t s = 0x12345678;
  size_t pos = 0;
  while (pos < data.size()) {
    s = s * 1664525 + 1013904223;
    auto w = words[(s >> 24) & 7].asBytes();
    size_t n = kj::min(w.size(), data.size() - pos);
    memcpy(data.begin() + pos, w.begin(), n);
    pos += n;
  }
  return data;
}

const kj::Array<kj::byte>& sourceData() {
  static const kj::Array<kj::byte> data = makeCompressibleData();
  return data;
}

// Streaming via a generic stream-call shape shared by both backends. The run callback returns
// whether the stream is finished.
template <typename RunFn>
size_t streamAll(kj::ArrayPtr<const kj::byte> data, RunFn&& run) {
  static kj::byte out[CHUNK];
  size_t total = 0;
  size_t offset = 0;
  while (offset < data.size()) {
    size_t n = kj::min(CHUNK, data.size() - offset);
    bool last = offset + n == data.size();
    const kj::byte* nextIn = data.begin() + offset;
    size_t availIn = n;
    offset += n;
    do {
      kj::byte* nextOut = out;
      size_t availOut = sizeof(out);
      bool done = run(&nextIn, &availIn, &nextOut, &availOut, last);
      total += sizeof(out) - availOut;
      if (done) return total;
    } while (availIn > 0 || last);
  }
  return total;
}

size_t compressNative(kj::ArrayPtr<const kj::byte> data) {
  BrotliEncoderState* ctx = BrotliEncoderCreateInstance(nullptr, nullptr, nullptr);
  KJ_REQUIRE(ctx != nullptr);
  KJ_DEFER(BrotliEncoderDestroyInstance(ctx));
  KJ_REQUIRE(BrotliEncoderSetParameter(ctx, BROTLI_PARAM_QUALITY, QUALITY) == BROTLI_TRUE);
  KJ_REQUIRE(BrotliEncoderSetParameter(ctx, BROTLI_PARAM_LGWIN, LGWIN) == BROTLI_TRUE);
  return streamAll(data,
      [&](const kj::byte** nextIn, size_t* availIn, kj::byte** nextOut, size_t* availOut,
          bool last) {
    KJ_REQUIRE(
        BrotliEncoderCompressStream(ctx, last ? BROTLI_OPERATION_FINISH : BROTLI_OPERATION_PROCESS,
            availIn, nextIn, availOut, nextOut, nullptr) == BROTLI_TRUE);
    return BrotliEncoderIsFinished(ctx) == BROTLI_TRUE;
  });
}

size_t compressBrotliRs(kj::ArrayPtr<const kj::byte> data) {
  void* ctx = brotli_rs_BrotliEncoderCreateInstance(nullptr, nullptr, nullptr);
  KJ_REQUIRE(ctx != nullptr);
  KJ_DEFER(brotli_rs_BrotliEncoderDestroyInstance(ctx));
  KJ_REQUIRE(brotli_rs_BrotliEncoderSetParameter(ctx, BROTLI_PARAM_QUALITY, QUALITY) == 1);
  KJ_REQUIRE(brotli_rs_BrotliEncoderSetParameter(ctx, BROTLI_PARAM_LGWIN, LGWIN) == 1);
  return streamAll(data,
      [&](const kj::byte** nextIn, size_t* availIn, kj::byte** nextOut, size_t* availOut,
          bool last) {
    KJ_REQUIRE(brotli_rs_BrotliEncoderCompressStream(ctx,
                   last ? BROTLI_OPERATION_FINISH : BROTLI_OPERATION_PROCESS, availIn, nextIn,
                   availOut, nextOut, nullptr) == 1);
    return brotli_rs_BrotliEncoderIsFinished(ctx) == 1;
  });
}

size_t decompressNative(kj::ArrayPtr<const kj::byte> data) {
  BrotliDecoderState* ctx = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
  KJ_REQUIRE(ctx != nullptr);
  KJ_DEFER(BrotliDecoderDestroyInstance(ctx));
  return streamAll(data,
      [&](const kj::byte** nextIn, size_t* availIn, kj::byte** nextOut, size_t* availOut, bool) {
    BrotliDecoderResult result =
        BrotliDecoderDecompressStream(ctx, availIn, nextIn, availOut, nextOut, nullptr);
    KJ_REQUIRE(result != BROTLI_DECODER_RESULT_ERROR);
    return result == BROTLI_DECODER_RESULT_SUCCESS;
  });
}

size_t decompressBrotliRs(kj::ArrayPtr<const kj::byte> data) {
  void* ctx = brotli_rs_BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
  KJ_REQUIRE(ctx != nullptr);
  KJ_DEFER(brotli_rs_BrotliDecoderDestroyInstance(ctx));
  return streamAll(data,
      [&](const kj::byte** nextIn, size_t* availIn, kj::byte** nextOut, size_t* availOut, bool) {
    int result =
        brotli_rs_BrotliDecoderDecompressStream(ctx, availIn, nextIn, availOut, nextOut, nullptr);
    KJ_REQUIRE(result != BROTLI_DECODER_RESULT_ERROR);
    return result == BROTLI_DECODER_RESULT_SUCCESS;
  });
}

kj::Array<kj::byte> compressedData() {
  auto& src = sourceData();
  size_t destLen = BrotliEncoderMaxCompressedSize(src.size());
  auto dest = kj::heapArray<kj::byte>(destLen);
  KJ_REQUIRE(BrotliEncoderCompress(QUALITY, LGWIN, BROTLI_MODE_GENERIC, src.size(), src.begin(),
                 &destLen, dest.begin()) == BROTLI_TRUE);
  return kj::heapArray<kj::byte>(dest.first(destLen));
}

void Brotli_Compress_Native(benchmark::State& state) {
  auto& src = sourceData();
  for (auto _: state) {
    benchmark::DoNotOptimize(compressNative(src));
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * src.size());
}

void Brotli_Compress_BrotliRs(benchmark::State& state) {
  auto& src = sourceData();
  for (auto _: state) {
    benchmark::DoNotOptimize(compressBrotliRs(src));
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * src.size());
}

void Brotli_Decompress_Native(benchmark::State& state) {
  auto& src = sourceData();
  auto compressed = compressedData();
  for (auto _: state) {
    KJ_ASSERT(decompressNative(compressed) == src.size());
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * src.size());
}

void Brotli_Decompress_BrotliRs(benchmark::State& state) {
  auto& src = sourceData();
  auto compressed = compressedData();
  for (auto _: state) {
    KJ_ASSERT(decompressBrotliRs(compressed) == src.size());
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * src.size());
}

WD_BENCHMARK(Brotli_Compress_Native)->Name("Brotli::Compress::Native");
WD_BENCHMARK(Brotli_Compress_BrotliRs)->Name("Brotli::Compress::BrotliRs");
WD_BENCHMARK(Brotli_Decompress_Native)->Name("Brotli::Decompress::Native");
WD_BENCHMARK(Brotli_Decompress_BrotliRs)->Name("Brotli::Decompress::BrotliRs");

}  // namespace
}  // namespace workerd
