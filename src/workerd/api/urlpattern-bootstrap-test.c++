// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/urlpattern-bootstrap.h>
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
  const auto arg = [](const v8::FunctionCallbackInfo<v8::Value>& info, int i) {
    return jsg::JsValue(info[i]);
  };

  add("compile"_kj,
      [=](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& info) -> v8::Local<v8::Value> {
    return UrlPatternBootstrap::compile(js, arg(info, 0), arg(info, 1), arg(info, 2));
  });
  add("processInit"_kj,
      [=](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& info) -> v8::Local<v8::Value> {
    return UrlPatternBootstrap::processInit(js, arg(info, 0));
  });
  add("compileLegacy"_kj,
      [=](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& info) -> v8::Local<v8::Value> {
    return UrlPatternBootstrap::compileLegacy(js, arg(info, 0), arg(info, 1), arg(info, 2));
  });
  add("processInitLegacy"_kj,
      [=](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& info) -> v8::Local<v8::Value> {
    return UrlPatternBootstrap::processInitLegacy(js, arg(info, 0));
  });
  return helpers;
}

// Runs `body` as the body of a function of `helpers` and `show`, which describes a compiled
// pattern's component, returning its result as a string.
void expectHelperScript(kj::StringPtr body, kj::StringPtr expected) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto code = kj::str(R"js(((helpers) => {
      const component = (compiled, index) => {
        const [pattern, regexp, names, type, exact] = compiled.slice(1 + index * 5, 6 + index * 5);
        const re = regexp === undefined ? 'none' : `/${regexp.source}/${regexp.flags}`;
        return `${pattern} ${re} [${names}] ${type} ${exact}`;
      };
      const error = (f) => {
        try {
          f();
          return 'no error';
        } catch (e) {
          return `${e.name}: ${e.message}`;
        }
      };
      const init = (members) => [
        members.protocol, members.username, members.password, members.hostname, members.port,
        members.pathname, members.search, members.hash, members.baseURL,
      ];
      )js",
        body, "})");
    auto fn = KJ_ASSERT_NONNULL(jsg::NonModuleScript::compile(js, code, "helpers-test.js"_kj)
                                    .runAndReturn(js)
                                    .tryCast<jsg::JsFunction>());
    auto result = fn.call(js, js.undefined(), jsg::JsValue(makeHelpers(js))).toString(js);
    KJ_EXPECT(result == expected, result, expected);
  });
}

KJ_TEST("compile returns each component's pattern, RegExp, names, type and exact value") {
  expectHelperScript(R"js(
    const compiled = helpers.compile('https://*.a.com/b/:id/*', undefined, false);
    return [
      compiled.length,
      compiled[0],
      component(compiled, 0),
      component(compiled, 3),
      component(compiled, 4),
      component(compiled, 5),
      component(compiled, 6),
    ].join('\n');
  )js",
      "41\n"
      "false\n"
      "https none [] 2 https\n"
      "*.a.com /^(.*)\\.a\\.com$/v [0] 0 \n"
      " none [] 3 \n"
      "/b/:id/* /^\\/b(?:\\/([^\\/]+?))(?:\\/(.*))$/v [id,0] 0 \n"
      "* none [0] 1 ");
}

KJ_TEST("compile takes an init, a baseURL and ignoreCase") {
  expectHelperScript(R"js(
    const regexps = helpers.compile(init({ pathname: '/(\\d+)', hash: 'x' }), undefined, true);
    const based = helpers.compile('/a', 'https://b.com/c', false);
    return [
      regexps[0],
      component(regexps, 5),
      component(regexps, 7),
      component(based, 3),
      component(based, 5),
    ].join('\n');
  )js",
      "true\n"
      "/(\\d+) /^(?:\\/(\\d+))$/iv [0] 0 \n"
      "x /^x$/iv [] 0 \n"
      "b.com none [] 2 b.com\n"
      "/a none [] 2 /a");
}

KJ_TEST("compile throws one TypeError for every pattern that does not compile") {
  expectHelperScript(R"js(
    return [
      error(() => helpers.compile('((', undefined, false)),
      error(() => helpers.compile('/a', 'nope', false)),
      error(() => helpers.compile(init({}), 'https://a/', false)),
      error(() => helpers.compile(init({ pathname: '/(a[)' }), undefined, false)),
    ].join('\n');
  )js",
      "TypeError: Failed to construct URLPattern\n"
      "TypeError: Failed to construct URLPattern\n"
      "TypeError: Failed to construct URLPattern\n"
      "TypeError: Failed to construct URLPattern");
}

KJ_TEST("processInit processes an init as URL components") {
  expectHelperScript(R"js(
    const show = (members) => {
      const result = helpers.processInit(init(members));
      return result === undefined ? 'undefined' : JSON.stringify(result);
    };
    return [
      show({}),
      show({ protocol: 'HTTPS', hostname: 'A.com', port: '443', pathname: '/a b', search: '?q' }),
      show({ pathname: 'c', baseURL: 'https://u@b.com/a/b?q#h' }),
      show({ port: 'x' }),
      show({ pathname: '/a', baseURL: 'nope' }),
    ].join('\n');
  )js",
      R"(["","","","","","","",""])"
      "\n"
      R"(["https","","","a.com","","/a%20b","q",""])"
      "\n"
      R"(["https","u","","b.com","","/a/c","",""])"
      "\n"
      "undefined\n"
      "undefined");
}

KJ_TEST("compileLegacy compiles with jsg::UrlPattern and the u flag") {
  expectHelperScript(R"js(
    const compiled = helpers.compileLegacy('/b/:id', 'https://a.com', true);
    const types = [];
    for (let i = 0; i < 8; i++) types.push(compiled[4 + i * 5]);
    return [
      compiled.length,
      compiled[0],
      component(compiled, 0),
      component(compiled, 5),
      types.join(','),
    ].join('\n');
  )js",
      "41\n"
      "false\n"
      "https /^https$/iu [] 0 \n"
      "/b/:id /^\\/b(?:\\/([^\\/]+))$/iu [id] 0 \n"
      "0,0,0,0,0,0,0,0");
}

KJ_TEST("compileLegacy throws the parser's TypeErrors") {
  expectHelperScript(R"js(
    return [
      error(() => helpers.compileLegacy('((', undefined, false)),
      error(() => helpers.compileLegacy('/a', 'nope', false)),
      error(() => helpers.compileLegacy(init({ pathname: '/:a/:a' }), undefined, false)),
      error(() => helpers.compileLegacy(init({ pathname: '/(a[)' }), undefined, false)),
    ].join('\n');
  )js",
      "TypeError: Syntax error in URL Pattern: a relative pattern must have a base URL.\n"
      "TypeError: Invalid base URL.\n"
      "TypeError: Syntax error in URL Pattern: Duplicated part names [a]\n"
      "TypeError: Invalid regular expression syntax.");
}

KJ_TEST("processInitLegacy processes an init, throwing where it does not") {
  expectHelperScript(R"js(
    return [
      JSON.stringify(helpers.processInitLegacy(init({}))),
      JSON.stringify(helpers.processInitLegacy(init({ protocol: 'HTTPS', search: '?q' }))),
      error(() => helpers.processInitLegacy(init({ port: 'x' }))),
    ].join('\n');
  )js",
      R"(["","","","","","","",""])"
      "\n"
      R"(["https","","","","","","q",""])"
      "\n"
      "TypeError: Invalid URL port component");
}

KJ_TEST("the helpers reject arguments of the wrong type") {
  expectHelperScript(R"js(
    return [
      error(() => helpers.compile(1, undefined, false)),
      error(() => helpers.compile([], undefined, false)),
      error(() => helpers.compile(init({ pathname: 1 }), undefined, false)),
      error(() => helpers.compile('/a', 1, false)),
      error(() => helpers.compile('/a', 'https://a/', 1)),
      error(() => helpers.processInit({})),
      error(() => helpers.compileLegacy('/a', 'https://a/', undefined)),
      error(() => helpers.processInitLegacy([])),
    ].join('\n');
  )js",
      "TypeError: init must be an array\n"
      "TypeError: init must have 9 elements\n"
      "TypeError: an init member must be a string\n"
      "TypeError: base must be a string\n"
      "TypeError: ignoreCase must be a boolean\n"
      "TypeError: init must be an array\n"
      "TypeError: ignoreCase must be a boolean\n"
      "TypeError: init must have 9 elements");
}

}  // namespace
}  // namespace workerd::api
