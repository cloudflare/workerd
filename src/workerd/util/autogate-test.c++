// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "autogate.h"

#include <stdlib.h>

#include <kj/test.h>

namespace workerd::util {
namespace {

// Whether this is the @all-autogates variant, whose WORKERD_ALL_AUTOGATES_EXCEPT names
// test-workerd (see all_autogates_variant_excludes in BUILD.bazel).
bool allAutogates() {
  return getenv("WORKERD_ALL_AUTOGATES") != nullptr;
}

KJ_TEST("without a config, the gates WORKERD_ALL_AUTOGATES_EXCEPT names are off") {
  KJ_EXPECT(!Autogate::isEnabled(AutogateKey::TEST_WORKERD));
  KJ_EXPECT(Autogate::isEnabled(AutogateKey::V8_FAST_API) == allAutogates());
}

KJ_TEST("initAllAutogates() leaves the gates WORKERD_ALL_AUTOGATES_EXCEPT names off") {
  Autogate::initAllAutogates();
  KJ_DEFER(Autogate::deinitAutogate());
  KJ_EXPECT(Autogate::isEnabled(AutogateKey::TEST_WORKERD) == !allAutogates());
  KJ_EXPECT(Autogate::isEnabled(AutogateKey::V8_FAST_API));
}

KJ_TEST("initAllAutogates() keeps the gates a config enables") {
  Autogate::initAutogateNamesForTest({"test-workerd"}, IgnoreAllAutogatesEnv::YES);
  Autogate::initAllAutogates();
  KJ_DEFER(Autogate::deinitAutogate());
  KJ_EXPECT(Autogate::isEnabled(AutogateKey::TEST_WORKERD));
  KJ_EXPECT(Autogate::isEnabled(AutogateKey::V8_FAST_API));
}

KJ_TEST("initAutogate() enables a config's gates even when WORKERD_ALL_AUTOGATES_EXCEPT names "
        "them") {
  Autogate::initAutogateNamesForTest({"test-workerd"});
  KJ_DEFER(Autogate::deinitAutogate());
  KJ_EXPECT(Autogate::isEnabled(AutogateKey::TEST_WORKERD));
  KJ_EXPECT(Autogate::isEnabled(AutogateKey::V8_FAST_API) == allAutogates());
}

KJ_TEST("initAutogate() leaves excludedFromAll off under WORKERD_ALL_AUTOGATES") {
  Autogate::initAutogateNamesForTest(kj::ArrayPtr<const kj::StringPtr>(), IgnoreAllAutogatesEnv::NO,
      kj::arr(AutogateKey::V8_FAST_API));
  KJ_DEFER(Autogate::deinitAutogate());
  KJ_EXPECT(!Autogate::isEnabled(AutogateKey::V8_FAST_API));
  KJ_EXPECT(!Autogate::isEnabled(AutogateKey::TEST_WORKERD));
  KJ_EXPECT(Autogate::isEnabled(AutogateKey::TAIL_STREAM_REFACTOR) == allAutogates());
}

KJ_TEST("initAutogate() enables a config's gates even when excludedFromAll names them") {
  Autogate::initAutogateNamesForTest(kj::arr<kj::StringPtr>("v8-fast-api"),
      IgnoreAllAutogatesEnv::NO, kj::arr(AutogateKey::V8_FAST_API));
  KJ_DEFER(Autogate::deinitAutogate());
  KJ_EXPECT(Autogate::isEnabled(AutogateKey::V8_FAST_API));
}

}  // namespace
}  // namespace workerd::util
