// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// #include <workerd/rust/python-parser/lib.rs.h>
#include "workerd/rust/cxx-integration/lib.rs.h"

#include <kj/test.h>

namespace workerd::rust::python_parser {
  ::rust::Vec<::rust::String> get_imports(::rust::Slice<::rust::Str const> sources);
}

using ::workerd::rust::python_parser::get_imports;

namespace workerd::api::pyodide {
  kj::Array<kj::String> parseImports(kj::ArrayPtr<kj::StringPtr> cpp_modules) {
    auto rust_modules = kj::heapArrayBuilder<::rust::Str const>(cpp_modules.size());
    for (auto& entry: cpp_modules) {
      rust_modules.add(entry.cStr());
    }
    ::rust::Slice<::rust::Str const> rust_slice(rust_modules.begin(), rust_modules.size());
    auto rust_result = get_imports(rust_slice);
    return workerd::fromRust(rust_result);
  }
}
