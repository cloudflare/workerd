#include <kj/test.h>
#include "test-fixture.h"

namespace workerd {
namespace {

KJ_TEST("setup/destroy") {
   TestFixture fixture;
}

KJ_TEST("single void runInIoContext run") {
  TestFixture fixture;
  uint runCount = 0;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    runCount++;
  });

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
    fixture.runInIoContext([&](const TestFixture::Environment& env) {
      runCount++;
    });

    KJ_EXPECT(runCount == i + 1);
  }
}

KJ_TEST("2 fixtures in a row with single runInIoContext run") {
  uint runCount = 0;

  for (uint i = 0; i < 2; i++) {
    TestFixture fixture;
    fixture.runInIoContext([&](const TestFixture::Environment& env) {
      runCount++;
    });

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
      env.compileAndRunScript("throw new Error('let_me_through');");
      return kj::READY_NOW;
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
    env.compileAndRunScript("throw new Error('test_error');");
    return kj::READY_NOW;
  }, kj::arr("test_error"_kj));

  KJ_EXPECT(runCount == 1);
}

KJ_TEST("compileAndRunScript") {
  TestFixture fixture;
  uint runCount = 0;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    runCount++;
    auto result = env.compileAndRunScript("42;");
    v8::String::Utf8Value value(env.isolate, result);
    KJ_EXPECT(*value == "42"_kj);
  });
  KJ_EXPECT(runCount == 1);
}

KJ_TEST("compileAndRunScript context access") {
  TestFixture fixture;
  uint runCount = 0;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    runCount++;
    auto result = env.compileAndRunScript("btoa([1,2,3,4,5]);");
    v8::String::Utf8Value value(env.isolate, result);
    KJ_EXPECT(*value == "MSwyLDMsNCw1"_kj);
  });
  KJ_EXPECT(runCount == 1);
}

KJ_TEST("compileAndRunScript exception handling") {
  TestFixture fixture;
  uint runCount = 0;
  uint exceptionCount = 0;

  try {
    fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
      runCount++;
      env.compileAndRunScript("throw new Error('test_error');");
      KJ_FAIL_REQUIRE("shouldn't happen");
    });
  } catch (kj::Exception& e) {
    exceptionCount++;
    KJ_EXPECT(e.getDescription() == "jsg.Error: test_error"_kj);
  }

  KJ_EXPECT(runCount == 1);
  KJ_EXPECT(exceptionCount == 1);
}

KJ_TEST("compileAndInstantiateModule") {
  TestFixture fixture;
  uint runCount = 0;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    runCount++;
    auto context = env.isolate->GetCurrentContext();

    auto ns = env.compileAndInstantiateModule("testFixtureTest",
        "export function init() { return 42; }"_kj);
    auto fn = ns->Get(context, jsg::v8Str(env.isolate, "init")).ToLocalChecked();
    KJ_EXPECT(fn->IsFunction());
    auto callResult = v8::Function::Cast(*fn)->
        Call(context, context->Global(), 0, nullptr).ToLocalChecked();
    v8::String::Utf8Value value(env.isolate, callResult);
    KJ_EXPECT(*value == "42"_kj);
  });

  KJ_EXPECT(runCount == 1);
}

KJ_TEST("runRequest") {
  TestFixture fixture({
    .mainModuleSource = R"SCRIPT(
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
    TestFixture fixture({
      .mainModuleSource = R"SCRIPT(
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


} // namespace
} // namespace workerd
