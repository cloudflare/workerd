// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "weak-refs.h"

#include <kj/memory.h>
#include <kj/test.h>

namespace workerd {
namespace {

KJ_TEST("WeakOwner weak handles go null when the owner is destroyed") {
  auto owner = kj::heap<WeakOwner<kj::String>>(kj::str("value"));
  KJ_EXPECT(owner->get() == "value");

  auto weak = owner->addWeak();
  KJ_EXPECT(KJ_ASSERT_NONNULL(weak.tryGet()).get() == "value");

  owner = nullptr;
  KJ_EXPECT(weak.tryGet() == kj::none);
}

KJ_TEST("WeakOwner invalidates weak handles before its value is destroyed") {
  struct Value {
    explicit Value(kj::Weak<WeakOwner<Value>>& weak): weak(weak) {}
    ~Value() noexcept(false) {
      KJ_EXPECT(weak.tryGet() == kj::none);
    }

    kj::Weak<WeakOwner<Value>>& weak;
  };

  kj::Weak<WeakOwner<Value>> weak = nullptr;
  auto owner = kj::heap<WeakOwner<Value>>(weak);
  weak = owner->addWeak();
  owner = nullptr;
}

}  // namespace
}  // namespace workerd
