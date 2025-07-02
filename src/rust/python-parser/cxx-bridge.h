#include "kj/string.h"

namespace workerd::api::pyodide {
  kj::Array<kj::String> parseImports(kj::ArrayPtr<kj::StringPtr> cpp_modules);
}
