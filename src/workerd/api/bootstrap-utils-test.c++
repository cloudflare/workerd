// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/bootstrap-utils.h>
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

  add("isFetcher"_kj,
      [](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& info) -> v8::Local<v8::Value> {
    return js.boolean(BootstrapUtils::isFetcher(js, jsg::JsValue(info[0])));
  });
  add("parseUrl"_kj,
      [](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& info) -> v8::Local<v8::Value> {
    return BootstrapUtils::parseUrl(js, jsg::JsValue(info[0]));
  });
  add("mimeTypeParse"_kj,
      [](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& info) -> v8::Local<v8::Value> {
    return BootstrapUtils::mimeTypeParse(js, jsg::JsValue(info[0]));
  });
  add("mimeTypeSerialize"_kj,
      [](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& info) -> v8::Local<v8::Value> {
    return BootstrapUtils::mimeTypeSerialize(
        js, jsg::JsValue(info[0]), jsg::JsValue(info[1]), jsg::JsValue(info[2]));
  });
  add("readableStreamState"_kj,
      [](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& info) -> v8::Local<v8::Value> {
    return BootstrapUtils::readableStreamState(js, jsg::JsValue(info[0]));
  });
  return helpers;
}

// Runs `body` as the body of a function of `helpers`, returning its result as a string.
void expectHelperScript(kj::StringPtr body, kj::StringPtr expected) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto code = kj::str("((helpers) => {", body, "})");
    auto fn = KJ_ASSERT_NONNULL(jsg::NonModuleScript::compile(js, code, "helpers-test.js"_kj)
                                    .runAndReturn(js)
                                    .tryCast<jsg::JsFunction>());
    auto result = fn.call(js, js.undefined(), jsg::JsValue(makeHelpers(js))).toString(js);
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

KJ_TEST("mimeTypeParse lists the lowercased type, subtype and parameter names, and the values") {
  expectHelperScript(R"js(
    return [
      'text/event-stream',
      'TEXT/Event-Stream; Charset=UTF-8',
      'text/plain; a=1; A=2; b="x\\"y"',
      'text/event-stream;',
      'bogus',
      '',
    ].map((value) => JSON.stringify(helpers.mimeTypeParse(value) ?? null)).join('\n');
  )js"_kj,
      "[\"text\",\"event-stream\"]\n"
      "[\"text\",\"event-stream\",\"charset\",\"UTF-8\"]\n"
      "[\"text\",\"plain\",\"a\",\"1\",\"b\",\"x\\\"y\"]\n"
      "[\"text\",\"event-stream\"]\n"
      "null\n"
      "null"_kj);
}

KJ_TEST("mimeTypeSerialize serializes what mimeTypeParse lists, quoting values as needed") {
  expectHelperScript(R"js(
    const roundTrip = (input) => {
      const [type, subtype, ...params] = helpers.mimeTypeParse(input);
      return helpers.mimeTypeSerialize(type, subtype, params);
    };
    return [
      roundTrip('Text/HTML'),
      roundTrip('text/plain;charset=utf-8;format="a b"'),
      roundTrip('multipart/form-data;boundary=x'),
      helpers.mimeTypeSerialize('text', 'plain', ['q', '', 'e', 'a"b']),
    ].join('\n');
  )js"_kj,
      "text/html\n"
      "text/plain;charset=utf-8;format=\"a b\"\n"
      "multipart/form-data; boundary=x\n"
      "text/plain;q=\"\";e=\"a\\\"b\""_kj);
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

KJ_TEST("The helpers reject arguments of the wrong type") {
  expectHelperScript(R"js(
    const messages = [];
    for (const fn of [
      () => helpers.parseUrl(1),
      () => helpers.mimeTypeParse(null),
      () => helpers.mimeTypeSerialize('text', 1, []),
      () => helpers.mimeTypeSerialize('text', 'plain', {}),
      () => helpers.mimeTypeSerialize('text', 'plain', ['a']),
      () => helpers.mimeTypeSerialize('text', 'plain', ['a', 1]),
    ]) {
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
      "TypeError: mimeTypeParse() expects a string\n"
      "TypeError: mimeTypeSerialize() expects a string subtype\n"
      "TypeError: mimeTypeSerialize() expects a parameter array\n"
      "TypeError: mimeTypeSerialize() expects a name and a value for each parameter\n"
      "TypeError: a MIME parameter list must hold only strings"_kj);
}

}  // namespace
}  // namespace workerd::api
