#pragma once

#include <rust/cxx.h>

namespace workerd::rust::gen_compile_cache {
  ::rust::Vec<uint8_t> compile(::rust::Str path, ::rust::Str source);
}
