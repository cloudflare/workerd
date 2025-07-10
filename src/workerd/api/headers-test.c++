// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "http.h"

#include <workerd/io/promise-wrapper.h>
#include <workerd/jsg/jsg-test.h>
#include <workerd/jsg/jsg.h>

#include <kj/hash.h>

namespace workerd::api {
namespace {

jsg::V8System v8System;

struct HeadersContext: public jsg::Object, public jsg::ContextGlobal {

  bool test(jsg::Lock& js) {

    // Verifies that header key and value memory is accounted for
    // in the isolate external memory correctly.

    // First, let's make sure just the ByteString itself is accounted.
    auto before = js.v8Isolate->GetExternalMemory();

    v8::HeapStatistics stats;

    // The GetHeapStatistics reports external memory but with
    // some processing that means it may not actually reflect
    // the correct value. This is why we add the v8 patch that
    // adds the GetExternalMemory() API, which yields a more
    // accurate immediate measure of the external memory. This
    // check is added to the test just so we can verify that
    // the differences are there. If v8 changes this we should
    // be notified.
    js.v8Isolate->GetHeapStatistics(&stats);
    auto statsBefore = stats.external_memory();
    KJ_ASSERT(statsBefore == 0);

    {
      auto str = js.accountedByteString("hello"_kj);
      auto after = js.v8Isolate->GetExternalMemory();
      KJ_ASSERT(after - before == 6);

      // The GetHeapStatistics API is not expected to reflect
      // the correct external memory value.
      v8::HeapStatistics stats;
      js.v8Isolate->GetHeapStatistics(&stats);
      auto statsAfter = stats.external_memory();
      KJ_ASSERT(statsBefore == statsAfter);
    }
    // Then check again after the str is destroyed
    auto after = js.v8Isolate->GetExternalMemory();
    KJ_ASSERT(after == before);

    // Now make sure the Headers object accounts for the memory.
    {
      auto headers = js.alloc<workerd::api::Headers>();
      headers->append(js, js.accountedByteString("KEY"), js.accountedByteString("value"));
      auto after = js.v8Isolate->GetExternalMemory();
      // Why 14? That's surprising! I'm happy you asked. Each header
      // entry ends up storing the key twice as an artifact of an
      // old implementation decision. This is something we can probably
      // refactor at some point.
      KJ_ASSERT(after - before == 10);
    }
    after = js.v8Isolate->GetExternalMemory();
    KJ_ASSERT(after == before);

    {
      kj::HttpHeaderTable::Builder builder;
      auto kFoo = builder.add("foo");
      auto headersTable = builder.build();
      kj::HttpHeaders kjHeaders(*headersTable);
      kjHeaders.set(kFoo, "test");

      auto headers =
          js.alloc<workerd::api::Headers>(js, kjHeaders, workerd::api::Headers::Guard::NONE);
      auto after = js.v8Isolate->GetExternalMemory();
      KJ_ASSERT(after - before == 9);
    }
    after = js.v8Isolate->GetExternalMemory();
    KJ_ASSERT(after == before);

    return true;
  }

  JSG_RESOURCE_TYPE(HeadersContext) {
    JSG_METHOD(test);
  }
};

JSG_DECLARE_ISOLATE_TYPE(HeadersIsolate,
    HeadersContext,
    workerd::api::Headers,
    jsg::TypeWrapperExtension<PromiseWrapper>);

KJ_TEST("Header memory is accounted for") {
  jsg::test::Evaluator<HeadersContext, HeadersIsolate, CompatibilityFlags::Reader> e(v8System);
  e.expectEval("test()", "boolean", "true");
}

}  // namespace
}  // namespace workerd::api
