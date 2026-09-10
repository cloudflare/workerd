#pragma once

#include <kj/time.h>

#include <cstdint>

namespace kj_rs {
namespace repr {

inline std::int64_t toNanos(kj::Date date) {
  return (date - kj::origin<kj::Date>()) / kj::NANOSECONDS;
}

inline kj::Date fromNanos(std::int64_t nanos) {
  return kj::origin<kj::Date>() + (nanos * kj::NANOSECONDS);
}

}  // namespace repr
}  // namespace kj_rs