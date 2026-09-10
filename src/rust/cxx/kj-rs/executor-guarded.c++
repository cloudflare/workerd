#include "kj-rs/executor-guarded.h"

#include <kj/debug.h>

namespace kj_rs {

bool isCurrent(const kj::Executor& executor) {
  return &executor == &kj::getCurrentThreadExecutor();
}

void requireCurrent(const kj::Executor& executor, kj::LiteralStringConst message) {
  KJ_REQUIRE(isCurrent(executor), message);
}

}  // namespace kj_rs
