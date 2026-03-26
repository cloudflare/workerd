#include "workerd/rust/nbytes/ffi.h"

#include <nbytes.h>

#include <algorithm>
#include <iterator>
#include <memory>

namespace workerd::rust::nbytes {

::rust::Vec<uint8_t> base64_decode(::rust::Slice<const uint8_t> input) {
  const char* src = reinterpret_cast<const char*>(input.data());
  size_t srclen = input.size();

  size_t decoded_size = ::nbytes::Base64DecodedSize(src, srclen);

  auto buf = std::make_unique<char[]>(decoded_size);
  size_t actual = ::nbytes::Base64Decode(buf.get(), decoded_size, src, srclen);

  ::rust::Vec<uint8_t> result;
  result.reserve(actual);
  auto* data = reinterpret_cast<const uint8_t*>(buf.get());
  std::copy_n(data, actual, std::back_inserter(result));
  return result;
}

}  // namespace workerd::rust::nbytes
