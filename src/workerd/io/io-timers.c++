#include "io-timers.h"

namespace workerd {

TimeoutId TimeoutId::Generator::getNext() {
  // The maximum integer value that we can represent as a double to convey to jsg.
  constexpr ValueType MAX_SAFE_INTEGER = (1ull << 53) - 1;

  auto id = nextId++;
  if (nextId > MAX_SAFE_INTEGER) {
    KJ_LOG(WARNING, "Unable to set timeout because there are no more unique ids");
    JSG_FAIL_REQUIRE(Error, "Unable to set timeout because there are no more unique ids "
        "less than Number.MAX_SAFE_INTEGER.");
  }
  return TimeoutId(id);
}

TimeoutManager::TimeoutParameters::TimeoutParameters(
    bool repeat, int64_t msDelay, jsg::Function<void()> function)
    : repeat(repeat), msDelay(msDelay), function(kj::mv(function)) {
  // Don't allow pushing Date.now() backwards! This should be checked before TimeoutParameters
  // is created but just in case...
  if (msDelay < 0) {
    this->msDelay = 0;
  }
}

}  // namespace workerd
