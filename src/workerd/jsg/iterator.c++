// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "iterator.h"

namespace workerd::jsg {

kj::Maybe<jsg::Promise<void>&> AsyncIteratorImpl::maybeCurrent() {
  if (!pendingStack.empty()) {
    return pendingStack.back();
  }
  return kj::none;
}

void AsyncIteratorImpl::pushCurrent(Promise<void> promise) {
  pendingStack.push_back(kj::mv(promise));
}

void AsyncIteratorImpl::popCurrent() {
  if (!pendingStack.empty()) {
    pendingStack.pop_front();
  }
}

void AsyncIteratorImpl::visitForGc(jsg::GcVisitor& visitor) {
  visitor.visitAll(pendingStack);
}

}  // namespace workerd::jsg
