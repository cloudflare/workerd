#include "test-maybe.h"

#include "kj-rs-demo/lib.rs.h"

#include <kj/common.h>
#include <kj/debug.h>
#include <kj/memory.h>

#include <cstdint>

namespace kj_rs_demo {

kj::Maybe<Shared> return_maybe_shared_some() {
  return kj::Maybe<Shared>(Shared{14});
}

kj::Maybe<Shared> return_maybe_shared_none() {
  return kj::Maybe<Shared>(kj::none);
}

kj::Maybe<int64_t> return_maybe() {
  kj::Maybe<int64_t> ret = kj::some(14);
  return kj::mv(ret);
}

kj::Maybe<int64_t> return_maybe_none() {
  kj::Maybe<int64_t> ret = kj::none;
  return kj::mv(ret);
}

// Static var to return non-dangling pointer without heap allocating
int64_t var = 14;

static_assert(sizeof(kj::Maybe<const int64_t&>) == sizeof(const int64_t&));
kj::Maybe<const int64_t&> return_maybe_ref_none() {
  kj::Maybe<const int64_t&> ret = kj::none;
  return kj::mv(ret);
}

kj::Maybe<const int64_t&> return_maybe_ref_some() {
  const int64_t& val = var;
  kj::Maybe<const int64_t&> ret = kj::some(val);
  return kj::mv(ret);
}

kj::Maybe<kj::Own<OpaqueCxxClass>> return_maybe_own_none() {
  kj::Maybe<kj::Own<OpaqueCxxClass>> maybe = kj::none;
  return kj::mv(maybe);
}

kj::Maybe<kj::Own<OpaqueCxxClass>> return_maybe_own_some() {
  kj::Maybe<kj::Own<OpaqueCxxClass>> ret = kj::heap<OpaqueCxxClass>(14);
  return kj::mv(ret);
}

void take_maybe_own_cxx(kj::Maybe<kj::Own<OpaqueCxxClass>> maybe) {
  KJ_IF_SOME(val, maybe) {
    KJ_ASSERT(val->getData() == 14);
  }
}

void cxx_take_maybe_shared_some(kj::Maybe<Shared> maybe) {
  KJ_IF_SOME(val, maybe) {
    KJ_ASSERT(val.i == -37);
  }
}

void cxx_take_maybe_shared_none(kj::Maybe<Shared> maybe) {
  KJ_IF_SOME(_, maybe) {
    KJ_FAIL_ASSERT();
  }
}

void cxx_take_maybe_ref_shared_some(kj::Maybe<Shared const&> maybe) {
  KJ_IF_SOME(val, maybe) {
    KJ_ASSERT(val.i == -38);
  }
}

void cxx_take_maybe_ref_shared_none(kj::Maybe<Shared const&> maybe) {
  KJ_IF_SOME(_, maybe) {
    KJ_FAIL_ASSERT();
  }
}

void test_maybe_reference_shared_own_driver() {
  kj::Maybe<kj::Own<OpaqueCxxClass>> maybe_own_some = return_maybe_own_some();
  uint64_t num = 15;
  kj::Maybe<uint64_t&> maybe_ref_some = kj::Maybe<uint64_t&>(num);
  kj::Maybe<Shared> maybe_shared_some = return_maybe_shared_some();

  auto maybe_own = take_maybe_own_ret(kj::mv(maybe_own_some));
  KJ_IF_SOME(i, maybe_own) {
    KJ_ASSERT(i->getData() == 42);
  } else {
    KJ_FAIL_ASSERT("Not reached");
  }
  take_maybe_own(kj::mv(maybe_own));

  auto maybe_ref = take_maybe_ref_ret(kj::mv(maybe_ref_some));
  take_maybe_ref(kj::mv(maybe_ref));

  auto maybe_shared = take_maybe_shared_ret(kj::mv(maybe_shared_some));
  KJ_IF_SOME(_, maybe_shared) {
    KJ_FAIL_ASSERT("Returns none, so unreached");
  }
  take_maybe_shared(kj::mv(maybe_shared));
}

kj::Maybe<rust::u8> test_maybe_u8_some() {
  return kj::some(234);
}
kj::Maybe<rust::u16> test_maybe_u16_some() {
  return kj::some(235);
}
kj::Maybe<rust::u32> test_maybe_u32_some() {
  return kj::some(236);
}
kj::Maybe<rust::u64> test_maybe_u64_some() {
  return kj::some(237);
}
kj::Maybe<rust::usize> test_maybe_usize_some() {
  return kj::some(238);
}
kj::Maybe<rust::i8> test_maybe_i8_some() {
  return kj::some(97);
}
kj::Maybe<rust::i16> test_maybe_i16_some() {
  return kj::some(240);
}
kj::Maybe<rust::i32> test_maybe_i32_some() {
  return kj::some(241);
}
kj::Maybe<rust::i64> test_maybe_i64_some() {
  return kj::some(242);
}
kj::Maybe<rust::isize> test_maybe_isize_some() {
  return kj::some(243);
}
kj::Maybe<rust::f32> test_maybe_f32_some() {
  return kj::some(244.678);
}
kj::Maybe<rust::f64> test_maybe_f64_some() {
  return kj::some(245.678);
}
kj::Maybe<bool> test_maybe_bool_some() {
  return kj::some(false);
}
kj::Maybe<rust::Str> test_maybe_str_some() {
  return kj::some("hello");
}

kj::Maybe<rust::u8> test_maybe_u8_none() {
  return kj::none;
}
kj::Maybe<rust::u16> test_maybe_u16_none() {
  return kj::none;
}
kj::Maybe<rust::u32> test_maybe_u32_none() {
  return kj::none;
}
kj::Maybe<rust::u64> test_maybe_u64_none() {
  return kj::none;
}
kj::Maybe<rust::usize> test_maybe_usize_none() {
  return kj::none;
}
kj::Maybe<rust::i8> test_maybe_i8_none() {
  return kj::none;
}
kj::Maybe<rust::i16> test_maybe_i16_none() {
  return kj::none;
}
kj::Maybe<rust::i32> test_maybe_i32_none() {
  return kj::none;
}
kj::Maybe<rust::i64> test_maybe_i64_none() {
  return kj::none;
}
kj::Maybe<rust::isize> test_maybe_isize_none() {
  return kj::none;
}
kj::Maybe<rust::f32> test_maybe_f32_none() {
  return kj::none;
}
kj::Maybe<rust::f64> test_maybe_f64_none() {
  return kj::none;
}
kj::Maybe<bool> test_maybe_bool_none() {
  return kj::none;
}
kj::Maybe<rust::Str> test_maybe_str_none() {
  return kj::none;
}
kj::Maybe<rust::Slice<const kj::byte>> test_maybe_u8_slice_some() {
  return "abc"_kjb.as<::kj_rs::Rust>();
}
kj::Maybe<rust::Slice<const kj::byte>> test_maybe_u8_slice_none() {
  return kj::none;
}

kj::Maybe<uint64_t&> test_maybe_pin_mut_some() {
  static uint64_t num = 15;
  return kj::Maybe<uint64_t&>(num);
}
kj::Maybe<uint64_t&> test_maybe_pin_mut_none() {
  return kj::none;
}

kj::Maybe<::rust::String> test_maybe_string_some() {
  return kj::some(::rust::String("hello"));
}
kj::Maybe<::rust::String> test_maybe_string_empty() {
  return kj::some(::rust::String(""));
}
kj::Maybe<::rust::String> test_maybe_string_none() {
  return kj::none;
}

void cxx_take_maybe_string_some(kj::Maybe<::rust::String> maybe) {
  KJ_IF_SOME(val, maybe) {
    KJ_ASSERT(kj::str(val) == "world");
  } else {
    KJ_FAIL_ASSERT("expected Some(\"world\"), got None");
  }
}
void cxx_take_maybe_string_empty(kj::Maybe<::rust::String> maybe) {
  KJ_IF_SOME(val, maybe) {
    KJ_ASSERT(kj::str(val) == "");
  } else {
    KJ_FAIL_ASSERT("expected Some(\"\"), got None -- empty string must not be conflated with none");
  }
}
void cxx_take_maybe_string_none(kj::Maybe<::rust::String> maybe) {
  KJ_IF_SOME(_, maybe) {
    KJ_FAIL_ASSERT("expected None, got Some");
  }
}
}  // namespace kj_rs_demo
