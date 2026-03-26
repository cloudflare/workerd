#pragma once

#include <rust/cxx.h>

namespace workerd::rust::nbytes {

::rust::Vec<uint8_t> base64_decode(::rust::Slice<const uint8_t> input);

}  // namespace workerd::rust::nbytes
