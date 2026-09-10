#include "kj-rs-demo/test_date.rs.h"
#include "test-date.h"

#include <kj/test.h>

namespace kj_rs_demo {
namespace {

KJ_TEST("C++ calls Rust FFI functions") {
  // Test C++ calling Rust functions directly via FFI

  // Test return functions - C++ calls Rust, Rust returns KjDate
  auto rust_epoch = r_return_date_epoch();
  KJ_EXPECT(rust_epoch == kj::UNIX_EPOCH);

  auto rust_specific = r_return_date_specific();
  auto expected_specific = kj::UNIX_EPOCH + (5000000000LL * kj::NANOSECONDS);  // 5 seconds
  KJ_EXPECT(rust_specific == expected_specific);

  int64_t test_nanos = 5000000000LL;
  auto rust_from_nanos = r_return_date_from_nanos(test_nanos);
  auto expected_from_nanos = kj::UNIX_EPOCH + (test_nanos * kj::NANOSECONDS);
  KJ_EXPECT(rust_from_nanos == expected_from_nanos);
}

KJ_TEST("C++ sends dates to Rust FFI functions") {
  // Test take functions - C++ passes KjDate to Rust for verification

  auto epoch = kj::UNIX_EPOCH;
  r_take_date_epoch(epoch);  // Should not throw/assert

  int64_t test_nanos = 7500000000LL;
  auto test_date = kj::UNIX_EPOCH + (test_nanos * kj::NANOSECONDS);
  r_take_date_and_verify_nanos(test_date, test_nanos);  // Should not throw/assert
}

KJ_TEST("C++ and Rust FFI round-trip and verification") {
  // Test complex interactions between C++ and Rust via FFI

  // Round-trip test: C++ -> Rust -> C++
  auto original = kj::UNIX_EPOCH + (888999000LL * kj::NANOSECONDS);
  auto rust_round_tripped = r_roundtrip_date(original);
  KJ_EXPECT(r_verify_date_equality(original, rust_round_tripped));

  // Verification functions: C++ asks Rust to verify dates
  auto date1 = kj::UNIX_EPOCH + (1234567890LL * kj::NANOSECONDS);
  auto date2 = kj::UNIX_EPOCH + (1234567890LL * kj::NANOSECONDS);
  auto date3 = kj::UNIX_EPOCH + (9876543210LL * kj::NANOSECONDS);

  KJ_EXPECT(r_verify_date_equality(date1, date2));
  KJ_EXPECT(!r_verify_date_equality(date1, date3));

  // Ordering verification: C++ asks Rust to verify ordering
  auto earlier = kj::UNIX_EPOCH + (1000000000LL * kj::NANOSECONDS);
  auto later = kj::UNIX_EPOCH + (2000000000LL * kj::NANOSECONDS);
  KJ_EXPECT(r_verify_date_ordering(earlier, later));
  KJ_EXPECT(!r_verify_date_ordering(later, earlier));

  // Extraction test: C++ asks Rust to extract nanoseconds
  int64_t expected_nanos = 3333333333LL;
  auto test_date = kj::UNIX_EPOCH + (expected_nanos * kj::NANOSECONDS);
  int64_t rust_extracted = r_extract_nanoseconds(test_date);
  KJ_EXPECT(rust_extracted == expected_nanos);
}

}  // namespace
}  // namespace kj_rs_demo
