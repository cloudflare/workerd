#pragma once

#include <rust/cxx.h>

#if (defined(__has_feature) && __has_feature(thread_sanitizer)) || defined(__SANITIZE_THREAD__)
#include <sanitizer/tsan_interface.h>
#endif

#include <kj/debug.h>
#include <kj/memory.h>

#include <cstdint>

namespace kj_rs_demo {

class OpaqueCxxClass {
 public:
  OpaqueCxxClass(uint64_t data): data(data) {}
  ~OpaqueCxxClass() {}
  uint64_t getData() const {
    return this->data;
  }
  void setData(uint64_t val) {
    this->data = val;
  }

 private:
  uint64_t data;
};

inline void tsanReleaseOpaque(const OpaqueCxxClass& value) {
#if (defined(__has_feature) && __has_feature(thread_sanitizer)) || defined(__SANITIZE_THREAD__)
  __tsan_release(const_cast<OpaqueCxxClass*>(&value));
#endif
}

inline void tsanAcquireOpaque(const OpaqueCxxClass& value) {
#if (defined(__has_feature) && __has_feature(thread_sanitizer)) || defined(__SANITIZE_THREAD__)
  __tsan_acquire(const_cast<OpaqueCxxClass*>(&value));
#endif
}

// Forward declaration for Rust function, including the lib.rs.h caused problems
kj::Own<OpaqueCxxClass> modify_own_return(kj::Own<OpaqueCxxClass> cpp_own);
// Rust function that takes in a cpp_own. Should cause C++ exception if the own is NULL
void null_exception_test(kj::Own<OpaqueCxxClass> cpp_own);
// Rust function that calls `null_kj_own` and tries to return it
kj::Own<OpaqueCxxClass> get_null();
// Rust function that takes ownweship and drops it
void take_own(kj::Own<OpaqueCxxClass> cpp_own);

rust::String null_exception_test_driver_1();
rust::String null_exception_test_driver_2();
void rust_take_own_driver();

// Function declarations
kj::Own<OpaqueCxxClass> cxx_kj_own();
kj::Own<OpaqueCxxClass> null_kj_own();
void give_own_back(kj::Own<OpaqueCxxClass> own);
void modify_own_return_test();
kj::Own<OpaqueCxxClass> breaking_things();
kj::Own<OpaqueCxxClass> cxx_try_return_own();
kj::Own<OpaqueCxxClass> cxx_fail_return_own();
kj::Own<int64_t> own_integer();
kj::Own<int64_t> own_integer_attached();

}  // namespace kj_rs_demo
