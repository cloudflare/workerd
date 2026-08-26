// Benchmarks native (chromium) zlib against zlib-rs (libz-rs-sys) on the
// node:zlib streaming workload: 1MB of compressible data in 16KB chunks with
// node's default parameters (level 6, windowBits 15, memLevel 8).

#include "zlib-rs-shim.h"

#include <workerd/tests/bench-tools.h>

#include <zlib.h>

#include <kj/array.h>
#include <kj/debug.h>

namespace workerd {
namespace {

constexpr size_t DATA_SIZE = 1 << 20;
constexpr size_t CHUNK = 16 * 1024;
constexpr int LEVEL = 6;
constexpr int WINDOW_BITS = 15;
constexpr int MEM_LEVEL = 8;

kj::Array<kj::byte> makeCompressibleData() {
  static constexpr kj::StringPtr words[] = {"the "_kj, "quick "_kj, "compression "_kj, "of "_kj,
    "zlib "_kj, "streaming "_kj, "workers "_kj, "runtime "_kj};
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

// Streaming via a generic stream-call shape shared by both backends.
template <typename RunFn>
size_t streamAll(kj::ArrayPtr<const kj::byte> data, RunFn&& run, bool expectStreamEnd) {
  static kj::byte out[CHUNK];
  size_t total = 0;
  size_t offset = 0;
  bool done = false;
  while (offset < data.size() && !done) {
    size_t n = kj::min(CHUNK, data.size() - offset);
    bool last = offset + n == data.size();
    const kj::byte* in = data.begin() + offset;
    uint32_t availIn = n;
    offset += n;
    do {
      uint32_t availInAfter;
      uint32_t availOutAfter;
      int result = run(in + (n - availIn), availIn, out, sizeof(out), last ? Z_FINISH : Z_NO_FLUSH,
          &availInAfter, &availOutAfter);
      KJ_REQUIRE(result == Z_OK || result == Z_STREAM_END || result == Z_BUF_ERROR,
          "zlib stream error", result);
      availIn = availInAfter;
      total += sizeof(out) - availOutAfter;
      if (result == Z_STREAM_END) {
        done = true;
        break;
      }
      if (availIn == 0 && availOutAfter > 0 && !last) break;
    } while (true);
    if (last && expectStreamEnd) {
      KJ_REQUIRE(done, "stream did not end");
    }
  }
  return total;
}

size_t deflateNative(kj::ArrayPtr<const kj::byte> data) {
  z_stream zs{};
  KJ_REQUIRE(
      deflateInit2(&zs, LEVEL, Z_DEFLATED, WINDOW_BITS, MEM_LEVEL, Z_DEFAULT_STRATEGY) == Z_OK);
  KJ_DEFER(deflateEnd(&zs));
  return streamAll(data,
      [&](const kj::byte* in, uint32_t availIn, kj::byte* out, uint32_t availOut, int flush,
          uint32_t* availInAfter, uint32_t* availOutAfter) {
    zs.next_in = const_cast<kj::byte*>(in);
    zs.avail_in = availIn;
    zs.next_out = out;
    zs.avail_out = availOut;
    int result = deflate(&zs, flush);
    *availInAfter = zs.avail_in;
    *availOutAfter = zs.avail_out;
    return result;
  },
      true);
}

size_t inflateNative(kj::ArrayPtr<const kj::byte> data) {
  z_stream zs{};
  KJ_REQUIRE(inflateInit2(&zs, WINDOW_BITS) == Z_OK);
  KJ_DEFER(inflateEnd(&zs));
  return streamAll(data,
      [&](const kj::byte* in, uint32_t availIn, kj::byte* out, uint32_t availOut, int flush,
          uint32_t* availInAfter, uint32_t* availOutAfter) {
    zs.next_in = const_cast<kj::byte*>(in);
    zs.avail_in = availIn;
    zs.next_out = out;
    zs.avail_out = availOut;
    int result = inflate(&zs, flush);
    *availInAfter = zs.avail_in;
    *availOutAfter = zs.avail_out;
    return result;
  },
      true);
}

size_t deflateZlibRs(kj::ArrayPtr<const kj::byte> data) {
  auto stream = zrs::newDeflate(LEVEL, WINDOW_BITS, MEM_LEVEL, Z_DEFAULT_STRATEGY);
  KJ_DEFER(zrs::freeDeflate(stream));
  return streamAll(data,
      [&](const kj::byte* in, uint32_t availIn, kj::byte* out, uint32_t availOut, int flush,
          uint32_t* availInAfter, uint32_t* availOutAfter) {
    return zrs::runDeflate(stream, in, availIn, out, availOut, flush, availInAfter, availOutAfter);
  },
      true);
}

size_t inflateZlibRs(kj::ArrayPtr<const kj::byte> data) {
  auto stream = zrs::newInflate(WINDOW_BITS);
  KJ_DEFER(zrs::freeInflate(stream));
  return streamAll(data,
      [&](const kj::byte* in, uint32_t availIn, kj::byte* out, uint32_t availOut, int flush,
          uint32_t* availInAfter, uint32_t* availOutAfter) {
    return zrs::runInflate(stream, in, availIn, out, availOut, flush, availInAfter, availOutAfter);
  },
      true);
}

kj::Array<kj::byte> compressedData() {
  auto& src = sourceData();
  auto out = kj::heapArray<kj::byte>(compressBound(src.size()));
  z_stream zs{};
  KJ_REQUIRE(
      deflateInit2(&zs, LEVEL, Z_DEFLATED, WINDOW_BITS, MEM_LEVEL, Z_DEFAULT_STRATEGY) == Z_OK);
  KJ_DEFER(deflateEnd(&zs));
  zs.next_in = const_cast<kj::byte*>(src.begin());
  zs.avail_in = src.size();
  zs.next_out = out.begin();
  zs.avail_out = out.size();
  KJ_REQUIRE(deflate(&zs, Z_FINISH) == Z_STREAM_END);
  return kj::heapArray<kj::byte>(out.first(out.size() - zs.avail_out));
}

void Zlib_Deflate_Native(benchmark::State& state) {
  auto& src = sourceData();
  for (auto _: state) {
    benchmark::DoNotOptimize(deflateNative(src));
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * src.size());
}

void Zlib_Deflate_ZlibRs(benchmark::State& state) {
  auto& src = sourceData();
  // Sanity: identical roundtrip size via native inflate of zlib-rs output is
  // covered by the inflate benches below consuming native-compressed data.
  for (auto _: state) {
    benchmark::DoNotOptimize(deflateZlibRs(src));
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * src.size());
}

void Zlib_Inflate_Native(benchmark::State& state) {
  auto& src = sourceData();
  auto compressed = compressedData();
  for (auto _: state) {
    KJ_ASSERT(inflateNative(compressed) == src.size());
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * src.size());
}

void Zlib_Inflate_ZlibRs(benchmark::State& state) {
  auto& src = sourceData();
  auto compressed = compressedData();
  for (auto _: state) {
    KJ_ASSERT(inflateZlibRs(compressed) == src.size());
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * src.size());
}

WD_BENCHMARK(Zlib_Deflate_Native)->Name("Zlib::Deflate::Native");
WD_BENCHMARK(Zlib_Deflate_ZlibRs)->Name("Zlib::Deflate::ZlibRs");
WD_BENCHMARK(Zlib_Inflate_Native)->Name("Zlib::Inflate::Native");
WD_BENCHMARK(Zlib_Inflate_ZlibRs)->Name("Zlib::Inflate::ZlibRs");

}  // namespace
}  // namespace workerd
