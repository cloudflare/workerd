#include "base64.h"

#include "simdutf.h"

#include <kj/debug.h>

namespace workerd::api {

kj::Array<kj::byte> Base64Module::decodeArray(kj::Array<kj::byte> input) {
  auto size = simdutf::maximal_binary_length_from_base64((const char*)input.begin(), input.size());
  auto buf = kj::heapArray<kj::byte>(size);
  auto result = simdutf::base64_to_binary(
      input.asChars().begin(), input.size(), buf.asChars().begin(), simdutf::base64_default);
  JSG_REQUIRE(result.error == simdutf::SUCCESS, DOMSyntaxError,
      kj::str("Invalid base64 at position ", result.count, ": ",
          simdutf::error_to_string(result.error).data()));
  KJ_ASSERT(result.count <= size);
  return buf.slice(0, result.count).attach(kj::mv(buf));
}

kj::Array<kj::byte> Base64Module::encodeArray(kj::Array<kj::byte> input) {
  auto size = simdutf::base64_length_from_binary(input.size());
  auto buf = kj::heapArray<kj::byte>(size);
  auto out_size = simdutf::binary_to_base64(
      input.asChars().begin(), input.size(), buf.asChars().begin(), simdutf::base64_default);
  KJ_ASSERT(out_size <= size);
  return buf.slice(0, out_size).attach(kj::mv(buf));
}

jsg::JsString Base64Module::encodeArrayToString(jsg::Lock& js, kj::Array<kj::byte> input) {
  auto size = simdutf::base64_length_from_binary(input.size());
  auto buf = kj::heapArray<kj::byte>(size);
  auto out_size = simdutf::binary_to_base64(
      input.asChars().begin(), input.size(), buf.asChars().begin(), simdutf::base64_default);
  KJ_ASSERT(out_size <= size);

  return js.str(buf.first(out_size));
}

}  // namespace workerd::api
