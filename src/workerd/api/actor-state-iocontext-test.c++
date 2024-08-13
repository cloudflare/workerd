// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/actor-state.h>
#include <workerd/io/actor-id.h>
#include <workerd/tests/test-fixture.h>

#include <kj/encoding.h>
#include <kj/test.h>

#include <algorithm>

namespace workerd::api {
namespace {

using workerd::TestFixture;

bool contains(kj::StringPtr haystack, kj::StringPtr needle) {
  return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end()) !=
      haystack.end();
}

class MockActorId: public ActorIdFactory::ActorId {
public:
  MockActorId(kj::String id): id(kj::mv(id)) {}
  kj::String toString() const override {
    return kj::str("MockActorId<", id, ">");
  }

  kj::Maybe<kj::StringPtr> getName() const override {
    return kj::none;
  }

  bool equals(const ActorId& other) const override {
    return false;
  }

  kj::Own<ActorId> clone() const override {
    return kj::heap<MockActorId>(kj::heapString(id));
  }

  virtual ~MockActorId() {};

private:
  kj::String id;
};

void runBadDeserialization(jsg::Lock& lock, kj::StringPtr expectedId) {
  // FF = kVersion token, 0E = version 15, 06 = an unknown tag value
  kj::StringPtr invalidV8Hex = "FF0E06"_kj;
  auto invalidV8Value = kj::decodeHex(invalidV8Hex.asArray());
  try {
    deserializeV8Value(lock, "some-key"_kj, invalidV8Value);
    KJ_FAIL_ASSERT("deserializeV8Value should have failed.");
  } catch (kj::Exception& ex) {
    if (ex.getDescription().startsWith("actor storage deserialization failed")) {
      KJ_ASSERT(contains(ex.getDescription(), expectedId));
    } else {
      throw;
    }
  }
}

void runBadDeserializationInIoContext(TestFixture& fixture, kj::StringPtr expectedId) {
  fixture.runInIoContext(
      [expectedId](const workerd::TestFixture::Environment& env) -> kj::Promise<void> {
    runBadDeserialization(env.lock, expectedId);
    return kj::READY_NOW;
  });
}

// TODO(maybe) It would be nice to have a test that tests the case when there's no IoContext,
// but that's a royal pain to set up in this test file we'd basically only test that we don't
// crash, which the actor-state-test.c++ does for us.

KJ_TEST("no actor specified") {
  TestFixture fixture;
  runBadDeserializationInIoContext(fixture, "actorId = ;"_kj);
}

KJ_TEST("actor specified with string id") {
  Worker::Actor::Id id = kj::str("testActorId");
  TestFixture fixture(TestFixture::SetupParams{.actorId = kj::mv(id)});
  runBadDeserializationInIoContext(fixture, "actorId = testActorId;"_kj);
}

KJ_TEST("actor specified with ActorId object") {
  kj::Own<ActorIdFactory::ActorId> mockActorId = kj::heap<MockActorId>(kj::str("testActorId"));
  Worker::Actor::Id id = kj::mv(mockActorId);
  TestFixture fixture(TestFixture::SetupParams{
    .actorId = kj::mv(id),
  });
  runBadDeserializationInIoContext(fixture, "actorId = MockActorId<testActorId>;"_kj);
}

}  // namespace
}  // namespace workerd::api
