// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <kj/test.h>
#include <workerd/tests/test-fixture.h>

namespace workerd::api {
namespace {

KJ_TEST("node:buffer import") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setNodeJsCompat(true);

  TestFixture fixture({
    .featureFlags = flags.asReader(),
    .mainModuleSource = R"SCRIPT(
      import { Buffer } from 'node:buffer';

      export default {
        fetch(request) {
          return new Response(new Buffer("test").toString());
        },
      };
    )SCRIPT"_kj});

  auto response = fixture.runRequest(kj::HttpMethod::POST, "http://www.example.com"_kj, ""_kj);

  KJ_EXPECT(response.statusCode == 200);
  KJ_EXPECT(response.body == "test");
}

KJ_TEST("node:buffer import without capability") {
  KJ_EXPECT_LOG(ERROR, "script startup threw exception");

  try {
    TestFixture fixture({
      .mainModuleSource = R"SCRIPT(
        import { Buffer } from 'node:buffer';

        export default {
          fetch(request) {
            return new Response(new Buffer("test").toString());
          },
        };
      )SCRIPT"_kj});

    KJ_UNREACHABLE;
  } catch (kj::Exception& e) {
    KJ_EXPECT(e.getDescription() == "script startup threw exception"_kj);
  }
}

}  // namespace
}  // namespace workerd::api

