// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "global-scope.h"

#include <workerd/io/io-context.h>
#include <workerd/io/observer.h>
#include <workerd/tests/test-fixture.h>

#include <kj/test.h>

namespace workerd::api {
namespace {

// Counts how many times claimRetryTokenBeforeUserCode() is fired.
class CountingRequestObserver final: public RequestObserver {
 public:
  CountingRequestObserver(uint& count): count(count) {}

  void claimRetryTokenBeforeUserCode() override {
    count++;
  }

 private:
  uint& count;
};

// The claim hook must fire exactly once per fetch dispatch, immediately before user code runs. A
// worker that simply returns a Response (no subrequests) is enough to exercise the single dispatch
// into the exported fetch() handler.
KJ_TEST("claimRetryTokenBeforeUserCode fires once per fetch dispatch") {
  uint claimCount = 0;

  TestFixture fixture(TestFixture::SetupParams{
    .mainModuleSource = R"SCRIPT(
        export default {
          async fetch(request) {
            return new Response("OK");
          },
        };
      )SCRIPT"_kj,
    .requestObserverFactory =
        kj::Function<kj::Own<RequestObserver>()>([&]() -> kj::Own<RequestObserver> {
    return kj::refcounted<CountingRequestObserver>(claimCount);
  }),
  });

  auto result = fixture.runRequest(kj::HttpMethod::GET, "http://www.example.com"_kj, ""_kj);
  KJ_EXPECT(result.statusCode == 200);

  KJ_EXPECT(claimCount == 1, "expected the claim hook to fire exactly once before user code");
}

}  // namespace
}  // namespace workerd::api
