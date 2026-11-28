// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/http.h>
#include <workerd/jsg/memory.h>
#include <workerd/tests/test-fixture.h>

#include <v8-profiler.h>

#include <kj/test.h>

namespace workerd::api {
namespace {

// How many of the Responses in a heap snapshot have a Headers object. Response reports an edge
// named "headers" only once it has created one.
size_t responsesWithHeaders(jsg::Lock& js) {
  static const jsg::HeapSnapshotDeleter deleter;
  kj::Own<const v8::HeapSnapshot> heap(
      js.v8Isolate->GetHeapProfiler()->TakeHeapSnapshot(nullptr, nullptr, true, true), deleter);
  size_t result = 0;
  for (int i = 0; i < heap->GetNodesCount(); i++) {
    auto node = heap->GetNode(i);
    if (kj::str(node->GetName()) != "workerd / Response") continue;
    for (int c = 0; c < node->GetChildrenCount(); c++) {
      if (kj::str(node->GetChild(c)->GetName()) == "headers") {
        ++result;
        break;
      }
    }
  }
  return result;
}

KJ_TEST("Responses that nothing asks for headers create no Headers object") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    kj::Vector<jsg::Ref<Response>> responses;
    // Response.json() gives a Content-Type of its own as well as the one its body implies.
    responses.add(Response::json_(js, js.obj(), kj::none));
    responses.add(Response::constructor(js, kj::Maybe<Body::Initializer>(kj::str("hello")), {}));
    responses.add(Response::constructor(js, {}, {}));
    responses.add(responses[0]->clone(js));

    auto& handler = KJ_REQUIRE_NONNULL(js.tryGetTypeHandler<jsg::Ref<Response>>());
    for (auto& response: responses) {
      KJ_ASSERT(handler.wrap(js, response.addRef())->IsObject());
    }
    KJ_EXPECT(responsesWithHeaders(js) == 0);

    // Asking for them creates them.
    responses[0]->getHeaders(js);
    KJ_EXPECT(responsesWithHeaders(js) == 1);
  });
}

}  // namespace
}  // namespace workerd::api
