// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "blob.h"
#include "form-data.h"
#include "streams.h"

#include <workerd/jsg/jsg-test.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api {
namespace {

jsg::V8System v8System;

static constexpr auto kBody = R"(--+
Content-Disposition: form-data; name="field0"

part0
--+
CONTENT-DISPOSITION: form-data;name="field1"

part1
--+
content-disposition:form-data;name="field0"

part2
--+
CoNTent-dIsposiTIOn: form-data; name="field1"

part3
--+--)"_kj;

static constexpr auto kUrlData = "field0=part0&field1=part1&field0=part2&field1=part3"_kj;

struct HeadersContext: public jsg::Object, public jsg::ContextGlobal {

  bool test(jsg::Lock& js) {

    // Verifies that FormData key and value memory is accounted for
    // in the isolate external memory correctly.

    auto before = js.v8Isolate->GetExternalMemory();

    {
      auto formData = js.alloc<workerd::api::FormData>();
      formData->append(js, js.accountedKjString("a"), js.accountedKjString("b"), kj::none);

      auto after = js.v8Isolate->GetExternalMemory();
      KJ_ASSERT(after - before, 4);
    }

    {
      auto formData = js.alloc<workerd::api::FormData>();
      formData->parse(js, kBody, "multipart/form-data; boundary=\"+\""_kj, true);
      KJ_ASSERT(formData->has(kj::str("field0")));
      KJ_ASSERT(formData->has(kj::str("field1")));

      auto after = js.v8Isolate->GetExternalMemory();
      KJ_ASSERT(after - before == 52);
    }

    {
      auto formData = js.alloc<workerd::api::FormData>();
      formData->parse(js, kUrlData, "application/x-www-form-urlencoded"_kj, true);
      KJ_ASSERT(formData->has(kj::str("field0")));
      KJ_ASSERT(formData->has(kj::str("field1")));

      auto after = js.v8Isolate->GetExternalMemory();
      KJ_ASSERT(after - before == 52);
    }

    return true;
  }

  JSG_RESOURCE_TYPE(HeadersContext) {
    JSG_METHOD(test);
  }
};

JSG_DECLARE_ISOLATE_TYPE(HeadersIsolate,
    HeadersContext,
    // It's unfortunate but we have to pull in all of these sets of
    // types just for the test to build, even tho they aren't actually
    // used by or relevant to the test.
    EW_FORMDATA_ISOLATE_TYPES,
    workerd::api::Blob,
    workerd::api::Blob::Options,
    workerd::api::File,
    workerd::api::File::Options);

KJ_TEST("FormData memory is accounted for") {
  jsg::test::Evaluator<HeadersContext, HeadersIsolate, CompatibilityFlags::Reader> e(v8System);
  e.expectEval("test()", "boolean", "true");
}

}  // namespace
}  // namespace workerd::api
