// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "test-fixture.h"

#include <kj/test.h>

namespace workerd {
namespace {

KJ_TEST("setup/destroy") {
  TestFixture fixture;
}

KJ_TEST("single void runInIoContext run") {
  TestFixture fixture;
  uint runCount = 0;

  fixture.runInIoContext([&](const TestFixture::Environment& env) { runCount++; });

  KJ_EXPECT(runCount == 1);
}

KJ_TEST("single runInIoContext with promise result") {
  TestFixture fixture;
  uint runCount = 0;

  auto result = fixture.runInIoContext([&](const TestFixture::Environment& env) {
    runCount++;
    return kj::Promise<int>(42);
  });

  KJ_EXPECT(runCount == 1);
  KJ_EXPECT(result == 42);
}

KJ_TEST("single runInIoContext with immediate result") {
  TestFixture fixture;
  uint runCount = 0;

  auto result = fixture.runInIoContext([&](const TestFixture::Environment& env) {
    runCount++;
    return 42;
  });

  KJ_EXPECT(runCount == 1);
  KJ_EXPECT(result == 42);
}

KJ_TEST("3 runInIoContext runs") {
  TestFixture fixture;
  uint runCount = 0;

  for (uint i = 0; i < 3; i++) {
    fixture.runInIoContext([&](const TestFixture::Environment& env) { runCount++; });

    KJ_EXPECT(runCount == i + 1);
  }
}

KJ_TEST("2 fixtures in a row with single runInIoContext run") {
  uint runCount = 0;

  for (uint i = 0; i < 2; i++) {
    TestFixture fixture;
    fixture.runInIoContext([&](const TestFixture::Environment& env) { runCount++; });

    KJ_EXPECT(runCount == i + 1);
  }
}

KJ_TEST("runInIoContext consuming ignored kj::Exception") {
  TestFixture fixture;
  uint runCount = 0;

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    runCount++;
    KJ_FAIL_REQUIRE("test_error");
  }, kj::arr("test_error"_kj));

  KJ_EXPECT(runCount == 1);
}

KJ_TEST("runInIoContext re-throwing kj::Exception") {
  TestFixture fixture;
  uint runCount = 0;
  uint exceptionCount = 0;

  try {
    fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
      runCount++;
      KJ_FAIL_REQUIRE("let_me_through");
    }, kj::arr("test_error"_kj));
  } catch (kj::Exception& e) {
    KJ_EXPECT(e.getDescription() == "let_me_through"_kj);
    exceptionCount++;
  }

  KJ_EXPECT(exceptionCount == 1);
  KJ_EXPECT(runCount == 1);
}

KJ_TEST("runInIoContext re-throwing js exception") {
  TestFixture fixture;
  uint runCount = 0;
  uint exceptionCount = 0;

  try {
    fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
      runCount++;
      env.js.throwException(env.js.error("let_me_through"));
    }, kj::arr("test_error"_kj));
  } catch (kj::Exception& e) {
    KJ_EXPECT(e.getDescription() == "jsg.Error: let_me_through"_kj);
    exceptionCount++;
  }

  KJ_EXPECT(runCount == 1);
  KJ_EXPECT(exceptionCount == 1);
}

KJ_TEST("runInIoContext consuming ignored js exception") {
  TestFixture fixture;
  uint runCount = 0;

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    runCount++;
    env.js.throwException(env.js.error("test_error"));
  }, kj::arr("test_error"_kj));

  KJ_EXPECT(runCount == 1);
}

KJ_TEST("runRequest") {
  TestFixture fixture({.mainModuleSource = R"SCRIPT(
      export default {
        async fetch(request) {
          const body = await(await request.blob()).text();
          return new Response(`${request.method} ${request.url} ${body}`, { status: 202 });
        },
      };
    )SCRIPT"_kj});

  auto result = fixture.runRequest(kj::HttpMethod::POST, "http://www.example.com"_kj, "TEST"_kj);
  KJ_EXPECT(result.statusCode == 202);
  KJ_EXPECT(result.body == "POST http://www.example.com TEST"_kj);
}

KJ_TEST("module import failure") {
  KJ_EXPECT_LOG(ERROR, "script startup threw exception");

  try {
    TestFixture fixture({.mainModuleSource = R"SCRIPT(
        import * from "bad-module";

        export default {
          async fetch(request) {
            return new Response("OK");
          },
        };
      )SCRIPT"_kj});

    KJ_FAIL_REQUIRE("exception expected");
  } catch (kj::Exception& e) {
    KJ_EXPECT(e.getDescription() == "script startup threw exception"_kj);
  }
}

}  // namespace
}  // namespace workerd
