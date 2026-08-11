#include "test-date.h"

#include <kj/debug.h>
#include <kj/exception.h>
#include <kj/time.h>

#include <chrono>

namespace kj_rs_demo {

kj::Date c_create_date_epoch() {
  return kj::UNIX_EPOCH;
}

kj::Date c_create_date_from_nanos(int64_t nanoseconds) {
  return kj::UNIX_EPOCH + (nanoseconds * kj::NANOSECONDS);
}

kj::Date c_return_date_epoch() {
  return kj::UNIX_EPOCH;
}

kj::Date c_return_date_from_nanos(int64_t nanoseconds) {
  return kj::UNIX_EPOCH + (nanoseconds * kj::NANOSECONDS);
}

kj::Date c_return_5_sec_after_epoch() {
  return kj::UNIX_EPOCH + (5000000000LL * kj::NANOSECONDS);
}

void c_take_date_epoch(kj::Date date) {
  KJ_ASSERT(date == kj::UNIX_EPOCH, "Expected Unix epoch date");
}

void c_take_date_7777777777(kj::Date date) {
  // Expect 7777777777 nanoseconds after Unix epoch (to match Rust test expectation)
  kj::Date expected = kj::UNIX_EPOCH + (7777777777LL * kj::NANOSECONDS);
  KJ_ASSERT(date == expected, "Expected specific date (7777777777 nanoseconds after epoch)");
}

void c_take_date_and_verify_nanos(kj::Date date, int64_t expected_nanos) {
  kj::Date expected = kj::UNIX_EPOCH + (expected_nanos * kj::NANOSECONDS);
  KJ_ASSERT(date == expected, "Date nanoseconds don't match expected value", expected_nanos);
}

kj::Date c_roundtrip_date(kj::Date date) {
  return date;
}

bool c_verify_date_equality(kj::Date date1, kj::Date date2) {
  return date1 == date2;
}

bool c_verify_date_ordering(kj::Date earlier, kj::Date later) {
  return earlier < later;
}

// Conversion testing functions
int64_t c_extract_nanoseconds_from_date(kj::Date date) {
  // Extract nanoseconds since Unix epoch
  return (date - kj::UNIX_EPOCH) / kj::NANOSECONDS;
}

}  // namespace kj_rs_demo