#include "base64.h"

#include "simdutf.h"

#include <kj/debug.h>

namespace workerd::api {

jsg::BufferSource Base64Module::decodeArray(jsg::Lock& js, jsg::BufferSource input) {
  auto ptr = input.asArrayPtr();
  auto size = simdutf::maximal_binary_length_from_base64((const char*)ptr.begin(), ptr.size());
  auto buf = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, size);
  auto result = simdutf::base64_to_binary(ptr.asChars().begin(), input.size(),
      buf.asArrayPtr().asChars().begin(), simdutf::base64_default);
  JSG_REQUIRE(result.error == simdutf::SUCCESS, DOMSyntaxError,
      kj::str("Invalid base64 at position ", result.count, ": ",
          simdutf::error_to_string(result.error).data()));
  KJ_ASSERT(result.count <= size);
  buf.trim(buf.size() - result.count);
  return jsg::BufferSource(js, kj::mv(buf));
}

jsg::BufferSource Base64Module::encodeArray(jsg::Lock& js, jsg::BufferSource input) {
  auto size = simdutf::base64_length_from_binary(input.size());
  auto buf = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, size);
  auto out_size = simdutf::binary_to_base64(input.asArrayPtr().asChars().begin(), input.size(),
      buf.asArrayPtr().asChars().begin(), simdutf::base64_default);
  KJ_ASSERT(out_size <= size);
  buf.limit(out_size);
  return jsg::BufferSource(js, kj::mv(buf));
}

jsg::JsString Base64Module::encodeArrayToString(jsg::Lock& js, jsg::BufferSource input) {
  KJ_ASSERT(input.size() < 256 * 1024 * 1024);
  auto size = simdutf::base64_length_from_binary(input.size());
  auto buf = jsg::BackingStore::alloc(js, size);
  auto out_size = simdutf::binary_to_base64(input.asArrayPtr().asChars().begin(), input.size(),
      buf.asArrayPtr().asChars().begin(), simdutf::base64_default);
  KJ_ASSERT(out_size <= size);
  return js.str(buf.asArrayPtr().first(out_size));
}

}  // namespace workerd::api
