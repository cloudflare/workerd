// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "iterator.h"

namespace workerd::jsg {

kj::Maybe<jsg::Promise<void>&> AsyncIteratorImpl::maybeCurrent() {
  return pendingStack.peekBack();
}

void AsyncIteratorImpl::pushCurrent(Promise<void> promise) {
  pendingStack.push(kj::mv(promise));
}

void AsyncIteratorImpl::popCurrent() {
  auto dropped KJ_UNUSED = KJ_ASSERT_NONNULL(pendingStack.pop());
}

void AsyncIteratorImpl::visitForGc(jsg::GcVisitor& visitor) {
  pendingStack.forEach([&](auto& p) { visitor.visit(p); });
}

}  // namespace workerd::jsg
