// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// What native code sees of a Blob in a context with the per-isolate bootstrap: the bytes it
// serializes to, what reading them creates, and the Blobs and Files api/blob-bridge.h makes and
// reads. The @all-autogates variant runs them against the TypeScript implementation (TS_BLOB).
// src/tests/blob/ covers what JavaScript sees.

#include <workerd/api/blob.h>
#include <workerd/jsg/script.h>
#include <workerd/jsg/ser.h>
#include <workerd/tests/test-fixture.h>
#include <workerd/util/autogate.h>

#include <capnp/message.h>
#include <kj/test.h>

namespace workerd::api {
namespace {

// Under the legacy layout, which the default flags select, a Blob's attributes are its own
// properties; under this one, the prototype's accessors.
enum class Layout { LEGACY, ON_PROTOTYPE };

void runWithBootstrap(kj::Function<void(jsg::Lock& js)> body, Layout layout = Layout::LEGACY) {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setJsgPropertyOnPrototypeTemplate(layout == Layout::ON_PROTOTYPE);
  TestFixture fixture({.featureFlags = flags.asReader()});
  fixture.runInIoContext([&](const TestFixture::Environment& env) { body(env.js); });
}

jsg::JsValue eval(jsg::Lock& js, kj::StringPtr code) {
  return jsg::NonModuleScript::compile(js, code, "blob-test.js"_kj).runAndReturn(js);
}

kj::String call(jsg::Lock& js, kj::StringPtr fnCode, jsg::JsValue value) {
  auto fn = KJ_ASSERT_NONNULL(eval(js, fnCode).tryCast<jsg::JsFunction>());
  return fn.call(js, js.undefined(), value).toString(js);
}

// The properties a test checks of a Blob, joined with commas: whether it is an instance of the
// global Blob (and File), its own keys, which it has under the legacy layout the fixture's flags
// select, as a constructed one does, and its size and type.
kj::String describe(jsg::Lock& js, jsg::JsValue value) {
  return call(js, R"js((b) => [
    Object.getPrototypeOf(b) === Blob.prototype,
    b instanceof File,
    Object.keys(b).join('|') === Object.keys(new Blob()).join('|'),
    b.size, b.type,
  ].join())js"_kj,
      value);
}

kj::Array<kj::byte> serialize(jsg::Lock& js, jsg::JsValue value) {
  jsg::Serializer serializer(js);
  serializer.write(js, value);
  auto released = serializer.release();
  return kj::mv(released.data);
}

// A Blob of type "a/b" holding "xy", after the V8 header (version 16): the host object tag, the
// serialization tag (15), the length-delimited type and the length-delimited bytes.
constexpr kj::byte BLOB_BYTES[] = {0xff, 0x10, 0x5c, 15, 3, 'a', '/', 'b', 2, 'x', 'y'};

KJ_TEST("The TS_BLOB autogate selects the implementation") {
  runWithBootstrap([](jsg::Lock& js) {
    KJ_EXPECT(
        JsBlob::isTsImplementation(js) == util::Autogate::isEnabled(util::AutogateKey::TS_BLOB));
  });
}

KJ_TEST("A Blob serializes as tag 15") {
  runWithBootstrap([](jsg::Lock& js) {
    auto bytes = serialize(js, eval(js, "new Blob(['xy'], { type: 'A/B' })"_kj));
    KJ_EXPECT(bytes == kj::arrayPtr(BLOB_BYTES), bytes);
    // A slice writes only its bytes.
    bytes = serialize(js, eval(js, "new Blob(['axyz']).slice(1, 3, 'a/b')"_kj));
    KJ_EXPECT(bytes == kj::arrayPtr(BLOB_BYTES), bytes);
  });
}

KJ_TEST("Reading tag 15 creates a Blob of the global Blob's implementation") {
  runWithBootstrap([](jsg::Lock& js) {
    jsg::Deserializer deserializer(js, BLOB_BYTES);
    auto value = deserializer.readValue(js);
    KJ_EXPECT(describe(js, value) == "true,false,true,2,a/b"_kj, describe(js, value));
    auto blob = KJ_ASSERT_NONNULL(JsBlob::tryUnwrap(js, value));
    KJ_EXPECT(blob.getData(js) == "xy"_kj.asBytes());
    // It serializes back to the same bytes.
    KJ_EXPECT(serialize(js, value) == kj::arrayPtr(BLOB_BYTES));
  });
}

KJ_TEST("A File does not serialize") {
  runWithBootstrap([](jsg::Lock& js) {
    // Through structuredClone(), which runs the same serializer, so as to see the error.
    auto result = eval(js, R"js(
      try { structuredClone(new File(['xy'], 'f')); 'cloned' } catch (e) { e.name }
    )js"_kj);
    KJ_EXPECT(result.toString(js) == "DataCloneError"_kj, result.toString(js));
  });
}

KJ_TEST("JsBlob and JsFile make instances of the global classes") {
  runWithBootstrap([](jsg::Lock& js) {
    auto bytes = jsg::JsUint8Array::create(js, "xy"_kj.asBytes());
    auto blob = JsBlob::create(js, jsg::JsBufferSource(bytes), kj::str("a/b"));
    auto blobValue = kj::mv(blob).toJsValue(js);
    KJ_EXPECT(describe(js, blobValue) == "true,false,true,2,a/b"_kj, describe(js, blobValue));

    auto file = JsFile::create(js, jsg::JsBufferSource(bytes), kj::str("n"), kj::str("t/x"), 5);
    KJ_EXPECT(file.getName(js) == "n"_kj);
    KJ_EXPECT(file.getLastModified(js) == 5);
    KJ_EXPECT(file.getType(js) == "t/x"_kj);
    KJ_EXPECT(file.getData(js) == "xy"_kj.asBytes());
  });
}

// Every own property of `value`, keys and descriptors in order, as JSON, with `lastModified`'s
// value left out.
kj::String ownProperties(jsg::Lock& js, jsg::JsValue value) {
  return call(js, R"js((o) => JSON.stringify(Reflect.ownKeys(o).map((k) => {
    const d = Object.getOwnPropertyDescriptor(o, k);
    if (k === 'lastModified') delete d.value;
    return [String(k), d];
  })))js"_kj,
      value);
}

// Whether structuredClone() of `value` throws DataCloneError, or else the clone's text and type.
kj::String cloneResult(jsg::Lock& js, jsg::JsValue value) {
  return call(js, R"js((b) => {
    try { const c = structuredClone(b); return [c.size, c.type].join(); }
    catch (e) { return e.name; }
  })js"_kj,
      value);
}

KJ_TEST("JsBlob and JsFile make instances as the constructors do") {
  for (auto layout: {Layout::LEGACY, Layout::ON_PROTOTYPE}) {
    runWithBootstrap([](jsg::Lock& js) {
      auto bytes = jsg::JsUint8Array::create(js, "xy"_kj.asBytes());
      auto made = JsBlob::create(js, jsg::JsBufferSource(bytes), kj::str("a/b")).toJsValue(js);
      auto constructed = eval(js, "new Blob(['xy'], { type: 'a/b' })"_kj);
      KJ_EXPECT(ownProperties(js, made) == ownProperties(js, constructed), ownProperties(js, made),
          ownProperties(js, constructed));
      KJ_EXPECT(serialize(js, made) == serialize(js, constructed));

      auto madeFile =
          JsFile::create(js, jsg::JsBufferSource(bytes), kj::str("n"), kj::str("a/b"), 5)
              .toJsValue(js);
      auto constructedFile = eval(js, "new File(['xy'], 'n', { type: 'a/b' })"_kj);
      KJ_EXPECT(ownProperties(js, madeFile) == ownProperties(js, constructedFile),
          ownProperties(js, madeFile), ownProperties(js, constructedFile));
      KJ_EXPECT(cloneResult(js, madeFile) == "DataCloneError"_kj, cloneResult(js, madeFile));
      KJ_EXPECT(cloneResult(js, constructedFile) == "DataCloneError"_kj);
    }, layout);
  }
}

KJ_TEST("A type that is not UTF-8") {
  runWithBootstrap([](jsg::Lock& js) {
    // The C++ Blob keeps the bytes; the TypeScript one keeps a string, which the bytes, decoded,
    // become (src/tests/blob/AGENTS.md, ledger #6). JavaScript sees the same string of either.
    auto blob = JsBlob::createEmpty(js,
        kj::str("a\xff"
                "b"));
    auto expected = JsBlob::isTsImplementation(js) ? "a\xef\xbf\xbd"
                                                     "b"_kj
                                                   : "a\xff"
                                                     "b"_kj;
    KJ_EXPECT(blob.getType(js) == expected, blob.getType(js));
    KJ_EXPECT(
        call(js, "(b) => b.type === 'a\\ufffdb'"_kj, kj::mv(blob).toJsValue(js)) == "true"_kj);
  });
}

KJ_TEST("JsBlob and JsFile read instances of the global classes") {
  runWithBootstrap([](jsg::Lock& js) {
    auto blob = KJ_ASSERT_NONNULL(
        JsBlob::tryUnwrap(js, eval(js, "new Blob(['abc'], { type: 'T/X' }).slice(1)"_kj)));
    KJ_EXPECT(blob.getData(js) == "bc"_kj.asBytes());
    KJ_EXPECT(blob.getType(js) == ""_kj);
    KJ_EXPECT(blob.tryGetFile(js) == kj::none);

    auto value = eval(js, "new File(['abc'], 'n', { type: 'T/X', lastModified: 7 })"_kj);
    auto file = KJ_ASSERT_NONNULL(KJ_ASSERT_NONNULL(JsBlob::tryUnwrap(js, value)).tryGetFile(js));
    KJ_EXPECT(file.getName(js) == "n"_kj);
    KJ_EXPECT(file.getLastModified(js) == 7);
    KJ_EXPECT(file.getType(js) == "t/x"_kj);

    // A File made from a Blob shares its bytes.
    auto named = JsFile::fromBlob(js, blob, kj::str("m"));
    KJ_EXPECT(named.getName(js) == "m"_kj);
    KJ_EXPECT(named.getData(js).begin() == blob.getData(js).begin());

    for (auto code: {"({})"_kj, "'x'"_kj, "new Uint8Array(1)"_kj,
           "Object.create(Blob.prototype)"_kj, "new Proxy(new Blob(), {})"_kj}) {
      KJ_EXPECT(JsBlob::tryUnwrap(js, eval(js, code)) == kj::none, code);
    }
    KJ_EXPECT(KJ_ASSERT_NONNULL(JsBlob::tryUnwrap(js, eval(js, "new Blob()"_kj))).tryGetFile(js) ==
        kj::none);
  });
}

}  // namespace
}  // namespace workerd::api
