// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/actor-state.h>
#include <workerd/api/util.h>
#include <workerd/jsg/jsg-test.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/ser.h>
#include <workerd/jsg/setup.h>

#include <capnp/message.h>
#include <capnp/rpc-twoparty.h>
#include <capnp/rpc.h>
#include <kj/encoding.h>
#include <kj/test.h>

#include <fstream>
#include <iostream>

namespace workerd::api {
namespace {

jsg::V8System v8System;

struct ActorStateContext: public jsg::Object, public jsg::ContextGlobal {
  JSG_RESOURCE_TYPE(ActorStateContext) {}
};
JSG_DECLARE_ISOLATE_TYPE(ActorStateIsolate, ActorStateContext);

KJ_TEST("v8 serialization version tag hasn't changed") {
  jsg::test::Evaluator<ActorStateContext, ActorStateIsolate> e(v8System);
  e.getIsolate().runInLockScope([&](ActorStateIsolate::Lock& isolateLock) {
    JSG_WITHIN_CONTEXT_SCOPE(isolateLock,
        isolateLock.newContext<ActorStateContext>().getHandle(isolateLock), [&](jsg::Lock& js) {
      auto buf = serializeV8Value(isolateLock, isolateLock.boolean(true));

      // Confirm that a version header is appropriately written and that it contains the expected
      // current version. When the version increases, we need to write a v8 patch that allows it
      // to continue writing data at the old version so that we can do a rolling upgrade without
      // any bugs caused by old processes failing to read data written by new ones.
      KJ_EXPECT(buf[0] == 0xFF);
      KJ_EXPECT(buf[1] == 0x0F);  // v8 serializer version

      // And this just confirms that the deserializer agrees on the version.
      v8::ValueDeserializer deserializer(isolateLock.v8Isolate, buf.begin(), buf.size());
      auto maybeHeader = deserializer.ReadHeader(isolateLock.v8Context());
      KJ_EXPECT(jsg::check(maybeHeader));
      KJ_EXPECT(deserializer.GetWireFormatVersion() == 15);

      // Just for kicks, make sure it deserializes properly too.
      KJ_EXPECT(deserializeV8Value(isolateLock, "some-key"_kj, buf).isTrue());
    });
  });
}

KJ_TEST("we support deserializing up to v15") {
  jsg::test::Evaluator<ActorStateContext, ActorStateIsolate> e(v8System);
  e.getIsolate().runInLockScope([&](ActorStateIsolate::Lock& isolateLock) {
    JSG_WITHIN_CONTEXT_SCOPE(isolateLock,
        isolateLock.newContext<ActorStateContext>().getHandle(isolateLock), [&](jsg::Lock& js) {
      kj::Vector<kj::StringPtr> testCases;
      testCases.add("54");
      testCases.add("FF0D54");
      testCases.add("FF0E54");
      testCases.add("FF0F54");

      for (const auto& hexStr: testCases) {
        auto dataIn = kj::decodeHex(hexStr.asArray());
        KJ_EXPECT(deserializeV8Value(isolateLock, "some-key"_kj, dataIn).isTrue());
      }
    });
  });
}

// This is hacky, but we want to compare the old deserialization logic that's been in prod from when
// actors went live through March 2022 to the new version of the deserialization logic and make sure
// it works the same.
// TODO(soon): Remove this. Ideally we can just fix the test below that attempts to read serialized
// data and round-trip it back to storage to deal with the problem that it likes to read in "sparse"
// JS arrays and write them back out as "dense" JS arrays, which breaks the equality check after
// round-tripping a value.
jsg::JsValue oldDeserializeV8Value(jsg::Lock& js, kj::ArrayPtr<const kj::byte> buf) {
  jsg::Deserializer des(js, buf);
  return des.readValue(js);
}

KJ_TEST("wire format version does not change deserialization behavior on real data") {
  // This test checks for the presence of a specially named file in the current working directory
  // that contains lines of hex-encoded v8-serialized data. It processes one line at time,
  // hex-decoding it and then testing deserializing/re-serializing it.

  std::ifstream file;
  file.open("serialization-test-data.txt");
  if (!file) {
    KJ_LOG(WARNING, "skipping serialization test due to missing data file");
    return;
  }

  jsg::test::Evaluator<ActorStateContext, ActorStateIsolate> e(v8System);
  e.getIsolate().runInLockScope([&](ActorStateIsolate::Lock& isolateLock) {
    JSG_WITHIN_CONTEXT_SCOPE(isolateLock,
        isolateLock.newContext<ActorStateContext>().getHandle(isolateLock), [&](jsg::Lock& js) {
      // Read in data line by line and verify that it round trips (serializes and
      // then deserializes) back to the exact same data as the input.
      std::string hexStr;
      const auto key = "some-key"_kj;
      while (std::getline(file, hexStr)) {
        auto dataIn = kj::decodeHex(kj::ArrayPtr(hexStr.c_str(), hexStr.size()));
        KJ_EXPECT(!dataIn.hadErrors, hexStr);

        auto oldVal = oldDeserializeV8Value(isolateLock, dataIn);
        auto oldOutput = serializeV8Value(isolateLock, oldVal);

        auto newVal = deserializeV8Value(isolateLock, key, dataIn);
        auto newOutput = serializeV8Value(isolateLock, newVal);
        KJ_EXPECT(oldOutput == newOutput, hexStr);
      }
    });
  });
}

}  // namespace
}  // namespace workerd::api
