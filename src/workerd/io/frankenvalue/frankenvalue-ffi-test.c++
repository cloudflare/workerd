// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tests the frankenvalue crate in the requests of a real worker: a Rust resource (test_bridge.rs)
// builds Frankenvalues from its arguments, turns them back into JavaScript, clones them and hands
// them to C++ by value, and its errors are compared with those of C++ Frankenvalue::fromJs().

#include <workerd/api/actor.h>
#include <workerd/api/http.h>
#include <workerd/api/worker-rpc.h>
#include <workerd/io/frankenvalue/test-bridge-state.h>
#include <workerd/io/frankenvalue/test_bridge.rs.h>
#include <workerd/jsg/script.h>
#include <workerd/rust/jsg/ffi-inl.h>
#include <workerd/tests/test-fixture.h>
#include <workerd/util/strong-bool.h>

#include <kj/test.h>

namespace workerd::frankenvalue::test {
namespace {

WD_STRONG_BOOL(Transferable);

// A channel that can be passed to another worker, or not. It is never sent requests.
class TestChannel final: public IoChannelFactory::SubrequestChannel {
 public:
  explicit TestChannel(Transferable transferable): transferable(transferable) {}

  kj::Own<WorkerInterface> startRequest(IoChannelFactory::SubrequestMetadata metadata) override {
    KJ_UNIMPLEMENTED("not used by these tests");
  }
  void requireAllowsTransfer() override {
    JSG_REQUIRE(transferable == Transferable::YES, DOMDataCloneError,
        "This test channel is not transferrable.");
  }
  kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> getTokenMaybeSync(
      IoChannelFactory::ChannelTokenUsage usage) override {
    KJ_UNIMPLEMENTED("not used by these tests");
  }

 private:
  Transferable transferable;
};

// A Durable Object class that can be passed to another worker. Nothing starts it.
class TestActorClassChannel final: public IoChannelFactory::ActorClassChannel {
 public:
  void requireAllowsTransfer() override {}
  kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> getTokenMaybeSync(
      IoChannelFactory::ChannelTokenUsage usage) override {
    KJ_UNIMPLEMENTED("not used by these tests");
  }
};

// A persistent RPC target that can be passed to another worker. Nothing restores it.
class TestRpcChannel final: public IoChannelFactory::RpcChannel {
 public:
  Session restore() override {
    KJ_UNIMPLEMENTED("not used by these tests");
  }
  void requireAllowsTransfer() override {}
  kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> getTokenMaybeSync(
      IoChannelFactory::ChannelTokenUsage usage) override {
    KJ_UNIMPLEMENTED("not used by these tests");
  }
};

template <typename T>
jsg::JsValue wrap(jsg::Lock& js, jsg::Ref<T> value) {
  auto& handler = KJ_ASSERT_NONNULL(js.tryGetTypeHandler<jsg::Ref<T>>());
  return jsg::JsValue(handler.wrap(js, kj::mv(value)));
}

template <typename T>
jsg::Ref<T> unwrap(jsg::Lock& js, jsg::JsValue value) {
  auto& handler = KJ_ASSERT_NONNULL(js.tryGetTypeHandler<jsg::Ref<T>>());
  return KJ_ASSERT_NONNULL(handler.tryUnwrap(js, value), value.toString(js));
}

jsg::JsValue runScript(const TestFixture::Environment& env, kj::StringPtr source) {
  return jsg::NonModuleScript::compile(env.js, source, "frankenvalue-ffi-test.js"_kj)
      .runAndReturn(env.js);
}

// `source`'s value, as a string.
kj::String eval(const TestFixture::Environment& env, kj::StringPtr source) {
  return runScript(env, source).toString(env.js);
}

void setGlobal(const TestFixture::Environment& env, kj::StringPtr name, jsg::JsValue value) {
  jsg::JsObject(env.js.v8Context()->Global()).set(env.js, name, value);
}

// Sets the globals the scripts use: `probe` (a Rust `Probe`), `service`, a Fetcher on
// `transferable`, and `local`, a Fetcher on a channel that can't be transferred.
void setGlobals(
    const TestFixture::Environment& env, kj::Rc<IoChannelFactory::SubrequestChannel> transferable) {
  auto& js = env.js;
  auto probe = ::workerd::rust::jsg::maybe_local_from_ffi<v8::Value>(probe_new(env.isolate))
                   .ToLocalChecked();
  setGlobal(env, "probe"_kj, jsg::JsValue(probe));
  setGlobal(env, "service"_kj,
      wrap(js, js.alloc<api::Fetcher>(env.context.addObject(kj::mv(transferable)))));
  kj::Rc<IoChannelFactory::SubrequestChannel> local = kj::rc<TestChannel>(Transferable::NO);
  setGlobal(
      env, "local"_kj, wrap(js, js.alloc<api::Fetcher>(env.context.addObject(kj::mv(local)))));
}

void setGlobals(const TestFixture::Environment& env) {
  setGlobals(env, kj::rc<TestChannel>(Transferable::YES));
}

// What `call` throws, as `${name}: ${message}`, or "no exception".
kj::String thrownBy(const TestFixture::Environment& env, kj::StringPtr call) {
  return eval(
      env, kj::str("try { ", call, "; 'no exception' } catch (e) { `${e.name}: ${e.message}` }"));
}

// What C++ Frankenvalue::fromJs() throws for the value of `source`, as thrownBy() describes it.
kj::String thrownByCpp(const TestFixture::Environment& env, kj::StringPtr source) {
  auto& js = env.js;
  auto value = runScript(env, source);
  return js.tryCatch([&]() {
    Frankenvalue::fromJs(js, value);
    return kj::str("no exception");
  }, [&](jsg::Value error) {
    setGlobal(env, "cppError"_kj, jsg::JsValue(error.getHandle(js)));
    return eval(env, "`${cppError.name}: ${cppError.message}`");
  });
}

KJ_TEST("plain data survives a round trip through a Rust Frankenvalue") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    setGlobals(env);
    runScript(env, R"JS(
      var original = {
        number: 1, string: "s", nested: {list: [1, "two", null]}, date: new Date(5),
        map: new Map([["k", 1n]]), bytes: new Uint8Array([1, 2, 3]),
      };
      var copy = probe.roundTrip(original);
    )JS"_kj);
    KJ_EXPECT(eval(env, "copy !== original && copy.nested !== original.nested") == "true");
    KJ_EXPECT(eval(env, "JSON.stringify(copy)") == eval(env, "JSON.stringify(original)"));
    KJ_EXPECT(eval(env, "copy.date instanceof Date && copy.date.getTime()") == "5");
    KJ_EXPECT(eval(env, "copy.map.get('k') === 1n") == "true");
    KJ_EXPECT(eval(env, "copy.bytes instanceof Uint8Array && copy.bytes.join()") == "1,2,3");
    KJ_EXPECT(eval(env, "probe.roundTrip('primitive')") == "primitive");
    KJ_EXPECT(eval(env, "probe.roundTrip(undefined) === undefined") == "true");
  });
}

KJ_TEST("the empty Frankenvalue is an empty object; one from JavaScript is never empty") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    setGlobals(env);
    KJ_EXPECT(eval(env, "JSON.stringify(probe.empty())") == "{}");
    // As in C++: fromJs() always makes a serialized value, even of `{}`.
    KJ_EXPECT(eval(env, "probe.isEmpty({})") == "false");
  });
}

KJ_TEST("estimateSize() from Rust is C++'s") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    setGlobals(env);
    for (auto source: {"({})"_kj, "({a: 'x'.repeat(1000)})"_kj, "[1, 2, 3]"_kj, "'s'"_kj}) {
      auto cppSize = Frankenvalue::fromJs(env.js, runScript(env, source)).estimateSize();
      auto rustSize = eval(env, kj::str("probe.estimateSize(", source, ")"));
      KJ_EXPECT(rustSize == kj::str(cppSize), source, rustSize, cppSize);
    }
    KJ_EXPECT(eval(env, "probe.estimateSize({a: 'x'.repeat(1000)}) > 1000") == "true");
  });
}

KJ_TEST("a clone of a Rust Frankenvalue outlives the original") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    setGlobals(env);
    KJ_EXPECT(eval(env, "JSON.stringify(probe.cloneRoundTrip({a: [1, {b: 'c'}]}))") ==
        R"({"a":[1,{"b":"c"}]})");
  });
}

KJ_TEST("a Fetcher becomes a capability, which comes back as a Fetcher on the same channel") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    kj::Rc<IoChannelFactory::SubrequestChannel> channel = kj::rc<TestChannel>(Transferable::YES);
    setGlobals(env, channel.addRef());
    runScript(env,
        "var copy = probe.roundTrip({service, n: 1}); var cloned = probe.cloneRoundTrip(service);"_kj);
    KJ_EXPECT(eval(env, "copy.service !== service && copy.n") == "1");
    KJ_EXPECT(eval(env, "Object.getPrototypeOf(copy.service) === Object.getPrototypeOf(service)") ==
        "true");
    auto copy = unwrap<api::Fetcher>(js, runScript(env, "copy.service"));
    KJ_EXPECT(copy->getSubrequestChannel(env.context).get() == channel.get());
    auto cloned = unwrap<api::Fetcher>(js, runScript(env, "cloned"));
    KJ_EXPECT(cloned->getSubrequestChannel(env.context).get() == channel.get());
  });
}

KJ_TEST("a DurableObjectClass becomes a capability, which comes back on the same channel") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    setGlobals(env);
    kj::Rc<IoChannelFactory::ActorClassChannel> channel = kj::rc<TestActorClassChannel>();
    setGlobal(env, "cls"_kj,
        wrap(js, js.alloc<api::DurableObjectClass>(env.context.addObject(channel.addRef()))));
    runScript(env, "var copy = probe.roundTrip({cls}); var cloned = probe.cloneRoundTrip(cls);"_kj);
    KJ_EXPECT(eval(env, "copy.cls !== cls") == "true");
    KJ_EXPECT(
        eval(env, "Object.getPrototypeOf(copy.cls) === Object.getPrototypeOf(cls)") == "true");
    auto copy = unwrap<api::DurableObjectClass>(js, runScript(env, "copy.cls"));
    KJ_EXPECT(copy->getChannel(env.context).get() == channel.get());
    auto cloned = unwrap<api::DurableObjectClass>(js, runScript(env, "cloned"));
    KJ_EXPECT(cloned->getChannel(env.context).get() == channel.get());
  });
}

KJ_TEST("a persistent RPC stub becomes a capability, which comes back on the same channel") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    setGlobals(env);
    kj::Rc<IoChannelFactory::RpcChannel> channel = kj::rc<TestRpcChannel>();
    setGlobal(env, "stub"_kj,
        wrap(js, js.alloc<api::JsRpcStub>(env.context.addObject(channel.addRef()), kj::none)));
    runScript(env, "var copy = probe.roundTrip({stub});"_kj);
    KJ_EXPECT(eval(env, "copy.stub !== stub") == "true");
    auto copy = unwrap<api::JsRpcStub>(js, runScript(env, "copy.stub"));
    auto copyChannel = KJ_ASSERT_NONNULL(copy->getRpcChannel(env.context));
    KJ_EXPECT(copyChannel.get() == channel.get());
  });
}

KJ_TEST("a Rust Frankenvalue moves into C++ that takes one by value") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    kj::Rc<IoChannelFactory::SubrequestChannel> channel = kj::rc<TestChannel>(Transferable::YES);
    setGlobals(env, channel.addRef());
    auto source = "({service, text: 'hello'})"_kj;
    auto cppSize = Frankenvalue::fromJs(js, runScript(env, source)).estimateSize();
    runScript(env, kj::str("probe.consume(", source, ")"));
    KJ_DEFER(consumed = kj::none);

    auto& value = KJ_ASSERT_NONNULL(consumed);
    KJ_EXPECT(value.estimateSize() == cppSize);
    KJ_ASSERT(value.getCapTable().size() == 1);
    KJ_EXPECT(&kj::downcast<IoChannelFactory::SubrequestChannel>(*value.getCapTable()[0]) ==
        channel.get());
    setGlobal(env, "consumed"_kj, value.toJs(js));
    KJ_EXPECT(eval(env, "consumed.text") == "hello");
    auto fetcher = unwrap<api::Fetcher>(js, runScript(env, "consumed.service"));
    KJ_EXPECT(fetcher->getSubrequestChannel(env.context).get() == channel.get());
  });
}

KJ_TEST("a value that can't be serialized throws what C++ throws") {
  consumed = kj::none;
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    setGlobals(env);
    for (auto source: {"(function f() {})"_kj, "({f() {}})"_kj, "Symbol('s')"_kj,
           "new (class C { x = 1; })()"_kj, "new Proxy({}, {})"_kj, "({local})"_kj}) {
      auto expected = thrownByCpp(env, source);
      KJ_EXPECT(expected != "no exception", source);
      for (auto method: {"roundTrip"_kj, "estimateSize"_kj, "consume"_kj}) {
        auto actual = thrownBy(env, kj::str("probe.", method, "(", source, ")"));
        KJ_EXPECT(actual == expected, source, method, actual, expected);
      }
    }
    KJ_EXPECT(
        thrownByCpp(env, "({local})") == "DataCloneError: This test channel is not transferrable.");
    KJ_EXPECT(consumed == kj::none);
  });
}

KJ_TEST("an exception that JavaScript throws during serialization propagates unchanged") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    setGlobals(env);
    KJ_EXPECT(eval(env, R"JS(
      (() => {
        const thrown = new RangeError("from a getter");
        try {
          probe.roundTrip({ get a() { throw thrown; } });
          return "no exception";
        } catch (e) {
          return e === thrown;
        }
      })()
    )JS"_kj) == "true");
    // Likewise from a getter deep in the value.
    KJ_EXPECT(thrownBy(env, "probe.consume({a: [{ get b() { throw new TypeError('deep'); } }]})") ==
        "TypeError: deep");
  });
}

KJ_TEST("toJs() of a capability outside an IoContext throws what C++ throws") {
  consumed = kj::none;
  KJ_DEFER(consumed = kj::none);
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    setGlobals(env);
    runScript(env, "probe.consume({service})"_kj);
  });
  auto& value = KJ_ASSERT_NONNULL(consumed);
  KJ_ASSERT(value.getCapTable().size() == 1);

  fixture.enterWorkerLock([&](Worker::Lock& lock) {
    JSG_WITHIN_CONTEXT_SCOPE(lock, lock.getContext(), [&](jsg::Lock& js) {
      KJ_ASSERT(IoContext::tryCurrent() == kj::none);
      // `${name}: ${message}` of what `toJs` throws, up to an internal error's reference, which
      // differs between two errors.
      auto describe = [&](auto&& toJs) -> kj::String {
        return js.tryCatch([&]() -> kj::String {
          toJs();
          return kj::str("no exception");
        }, [&](jsg::Value error) {
          jsg::JsObject(js.v8Context()->Global())
              .set(js, "toJsError"_kj, jsg::JsValue(error.getHandle(js)));
          return jsg::NonModuleScript::compile(js,
              "`${toJsError.name}: ${toJsError.message}`.replace(/; reference = .*/, '')"_kj,
              "frankenvalue-ffi-test.js"_kj)
              .runAndReturn(js)
              .toString(js);
        });
      };
      auto expected = describe([&]() { value.toJs(js); });
      auto actual = describe([&]() {
        v8::Local<v8::Value> result;
        if (!::workerd::rust::jsg::maybe_local_from_ffi<v8::Value>(
                to_js_from_rust(js.v8Isolate, value))
                 .ToLocal(&result)) {
          throw jsg::JsExceptionThrown();
        }
      });
      KJ_EXPECT(expected != "no exception", expected);
      KJ_EXPECT(actual == expected, actual, expected);
    });
  });
}

}  // namespace
}  // namespace workerd::frankenvalue::test
