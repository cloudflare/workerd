// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "cf-property.h"
#include <workerd/jsg/jsg.h>
#include <workerd/tests/test-fixture.h>

namespace workerd::api {
namespace {

KJ_TEST("Test that CfProperty is frozen by default") {
  TestFixture fixture({
    .mainModuleSource = R"SCRIPT(
      export default {
        async fetch(request) {
          request.cf.foo = 100;
          return new Response(`OK`);
        },
      };
    )SCRIPT"_kj});

  try {
    auto result = fixture.runRequest(kj::HttpMethod::POST, "http://www.example.com"_kj, "TEST"_kj);
    KJ_FAIL_REQUIRE("exception expected");
  } catch (kj::Exception& e) {
    KJ_EXPECT(e.getDescription() == "jsg.TypeError: Cannot add property foo, object is not extensible"_kj);
  }
}


KJ_TEST("Test that CfProperty::deepClone returns editable object") {
  TestFixture fixture({
    .mainModuleSource = R"SCRIPT(
      export default {
        async fetch(request) {
          const req = new Request(request);
          req.cf.foo = 100;
          return new Response(`OK`);
        },
      };
    )SCRIPT"_kj});
  auto result = fixture.runRequest(kj::HttpMethod::POST, "http://www.example.com"_kj, "TEST"_kj);
  KJ_EXPECT(result.statusCode == 200);
}

}  // namespace
}  // namespace workerd::api
