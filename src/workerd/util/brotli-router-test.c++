#include <workerd/util/autogate.h>

#include <brotli/decode.h>
#include <brotli/encode.h>

#include <kj/array.h>
#include <kj/test.h>

// Direct entry points into the two implementations behind the router, for comparing against the
// routed (unprefixed) API. The unprefixed spelling comes from the brotli headers; these must be
// hand-declared since the renames in build/BUILD.brotli only apply to the C implementation's own
// compilation.
extern "C" {
int brotli_c_BrotliEncoderCompress(int quality,
    int lgwin,
    int mode,
    size_t inputSize,
    const unsigned char* inputBuffer,
    size_t* encodedSize,
    unsigned char* encodedBuffer);
int brotli_rs_BrotliEncoderCompress(int quality,
    int lgwin,
    int mode,
    size_t inputSize,
    const unsigned char* inputBuffer,
    size_t* encodedSize,
    unsigned char* encodedBuffer);
const char* brotli_c_BrotliDecoderErrorString(int code);
const char* brotli_rs_BrotliDecoderErrorString(int code);
}

namespace workerd::util {
namespace {

using CompressFn = int (*)(int, int, int, size_t, const unsigned char*, size_t*, unsigned char*);

// Adapter over the header-declared routed API (whose mode parameter is the
// BrotliEncoderMode enum) to the common signature used by the prefixed entry points.
int routedCompress(int quality,
    int lgwin,
    int mode,
    size_t inputSize,
    const unsigned char* inputBuffer,
    size_t* encodedSize,
    unsigned char* encodedBuffer) {
  return BrotliEncoderCompress(quality, lgwin, static_cast<BrotliEncoderMode>(mode), inputSize,
      inputBuffer, encodedSize, encodedBuffer);
}

// Quality 4 chosen because the two implementations' hashing heuristics observably diverge
// there for this input; at some other qualities the port produces byte-identical streams.
constexpr int QUALITY = 4;
constexpr int LGWIN = 22;

kj::Array<kj::byte> makeInput() {
  static constexpr kj::StringPtr words[] = {"the "_kj, "quick "_kj, "compression "_kj, "of "_kj,
    "brotli "_kj, "streaming "_kj, "workers "_kj, "runtime "_kj};
  auto data = kj::heapArray<kj::byte>(64 * 1024);
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

kj::Array<kj::byte> compressWith(CompressFn fn, kj::ArrayPtr<const kj::byte> input) {
  size_t destLen = BrotliEncoderMaxCompressedSize(input.size());
  auto dest = kj::heapArray<kj::byte>(destLen);
  KJ_REQUIRE(fn(QUALITY, LGWIN, BROTLI_MODE_GENERIC, input.size(), input.begin(), &destLen,
                 dest.begin()) == BROTLI_TRUE);
  return kj::heapArray<kj::byte>(dest.first(destLen));
}

void expectRoundtrip(kj::ArrayPtr<const kj::byte> compressed, kj::ArrayPtr<const kj::byte> input) {
  size_t destLen = input.size();
  auto dest = kj::heapArray<kj::byte>(destLen);
  KJ_EXPECT(BrotliDecoderDecompress(compressed.size(), compressed.begin(), &destLen,
                dest.begin()) == BROTLI_DECODER_RESULT_SUCCESS);
  KJ_EXPECT(dest.first(destLen) == input);
}

KJ_TEST("brotli router uses the C implementation with the compression-rs gate off") {
  Autogate::initAutogateNamesForTest(
      kj::ArrayPtr<const kj::StringPtr>(), IgnoreAllAutogatesEnv::YES);
  KJ_DEFER(Autogate::deinitAutogate());

  KJ_EXPECT(BrotliDecoderErrorString(BROTLI_DECODER_ERROR_FORMAT_PADDING_1) ==
      brotli_c_BrotliDecoderErrorString(BROTLI_DECODER_ERROR_FORMAT_PADDING_1));

  auto input = makeInput();
  auto routed = compressWith(&routedCompress, input);
  auto native = compressWith(&brotli_c_BrotliEncoderCompress, input);
  auto rs = compressWith(&brotli_rs_BrotliEncoderCompress, input);

  KJ_EXPECT(routed == native);
  // The premise of the routing test: the two implementations produce observably different
  // streams for this input.
  KJ_EXPECT(routed != rs);
  expectRoundtrip(routed, input);
}

KJ_TEST("brotli router uses rust-brotli with the compression-rs gate on") {
  Autogate::initAutogateForTest({AutogateKey::COMPRESSION_RS});
  KJ_DEFER(Autogate::deinitAutogate());

  KJ_EXPECT(BrotliDecoderErrorString(BROTLI_DECODER_ERROR_FORMAT_PADDING_1) ==
      brotli_rs_BrotliDecoderErrorString(BROTLI_DECODER_ERROR_FORMAT_PADDING_1));
  // The Rust implementation's error strings match the C implementation's.
  KJ_EXPECT(
      kj::StringPtr(brotli_rs_BrotliDecoderErrorString(BROTLI_DECODER_ERROR_FORMAT_PADDING_1)) ==
      kj::StringPtr(brotli_c_BrotliDecoderErrorString(BROTLI_DECODER_ERROR_FORMAT_PADDING_1)));

  auto input = makeInput();
  auto routed = compressWith(&routedCompress, input);
  auto native = compressWith(&brotli_c_BrotliEncoderCompress, input);
  auto rs = compressWith(&brotli_rs_BrotliEncoderCompress, input);

  KJ_EXPECT(routed == rs);
  KJ_EXPECT(routed != native);
  // rust-brotli output decoded by whichever implementation the router selects; also decode the
  // C implementation's stream to cover cross-implementation compatibility.
  expectRoundtrip(routed, input);
  expectRoundtrip(native, input);
}

}  // namespace
}  // namespace workerd::util
