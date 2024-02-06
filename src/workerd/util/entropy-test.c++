#include "entropy.h"
#include <kj/test.h>

namespace workerd {
namespace {

KJ_TEST("Mock entropy source (uses a counter)") {
  kj::byte buffer[10];
  auto mock = getMockEntropySource();
  mock->generate(buffer);
  for (int i = 0; i < 10; i++) {
    KJ_ASSERT(buffer[i] == i);
  }
  mock->generate(buffer);
  for (int i = 0; i < 10; i++) {
    KJ_ASSERT(buffer[i] == i + 10);
  }
}

KJ_TEST("Mock entropy source (uses a fixed char)") {
  kj::byte buffer[10];
  auto mock = getMockEntropySource('a');
  mock->generate(buffer);
  for (int i = 0; i < 10; i++) {
    KJ_ASSERT(buffer[i] == 'a');
  }
}

KJ_TEST("Fake entropy source (uses a fixed sequence)") {
  kj::byte buffer[10];
  getFakeEntropySource().generate(buffer);
  for (int i = 0; i < 10; i++) {
    KJ_ASSERT(buffer[i] == (i % 4) * 22 + 12);
  }
}

}  // namespace
}  // namespace workerd
