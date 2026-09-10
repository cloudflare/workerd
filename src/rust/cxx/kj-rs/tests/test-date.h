#pragma once

#include <kj/common.h>
#include <kj/time.h>

#include <cstdint>

namespace kj_rs_demo {

// Helper functions to create kj::Date values for testing
kj::Date c_create_date_epoch();
kj::Date c_create_date_from_nanos(int64_t nanoseconds);
kj::Date c_create_date_now();

// Functions to test C++ -> Rust Date passing
kj::Date c_return_date_epoch();
kj::Date c_return_date_from_nanos(int64_t nanoseconds);
kj::Date c_return_5_sec_after_epoch();

// Functions to test Rust -> C++ Date passing
void c_take_date_epoch(kj::Date date);
void c_take_date_7777777777(kj::Date date);
void c_take_date_and_verify_nanos(kj::Date date, int64_t expected_nanos);

// Round-trip testing functions
kj::Date c_roundtrip_date(kj::Date date);
bool c_verify_date_equality(kj::Date date1, kj::Date date2);
bool c_verify_date_ordering(kj::Date earlier, kj::Date later);

// Conversion testing functions
int64_t c_extract_nanoseconds_from_date(kj::Date date);

}  // namespace kj_rs_demo
