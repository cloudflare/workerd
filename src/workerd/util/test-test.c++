// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "test.h"

namespace workerd {
namespace {

KJ_TEST("can check thrown exception type and message") {

  WD_EXPECT_THROW(KJ_EXCEPTION(DISCONNECTED, "foo"),
      kj::throwRecoverableException(KJ_EXCEPTION(DISCONNECTED, "foo")));

  {
    KJ_EXPECT_LOG(ERROR, "exception description didn't match");

    WD_EXPECT_THROW(KJ_EXCEPTION(DISCONNECTED, "bar"),
        kj::throwRecoverableException(KJ_EXCEPTION(DISCONNECTED, "foo")));
  }

  {
    KJ_EXPECT_LOG(ERROR, "code threw wrong exception type");

    WD_EXPECT_THROW(KJ_EXCEPTION(UNIMPLEMENTED, "foo"),
        kj::throwRecoverableException(KJ_EXCEPTION(DISCONNECTED, "foo")));
  }

  {
    KJ_EXPECT_LOG(ERROR, "code did not throw");

    WD_EXPECT_THROW(KJ_EXCEPTION(DISCONNECTED, "foo"), {});
  }
}

}  // namespace
}  // namespace workerd
