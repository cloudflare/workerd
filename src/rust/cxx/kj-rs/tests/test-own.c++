#include "test-own.h"

#include "kj-rs/convert.h"
#include "kj-rs/kj-rs.h"

#include <kj/string.h>

#include <exception>
#include <stdexcept>

using namespace kj_rs;

namespace kj_rs_demo {

kj::Own<OpaqueCxxClass> cxx_kj_own() {
  return kj::heap<OpaqueCxxClass>(42);
}

kj::Own<OpaqueCxxClass> null_kj_own() {
  return kj::Own<OpaqueCxxClass>();
}

void give_own_back(kj::Own<OpaqueCxxClass> own) {
  own->setData(37);
  KJ_ASSERT(own->getData() == 37);
}

void modify_own_return_test() {
  auto owned = kj::heap<OpaqueCxxClass>(17);
  auto returned = modify_own_return(kj::mv(owned));
  KJ_ASSERT(returned->getData() == 72);
}

kj::Own<OpaqueCxxClass> breaking_things() {
  auto own0 = kj::heap<OpaqueCxxClass>(42);
  auto own1 = kj::heap<OpaqueCxxClass>(72);
  auto own2 = own0.attach(kj::mv(own1));
  return own2;
}

kj::Own<OpaqueCxxClass> cxx_try_return_own() {
  return kj::heap<OpaqueCxxClass>(5150);
}

kj::Own<OpaqueCxxClass> cxx_fail_return_own() {
  throw std::runtime_error("cxx_fail_return_own");
}

kj::Own<int64_t> own_integer() {
  return kj::heap<int64_t>(-67582);
}

kj::Own<int64_t> own_integer_attached() {
  auto own = kj::heap<int64_t>(-67582);
  auto attach = kj::heap<OpaqueCxxClass>(18672483);
  return own.attach(kj::mv(attach));
}

rust::string null_exception_test_driver_1() {
  try {
    auto _ = modify_own_return(null_kj_own());
    return rust::string();
  } catch (...) {
    return kj::getCaughtExceptionAsKj().getDescription().as<RustCopyUncheckedUtf8>();
  }
}

rust::string null_exception_test_driver_2() {
  try {
    auto _ = get_null();
    return rust::string();
  } catch (...) {
    return kj::getCaughtExceptionAsKj().getDescription().as<RustCopyUncheckedUtf8>();
  }
}

void rust_take_own_driver() {
  auto own = kj::heap<OpaqueCxxClass>(14);
  take_own(kj::mv(own));
}

}  // namespace kj_rs_demo
