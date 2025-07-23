#include "bridge.h"

#include <rust/cxx.h>

#include <kj/exception.h>

namespace workerd::rust::cxx_integration {

namespace {

void throwKjException(const char* msg, size_t size) {
  auto str = kj::str(kj::StringPtr(msg, size - 1));
  delete[] msg;
  kj::throwFatalException(
      kj::Exception(kj::Exception::Type::FAILED, __FILE__, __LINE__, kj::mv(str)));
}

}  // namespace

void install_throw_handler() {
  ::rust::throw_rust_error = throwKjException;
}
}  // namespace workerd::rust::cxx_integration
