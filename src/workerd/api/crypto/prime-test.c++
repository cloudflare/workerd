#include "prime.h"

#include <kj/test.h>

namespace workerd::api {
namespace {

KJ_TEST("checkPrime rejects excessive num_checks") {
  uint8_t buf[] = {0x07};
  KJ_EXPECT_THROW_MESSAGE("Invalid number of checks", checkPrime(kj::arrayPtr(buf, 1u), 65));
}

KJ_TEST("checkPrime rejects oversized candidate") {
  auto bigBuf = kj::heapArray<kj::byte>(2000);
  memset(bigBuf.begin(), 0xFF, bigBuf.size());
  KJ_EXPECT_THROW_MESSAGE("exceeds maximum size", checkPrime(bigBuf.asPtr(), 1));
}

KJ_TEST("checkPrime accepts valid inputs") {
  uint8_t buf7[] = {0x07};
  uint8_t buf9[] = {0x09};
  KJ_EXPECT(checkPrime(kj::arrayPtr(buf7, 1u), 10) == true);
  KJ_EXPECT(checkPrime(kj::arrayPtr(buf9, 1u), 10) == false);
}

}  // namespace
}  // namespace workerd::api
