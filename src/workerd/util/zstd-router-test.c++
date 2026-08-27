#include <workerd/util/autogate.h>

#include <zstd.h>

#include <kj/array.h>
#include <kj/test.h>

// Direct entry points into the two implementations behind the router, for comparing against the
// routed (unprefixed) API declared by zstd.h. Only decompression branches on the gate, so only
// the decompression context entry points exist in both spellings.
extern "C" {
void* zstd_c_ZSTD_createDCtx(void);
size_t zstd_c_ZSTD_freeDCtx(void* dctx);
size_t zstd_c_ZSTD_DCtx_setParameter(void* dctx, int param, int value);
size_t zstd_c_ZSTD_decompressStream(void* dctx, void* output, void* input);
void* zstd_rs_ZSTD_createDCtx(void);
size_t zstd_rs_ZSTD_freeDCtx(void* dctx);
size_t zstd_rs_ZSTD_DCtx_setParameter(void* dctx, int param, int value);
size_t zstd_rs_ZSTD_decompressStream(void* dctx, void* output, void* input);
}

namespace workerd::util {
namespace {

// ZSTD_d_format, an experimental parameter the C implementation accepts and the Rust decoder
// rejects; used to observe which implementation the router selected.
constexpr int ZSTD_D_FORMAT = 1000;

struct DctxFns {
  void* (*create)();
  size_t (*free)(void* dctx);
  size_t (*setParameter)(void* dctx, int param, int value);
  size_t (*decompressStream)(void* dctx, void* output, void* input);
};

// The routed API, adapted to the hand-declared opaque-pointer shapes above.
const DctxFns routedFns{
  []() -> void* { return ZSTD_createDCtx(); },
  [](void* dctx) { return ZSTD_freeDCtx(reinterpret_cast<ZSTD_DCtx*>(dctx)); },
  [](void* dctx, int param, int value) {
  return ZSTD_DCtx_setParameter(
      reinterpret_cast<ZSTD_DCtx*>(dctx), static_cast<ZSTD_dParameter>(param), value);
},
  [](void* dctx, void* output, void* input) {
  return ZSTD_decompressStream(reinterpret_cast<ZSTD_DCtx*>(dctx),
      reinterpret_cast<ZSTD_outBuffer*>(output), reinterpret_cast<ZSTD_inBuffer*>(input));
},
};

const DctxFns cFns{
  zstd_c_ZSTD_createDCtx,
  zstd_c_ZSTD_freeDCtx,
  zstd_c_ZSTD_DCtx_setParameter,
  zstd_c_ZSTD_decompressStream,
};

const DctxFns rsFns{
  zstd_rs_ZSTD_createDCtx,
  zstd_rs_ZSTD_freeDCtx,
  zstd_rs_ZSTD_DCtx_setParameter,
  zstd_rs_ZSTD_decompressStream,
};

kj::Array<kj::byte> makeInput() {
  constexpr kj::StringPtr chunk = "the quick compression of zstd streaming workers runtime "_kj;
  auto data = kj::heapArray<kj::byte>(256 * 1024);
  for (size_t pos = 0; pos < data.size(); pos += chunk.size()) {
    memcpy(data.begin() + pos, chunk.begin(), kj::min(chunk.size(), data.size() - pos));
  }
  return data;
}

kj::Array<kj::byte> compressRouted(kj::ArrayPtr<const kj::byte> input, bool checksum = false) {
  auto* cctx = ZSTD_createCCtx();
  KJ_REQUIRE(cctx != nullptr);
  KJ_DEFER(ZSTD_freeCCtx(cctx));
  if (checksum) {
    KJ_REQUIRE(!ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 1)));
  }
  auto dest = kj::heapArray<kj::byte>(input.size() + 1024);
  ZSTD_inBuffer in{input.begin(), input.size(), 0};
  ZSTD_outBuffer out{dest.begin(), dest.size(), 0};
  size_t result = ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_end);
  KJ_REQUIRE(result == 0 && !ZSTD_isError(result));
  return kj::heapArray<kj::byte>(dest.first(out.pos));
}

// Streaming decompression through the given entry points, in 4KB input and 4KB output steps.
kj::Array<kj::byte> decompressWith(const DctxFns& fns, kj::ArrayPtr<const kj::byte> compressed) {
  auto* dctx = fns.create();
  KJ_REQUIRE(dctx != nullptr);
  KJ_DEFER(fns.free(dctx));

  kj::Vector<kj::byte> result;
  kj::byte chunk[4096];
  size_t offset = 0;
  size_t lastResult = 1;
  while (true) {
    size_t n = kj::min(sizeof(chunk), compressed.size() - offset);
    ZSTD_inBuffer in{compressed.begin() + offset, n, 0};
    do {
      ZSTD_outBuffer out{chunk, sizeof(chunk), 0};
      lastResult = fns.decompressStream(dctx, &out, &in);
      KJ_REQUIRE(!ZSTD_isError(lastResult), ZSTD_getErrorName(lastResult));
      result.addAll(kj::arrayPtr(chunk, out.pos));
    } while (in.pos < in.size || (lastResult != 0 && offset + n == compressed.size()));
    offset += n;
    if (offset == compressed.size()) break;
  }
  KJ_REQUIRE(lastResult == 0, "stream did not end");
  return result.releaseAsArray();
}

bool acceptsDFormatParam(const DctxFns& fns) {
  auto* dctx = fns.create();
  KJ_REQUIRE(dctx != nullptr);
  KJ_DEFER(fns.free(dctx));
  return !ZSTD_isError(fns.setParameter(dctx, ZSTD_D_FORMAT, 0));
}

KJ_TEST("zstd router uses the C implementation with the compression-rs gate off") {
  Autogate::initAutogateNamesForTest(
      kj::ArrayPtr<const kj::StringPtr>(), IgnoreAllAutogatesEnv::YES);
  KJ_DEFER(Autogate::deinitAutogate());

  // The premise of the routing probe: the two implementations observably differ on the
  // experimental ZSTD_d_format parameter.
  KJ_EXPECT(acceptsDFormatParam(cFns));
  KJ_EXPECT(!acceptsDFormatParam(rsFns));
  KJ_EXPECT(acceptsDFormatParam(routedFns));

  auto input = makeInput();
  auto compressed = compressRouted(input);
  KJ_EXPECT(decompressWith(routedFns, compressed) == input);
  KJ_EXPECT(decompressWith(cFns, compressed) == input);
  KJ_EXPECT(decompressWith(rsFns, compressed) == input);
}

KJ_TEST("zstd router uses the Rust decoder with the compression-rs gate on") {
  Autogate::initAutogateForTest({AutogateKey::COMPRESSION_RS});
  KJ_DEFER(Autogate::deinitAutogate());

  KJ_EXPECT(!acceptsDFormatParam(routedFns));
  KJ_EXPECT(acceptsDFormatParam(cFns));

  auto input = makeInput();
  // Compression is not routed: it must keep working with the gate on.
  auto compressed = compressRouted(input);
  KJ_EXPECT(decompressWith(routedFns, compressed) == input);
  KJ_EXPECT(decompressWith(rsFns, compressed) == input);
  KJ_EXPECT(decompressWith(cFns, compressed) == input);

  // Checksummed frames verify on the Rust side.
  auto checksummed = compressRouted(input, true);
  KJ_EXPECT(decompressWith(routedFns, checksummed) == input);

  // Corruption is reported through the standard error code encoding.
  {
    auto corrupted = kj::heapArray<kj::byte>(checksummed);
    corrupted[corrupted.size() / 2] ^= 0xff;
    auto* dctx = routedFns.create();
    KJ_DEFER(routedFns.free(dctx));
    kj::byte chunk[4096];
    ZSTD_inBuffer in{corrupted.begin(), corrupted.size(), 0};
    size_t result = 0;
    do {
      ZSTD_outBuffer out{chunk, sizeof(chunk), 0};
      result = routedFns.decompressStream(dctx, &out, &in);
    } while (!ZSTD_isError(result) && result != 0 && in.pos < in.size);
    KJ_EXPECT(ZSTD_isError(result));
  }
}

}  // namespace
}  // namespace workerd::util
