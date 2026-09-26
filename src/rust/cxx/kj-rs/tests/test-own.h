#pragma once

#include <rust/cxx.h>

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

// `TwoBase` has two polymorphic bases, so `SecondBase` sits at a nonzero offset inside the complete
// object. A `kj::Own<SecondBase>` that points into a `TwoBase` must hand its disposer the complete
// object's address (`dynamic_cast<void*>`), not the `SecondBase` subobject's.
class FirstBase {
 public:
  virtual ~FirstBase() = default;
  virtual uint64_t first() const {
    return firstData;
  }

 private:
  uint64_t firstData = 1;
};

class SecondBase {
 public:
  virtual ~SecondBase() = default;
  virtual uint64_t second() const {
    return secondData;
  }

 private:
  uint64_t secondData = 2;
};

class TwoBase final: public FirstBase, public SecondBase {
 public:
  TwoBase() = default;
  ~TwoBase() override {
    ++destroyed;
  }

  // Number of `TwoBase` destructors that have run.
  static uint64_t destroyed;
};

// Forward declaration for Rust function, including the lib.rs.h caused problems
kj::Own<OpaqueCxxClass> modify_own_return(kj::Own<OpaqueCxxClass> cpp_own);
// Rust function that takes in a cpp_own. Should cause C++ exception if the own is NULL
void null_exception_test(kj::Own<OpaqueCxxClass> cpp_own);
// Rust function that calls `null_kj_own` and tries to return it
kj::Own<OpaqueCxxClass> get_null();
// Rust function that takes ownweship and drops it
void take_own(kj::Own<OpaqueCxxClass> cpp_own);
// Rust function that takes ownership of a `SecondBase` and drops it
void take_second_base(kj::Own<SecondBase> own);

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

// Hands Rust a `kj::heap<TwoBase>()` as `kj::Own<SecondBase>`.
kj::Own<SecondBase> heap_two_base();
// Rust drops a `kj::Own<SecondBase>` to a stack `TwoBase` whose disposer records the address it was
// given; true when that was the complete object's address.
bool rust_drop_recorded_two_base_driver();
// Rust drops a `kj::heap<TwoBase>()` held as `kj::Own<SecondBase>`; returns how many `TwoBase`
// destructors ran.
uint64_t rust_drop_heap_two_base_driver();
uint64_t two_base_destroyed();
kj::Own<TwoBase> heap_two_base_complete();

}  // namespace kj_rs_demo
