#include <workerd/rust/async/executor-guarded.h>

#include <kj/debug.h>

namespace workerd::rust::async {

bool isCurrent(const kj::Executor& executor) {
  return &executor == &kj::getCurrentThreadExecutor();
}

void requireCurrent(const kj::Executor& executor, kj::LiteralStringConst message) {
  KJ_REQUIRE(isCurrent(executor), message);
}

}  // namespace workerd::rust::async
