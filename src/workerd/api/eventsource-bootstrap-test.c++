// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/eventsource-bootstrap.h>
#include <workerd/jsg/script.h>
#include <workerd/tests/test-fixture.h>

#include <kj/test.h>

namespace workerd::api {
namespace {

using Callback = jsg::Function<v8::Local<v8::Value>(const v8::FunctionCallbackInfo<v8::Value>&)>;

// The helpers as JS functions, taking the same arguments as the bootstrap's utils members.
jsg::JsObject makeHelpers(jsg::Lock& js) {
  auto helpers = js.obj();
  auto context = js.v8Context();
  const auto add = [&](kj::StringPtr name, Callback callback) {
    helpers.set(js, name, jsg::JsValue(js.wrapReturningFunction(context, kj::mv(callback))));
  };

  add("createEventSourceIo"_kj,
      [](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>&) -> v8::Local<v8::Value> {
    return EventSourceBootstrap::createEventSourceIo(js);
  });
  add("isFetcher"_kj,
      [](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& info) -> v8::Local<v8::Value> {
    return js.boolean(EventSourceBootstrap::isFetcher(js, jsg::JsValue(info[0])));
  });
  add("parseUrl"_kj,
      [](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& info) -> v8::Local<v8::Value> {
    return EventSourceBootstrap::parseUrl(js, jsg::JsValue(info[0]));
  });
  add("isEventStreamMimeType"_kj,
      [](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& info) -> v8::Local<v8::Value> {
    return js.boolean(EventSourceBootstrap::isEventStreamMimeType(js, jsg::JsValue(info[0])));
  });
  add("readableStreamState"_kj,
      [](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& info) -> v8::Local<v8::Value> {
    return EventSourceBootstrap::readableStreamState(js, jsg::JsValue(info[0]));
  });
  return helpers;
}

// Runs `body` as the body of a function of `helpers` and `arg`, returning its result.
jsg::JsValue runHelperScript(
    jsg::Lock& js, kj::StringPtr body, kj::Maybe<jsg::JsValue> arg = kj::none) {
  auto code = kj::str("((helpers, arg) => {", body, "})");
  auto fn = KJ_ASSERT_NONNULL(jsg::NonModuleScript::compile(js, code, "helpers-test.js"_kj)
                                  .runAndReturn(js)
                                  .tryCast<jsg::JsFunction>());
  return fn.call(js, js.undefined(), jsg::JsValue(makeHelpers(js)), arg.orDefault(js.undefined()));
}

void expectHelperScript(kj::StringPtr body, kj::StringPtr expected) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto result = runHelperScript(env.js, body).toString(env.js);
    KJ_EXPECT(result == expected, result, expected);
  });
}

KJ_TEST("parseUrl parses absolute URLs without a base, and serializes their origins") {
  expectHelperScript(R"js(
    const show = (input) => {
      const parsed = helpers.parseUrl(input);
      if (parsed === undefined) return 'undefined';
      return [Object.getPrototypeOf(parsed), parsed.href, parsed.origin].join(' ');
    };
    return [
      show('HTTP://Example.org:80/a/../b?q#f'),
      show('https://example.org:8443/'),
      show('data:text/plain,x'),
      show('/relative'),
      show(''),
    ].join('\n');
  )js"_kj,
      " http://example.org/b?q#f http://example.org\n"
      " https://example.org:8443/ https://example.org:8443\n"
      " data:text/plain,x null\n"
      "undefined\n"
      "undefined"_kj);
}

KJ_TEST("isEventStreamMimeType accepts any parameters on text/event-stream, and nothing else") {
  expectHelperScript(R"js(
    return [
      'text/event-stream',
      'TEXT/Event-Stream; charset=utf-8',
      'text/event-stream;',
      'text/plain',
      'text/event-stream-x',
      'bogus',
      '',
    ].map((value) => helpers.isEventStreamMimeType(value)).join();
  )js"_kj,
      "true,true,true,false,false,false,false"_kj);
}

KJ_TEST("readableStreamState tells streams' state, and anything else apart") {
  expectHelperScript(R"js(
    const readable = new Response('x').body;
    const locked = new Response('x').body;
    locked.getReader();
    const disturbed = new Response('x').body;
    const reader = disturbed.getReader();
    reader.cancel();
    reader.releaseLock();
    return [
      helpers.readableStreamState(readable),
      helpers.readableStreamState(locked),
      helpers.readableStreamState(disturbed),
      helpers.readableStreamState({}),
      helpers.readableStreamState(undefined),
      helpers.readableStreamState(new Response('x')),
    ].join();
  )js"_kj,
      "readable,locked,disturbed,,,"_kj);
}

KJ_TEST("isFetcher recognizes only Fetchers") {
  expectHelperScript(R"js(
    return [{}, null, undefined, 'x', new Request('http://x/')].map(
        (value) => helpers.isFetcher(value)).join();
  )js"_kj,
      "false,false,false,false,false"_kj);
}

KJ_TEST("The string helpers reject anything but strings") {
  expectHelperScript(R"js(
    const messages = [];
    for (const fn of [() => helpers.parseUrl(1), () => helpers.isEventStreamMimeType(null)]) {
      try {
        fn();
        messages.push('ok');
      } catch (e) {
        messages.push(e.constructor.name + ': ' + e.message);
      }
    }
    return messages.join('\n');
  )js"_kj,
      "TypeError: parseUrl() expects a string\n"
      "TypeError: isEventStreamMimeType() expects a string"_kj);
}

KJ_TEST("An EventSourceIo is current only within the request that created it") {
  TestFixture fixture;
  kj::Maybe<jsg::JsRef<jsg::JsValue>> io;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto created = runHelperScript(env.js, "return helpers.createEventSourceIo();"_kj);
    auto result = runHelperScript(env.js, R"js(
      arg.requireCurrent();
      return [typeof arg, arg.isCurrent()].join();
    )js"_kj,
        created)
                      .toString(env.js);
    KJ_EXPECT(result == "object,true"_kj, result);
    io = jsg::JsRef(env.js, created);
  });
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& handle = KJ_ASSERT_NONNULL(io);
    auto result = runHelperScript(env.js, R"js(
      const results = [arg.isCurrent()];
      for (const fn of [
        () => arg.requireCurrent(),
        () => arg.logUncaughtException(new Error('x')),
      ]) {
        try {
          fn();
          results.push('ok');
        } catch (e) {
          results.push(/different request/.test(e.message));
        }
      }
      return results.join();
    )js"_kj,
        handle.getHandle(env.js))
                      .toString(env.js);
    KJ_EXPECT(result == "false,true,true"_kj, result);
    io = kj::none;
  });
}

}  // namespace
}  // namespace workerd::api
