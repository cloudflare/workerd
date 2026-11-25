// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "exception.h"

#include <kj/test.h>

namespace workerd::jsg {
namespace {

KJ_TEST("tunneledErrorType() extracts an error tunneled through a failed assertion") {
  auto info = tunneledErrorType("expected foo == bar; jsg.TypeError: oops");
  KJ_EXPECT(info.message == "TypeError: oops");
  KJ_EXPECT(info.isJsgError);
  KJ_EXPECT(!info.isInternal);
}

KJ_TEST("tunneledErrorType() keeps the whole message of a failed assertion without a tunneled "
        "error") {
  kj::StringPtr message = "expected map.find(index) == kj::none; can't replace entry; index = 6";
  auto info = tunneledErrorType(message);
  KJ_EXPECT(info.message == message);
  KJ_EXPECT(!info.isJsgError);
  KJ_EXPECT(info.isInternal);

  KJ_EXPECT(annotateBroken(message, "broken.constructorFailed") ==
      kj::str("broken.constructorFailed; jsg-internal.Error: ", message));
}

}  // namespace
}  // namespace workerd::jsg
