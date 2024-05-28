// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
#include <kj/test.h>

namespace workerd {

// Checks that code throws a kj::Exception matching the type and message of the given exception.
//
// Note: Performs no special handling for JsExceptionThrown, so tests that need to explicitly
// detect thrown JS exceptions will probably want to use a separate macro.
#define WD_EXPECT_THROW(expException, code, ...)                                                   \
  do {                                                                                             \
    auto expExcObj = expException;                                                                 \
    KJ_IF_SOME(e, ::kj::runCatchingExceptions([&]() { (void)({ code; }); })) {                     \
      KJ_EXPECT(e.getType() == expExcObj.getType(), "code threw wrong exception type: " #code,     \
          e, ##__VA_ARGS__);                                                                       \
      KJ_EXPECT(e.getDescription() == expExcObj.getDescription(),                                  \
          "exception description didn't match", e, ##__VA_ARGS__);                                 \
    } else {                                                                                       \
      KJ_FAIL_EXPECT("code did not throw: " #code, ##__VA_ARGS__);                                 \
    }                                                                                              \
  } while (false)

} // namespace workerd
