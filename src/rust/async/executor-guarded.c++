#include <workerd/rust/async/executor-guarded.h>

#include <kj/debug.h>

namespace workerd::rust::async {

void requireCurrent(const kj::Executor& executor, kj::LiteralStringConst message) {
  KJ_REQUIRE(&executor == &kj::getCurrentThreadExecutor(), message);
}

}  // namespace workerd::rust::async
