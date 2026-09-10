#include <workerd/util/autogate.h>

#include <zlib.h>

#include <kj/array.h>
#include <kj/test.h>

// Direct entry points into the two implementations behind the router, for comparing against the
// routed (unprefixed) API. The unprefixed spelling comes from zlib.h (compiled with
// CHROMIUM_ZLIB_NO_CHROMECONF); these must be hand-declared since chromeconf.h's renames make the
// two spellings mutually exclusive within one translation unit.
extern "C" {
const char* Cr_z_zlibVersion(void);
const char* zlib_rs_zlibVersion(void);
int Cr_z_compress2(unsigned char* dest,
    unsigned long* destLen,
    const unsigned char* source,
    unsigned long sourceLen,
    int level);
int zlib_rs_compress2(unsigned char* dest,
    unsigned long* destLen,
    const unsigned char* source,
    unsigned long sourceLen,
    int level);
}

namespace workerd::util {
namespace {

using Compress2Fn = int (*)(
    unsigned char*, unsigned long*, const unsigned char*, unsigned long, int);

kj::Array<kj::byte> makeInput() {
  constexpr kj::StringPtr chunk = "the quick compression of zlib streaming workers runtime "_kj;
  auto data = kj::heapArray<kj::byte>(64 * 1024);
  for (size_t pos = 0; pos < data.size(); pos += chunk.size()) {
    memcpy(data.begin() + pos, chunk.begin(), kj::min(chunk.size(), data.size() - pos));
  }
  return data;
}

kj::Array<kj::byte> compressWith(Compress2Fn fn, kj::ArrayPtr<const kj::byte> input) {
  unsigned long destLen = compressBound(input.size());
  auto dest = kj::heapArray<kj::byte>(destLen);
  KJ_REQUIRE(fn(dest.begin(), &destLen, input.begin(), input.size(), 6) == Z_OK);
  return kj::heapArray<kj::byte>(dest.first(destLen));
}

void expectRoundtrip(kj::ArrayPtr<const kj::byte> compressed, kj::ArrayPtr<const kj::byte> input) {
  unsigned long destLen = input.size();
  auto dest = kj::heapArray<kj::byte>(destLen);
  KJ_EXPECT(uncompress(dest.begin(), &destLen, compressed.begin(), compressed.size()) == Z_OK);
  KJ_EXPECT(dest.first(destLen) == input);
}

KJ_TEST("zlib router uses chromium zlib with the compression-rs gate off") {
  Autogate::initAutogateNamesForTest(
      kj::ArrayPtr<const kj::StringPtr>(), IgnoreAllAutogatesEnv::YES);
  KJ_DEFER(Autogate::deinitAutogate());

  KJ_EXPECT(zlibVersion() == Cr_z_zlibVersion());

  auto input = makeInput();
  auto routed = compressWith(&compress2, input);
  auto chromium = compressWith(&Cr_z_compress2, input);
  auto rs = compressWith(&zlib_rs_compress2, input);

  KJ_EXPECT(routed == chromium);
  // The premise of the routing test: the two implementations produce observably different
  // streams for this input.
  KJ_EXPECT(routed != rs);
  expectRoundtrip(routed, input);
}

KJ_TEST("zlib router uses zlib-rs with the compression-rs gate on") {
  Autogate::initAutogateForTest({AutogateKey::COMPRESSION_RS});
  KJ_DEFER(Autogate::deinitAutogate());

  KJ_EXPECT(zlibVersion() == zlib_rs_zlibVersion());

  auto input = makeInput();
  auto routed = compressWith(&compress2, input);
  auto chromium = compressWith(&Cr_z_compress2, input);
  auto rs = compressWith(&zlib_rs_compress2, input);

  KJ_EXPECT(routed == rs);
  KJ_EXPECT(routed != chromium);
  // zlib-rs output decoded by whichever implementation the router selects; also decode the
  // chromium stream to cover cross-implementation compatibility.
  expectRoundtrip(routed, input);
  expectRoundtrip(chromium, input);
}

}  // namespace
}  // namespace workerd::util
