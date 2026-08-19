#pragma once

#include "kj-rs-demo/test-own.h"

#include <kj/common.h>

#include <cstdint>

namespace kj_rs_demo {

struct Shared;
class OpaqueCxxClass;

kj::Maybe<Shared> return_maybe_shared_some();
kj::Maybe<Shared> return_maybe_shared_none();

kj::Maybe<int64_t> return_maybe();
kj::Maybe<int64_t> return_maybe_none();

kj::Maybe<const int64_t&> return_maybe_ref_some();
kj::Maybe<const int64_t&> return_maybe_ref_none();

kj::Maybe<kj::Own<OpaqueCxxClass>> return_maybe_own_none();
kj::Maybe<kj::Own<OpaqueCxxClass>> return_maybe_own_some();

void take_maybe_own_cxx(kj::Maybe<kj::Own<OpaqueCxxClass>> maybe);

void cxx_take_maybe_shared_some(kj::Maybe<Shared> maybe);
void cxx_take_maybe_shared_none(kj::Maybe<Shared> maybe);
void cxx_take_maybe_ref_shared_some(kj::Maybe<Shared const&> maybe);
void cxx_take_maybe_ref_shared_none(kj::Maybe<Shared const&> maybe);

void test_maybe_reference_shared_own_driver();

kj::Maybe<rust::u8> test_maybe_u8_some();
kj::Maybe<rust::u16> test_maybe_u16_some();
kj::Maybe<rust::u32> test_maybe_u32_some();
kj::Maybe<rust::u64> test_maybe_u64_some();
kj::Maybe<rust::usize> test_maybe_usize_some();
kj::Maybe<rust::i8> test_maybe_i8_some();
kj::Maybe<rust::i16> test_maybe_i16_some();
kj::Maybe<rust::i32> test_maybe_i32_some();
kj::Maybe<rust::i64> test_maybe_i64_some();
kj::Maybe<rust::isize> test_maybe_isize_some();
kj::Maybe<rust::f32> test_maybe_f32_some();
kj::Maybe<rust::f64> test_maybe_f64_some();
kj::Maybe<bool> test_maybe_bool_some();
kj::Maybe<rust::Str> test_maybe_str_some();
kj::Maybe<rust::u8> test_maybe_u8_none();
kj::Maybe<rust::u16> test_maybe_u16_none();
kj::Maybe<rust::u32> test_maybe_u32_none();
kj::Maybe<rust::u64> test_maybe_u64_none();
kj::Maybe<rust::usize> test_maybe_usize_none();
kj::Maybe<rust::i8> test_maybe_i8_none();
kj::Maybe<rust::i16> test_maybe_i16_none();
kj::Maybe<rust::i32> test_maybe_i32_none();
kj::Maybe<rust::i64> test_maybe_i64_none();
kj::Maybe<rust::isize> test_maybe_isize_none();
kj::Maybe<rust::f32> test_maybe_f32_none();
kj::Maybe<rust::f64> test_maybe_f64_none();
kj::Maybe<bool> test_maybe_bool_none();
kj::Maybe<rust::Str> test_maybe_str_none();
kj::Maybe<rust::Slice<const kj::byte>> test_maybe_u8_slice_some();
kj::Maybe<rust::Slice<const kj::byte>> test_maybe_u8_slice_none();
kj::Maybe<uint64_t&> test_maybe_pin_mut_some();
kj::Maybe<uint64_t&> test_maybe_pin_mut_none();

// `kj::Maybe<::rust::String>` round-trip. The `_empty` variant returns `Some("")` -- distinct from
// `_none` -- to prove the FFI preserves the some/none distinction rather than folding an empty
// string into `kj::none`.
kj::Maybe<::rust::String> test_maybe_string_some();
kj::Maybe<::rust::String> test_maybe_string_empty();
kj::Maybe<::rust::String> test_maybe_string_none();

void cxx_take_maybe_string_some(kj::Maybe<::rust::String> maybe);
void cxx_take_maybe_string_empty(kj::Maybe<::rust::String> maybe);
void cxx_take_maybe_string_none(kj::Maybe<::rust::String> maybe);

}  // namespace kj_rs_demo
