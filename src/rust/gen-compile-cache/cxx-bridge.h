#pragma once

#include <rust/cxx.h>

namespace workerd::rust::gen_compile_cache {
struct CompileOptions;

::rust::Vec<uint8_t> compile(::rust::Str path,
    ::rust::Str source,
    ::rust::Slice<const ::rust::String> v8Flags,
    const CompileOptions& options);
}  // namespace workerd::rust::gen_compile_cache
