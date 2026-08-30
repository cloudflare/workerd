// Benchmarks native (C) zstd against the Rust decoder (ruzstd) on the
// node:zlib zstd streaming workload: 1MB of compressible data in 16KB chunks
// with the default compression level. Only decompression is routed to Rust, so
// compression is measured on the native implementation only.

#include <workerd/tests/bench-tools.h>

#include <zstd.h>

#include <kj/array.h>
#include <kj/debug.h>

// The Rust decoder, exported under the zstd_rs_ prefix by src/rust/zstd-rs. The unprefixed names
// belong to the routing layer, so the benchmark calls the Rust side directly through these.
extern "C" {
void* zstd_rs_ZSTD_createDCtx(void);
size_t zstd_rs_ZSTD_freeDCtx(void* dctx);
size_t zstd_rs_ZSTD_decompressStream(void* dctx, void* output, void* input);
}

namespace workerd {
namespace {

constexpr size_t DATA_SIZE = 1 << 20;
constexpr size_t CHUNK = 16 * 1024;

kj::Array<kj::byte> makeCompressibleData() {
  static constexpr kj::StringPtr words[] = {"the "_kj, "quick "_kj, "compression "_kj, "of "_kj,
    "zstd "_kj, "streaming "_kj, "workers "_kj, "runtime "_kj};
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

kj::Array<kj::byte> compressedData() {
  auto& src = sourceData();
  auto out = kj::heapArray<kj::byte>(ZSTD_compressBound(src.size()));
  auto* cctx = ZSTD_createCCtx();
  KJ_REQUIRE(cctx != nullptr);
  KJ_DEFER(ZSTD_freeCCtx(cctx));
  ZSTD_inBuffer in{src.begin(), src.size(), 0};
  ZSTD_outBuffer zout{out.begin(), out.size(), 0};
  KJ_REQUIRE(ZSTD_compressStream2(cctx, &zout, &in, ZSTD_e_end) == 0);
  return kj::heapArray<kj::byte>(out.first(zout.pos));
}

size_t compressNative(kj::ArrayPtr<const kj::byte> data) {
  static kj::byte out[CHUNK];
  auto* cctx = ZSTD_createCCtx();
  KJ_REQUIRE(cctx != nullptr);
  KJ_DEFER(ZSTD_freeCCtx(cctx));
  size_t total = 0;
  for (size_t offset = 0; offset < data.size(); offset += CHUNK) {
    size_t n = kj::min(CHUNK, data.size() - offset);
    bool last = offset + n == data.size();
    ZSTD_inBuffer in{data.begin() + offset, n, 0};
    size_t result;
    do {
      ZSTD_outBuffer zout{out, sizeof(out), 0};
      result = ZSTD_compressStream2(cctx, &zout, &in, last ? ZSTD_e_end : ZSTD_e_continue);
      KJ_REQUIRE(!ZSTD_isError(result));
      total += zout.pos;
    } while (in.pos < in.size || (last && result != 0));
  }
  return total;
}

// Streaming via a generic stream-call shape shared by both backends.
template <typename Fns>
size_t streamDecompress(const Fns& fns, kj::ArrayPtr<const kj::byte> data) {
  static kj::byte out[CHUNK];
  auto* dctx = fns.create();
  KJ_REQUIRE(dctx != nullptr);
  KJ_DEFER(fns.free(dctx));
  size_t total = 0;
  size_t result = 1;
  for (size_t offset = 0; offset < data.size(); offset += CHUNK) {
    size_t n = kj::min(CHUNK, data.size() - offset);
    bool last = offset + n == data.size();
    ZSTD_inBuffer in{data.begin() + offset, n, 0};
    do {
      ZSTD_outBuffer zout{out, sizeof(out), 0};
      result = fns.decompress(dctx, &zout, &in);
      KJ_REQUIRE(!ZSTD_isError(result));
      total += zout.pos;
    } while (in.pos < in.size || (last && result != 0));
  }
  KJ_REQUIRE(result == 0, "stream did not end");
  return total;
}

struct NativeFns {
  void* create() const {
    return ZSTD_createDCtx();
  }
  size_t free(void* dctx) const {
    return ZSTD_freeDCtx(reinterpret_cast<ZSTD_DCtx*>(dctx));
  }
  size_t decompress(void* dctx, ZSTD_outBuffer* output, ZSTD_inBuffer* input) const {
    return ZSTD_decompressStream(reinterpret_cast<ZSTD_DCtx*>(dctx), output, input);
  }
};

struct ZstdRsFns {
  void* create() const {
    return zstd_rs_ZSTD_createDCtx();
  }
  size_t free(void* dctx) const {
    return zstd_rs_ZSTD_freeDCtx(dctx);
  }
  size_t decompress(void* dctx, ZSTD_outBuffer* output, ZSTD_inBuffer* input) const {
    return zstd_rs_ZSTD_decompressStream(dctx, output, input);
  }
};

void Zstd_Compress_Native(benchmark::State& state) {
  auto& src = sourceData();
  for (auto _: state) {
    benchmark::DoNotOptimize(compressNative(src));
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * src.size());
}

void Zstd_Decompress_Native(benchmark::State& state) {
  auto& src = sourceData();
  auto compressed = compressedData();
  for (auto _: state) {
    KJ_ASSERT(streamDecompress(NativeFns{}, compressed) == src.size());
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * src.size());
}

void Zstd_Decompress_ZstdRs(benchmark::State& state) {
  auto& src = sourceData();
  auto compressed = compressedData();
  for (auto _: state) {
    KJ_ASSERT(streamDecompress(ZstdRsFns{}, compressed) == src.size());
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * src.size());
}

WD_BENCHMARK(Zstd_Compress_Native)->Name("Zstd::Compress::Native");
WD_BENCHMARK(Zstd_Decompress_Native)->Name("Zstd::Decompress::Native");
WD_BENCHMARK(Zstd_Decompress_ZstdRs)->Name("Zstd::Decompress::ZstdRs");

}  // namespace
}  // namespace workerd
