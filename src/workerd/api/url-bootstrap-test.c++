// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/url-bootstrap.h>
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

  add("urlParse"_kj,
      [=](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& info) -> v8::Local<v8::Value> {
    return UrlBootstrap::parse(js, arg(info, 0), arg(info, 1), arg(info, 2));
  });
  add("urlCanParse"_kj,
      [=](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& info) -> v8::Local<v8::Value> {
    return js.boolean(UrlBootstrap::canParse(js, arg(info, 0), arg(info, 1)));
  });
  add("urlUpdate"_kj,
      [=](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& info) -> v8::Local<v8::Value> {
    return UrlBootstrap::update(js, arg(info, 0), arg(info, 1), arg(info, 2), arg(info, 3));
  });
  add("urlOrigin"_kj,
      [=](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& info) -> v8::Local<v8::Value> {
    return UrlBootstrap::origin(js, arg(info, 0));
  });
  add("createLegacyUrl"_kj,
      [=](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& info) -> v8::Local<v8::Value> {
    return UrlBootstrap::createLegacyUrl(js, arg(info, 0), arg(info, 1));
  });
  add("tryCreateLegacyUrl"_kj,
      [=](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& info) -> v8::Local<v8::Value> {
    return UrlBootstrap::tryCreateLegacyUrl(js, arg(info, 0), arg(info, 1));
  });
  add("createLegacySearchParams"_kj,
      [=](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& info) -> v8::Local<v8::Value> {
    return UrlBootstrap::createLegacySearchParams(js);
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

KJ_TEST("urlParse returns the href and writes the components and scheme type") {
  expectHelperScript(R"js(
    const out = new Uint32Array(9);
    const show = (input, base) => {
      out.fill(7);
      const href = helpers.urlParse(input, base, out);
      return `${href} ${out.join(',')}`;
    };
    return [
      show('https://user:pass@example.com:1234/foo/bar?baz#quux'),
      show('../x?y', 'http://a.com/b/c'),
      show('mailto:x@y'),
      show('nope'),
      show('/x', 'nope'),
    ].join('\n');
  )js"_kj,
      "https://user:pass@example.com:1234/foo/bar?baz#quux 6,12,17,29,1234,34,42,46,2\n"
      "http://a.com/x?y 5,7,7,12,4294967295,12,14,4294967295,0\n"
      "mailto:x@y 7,7,7,7,4294967295,7,4294967295,4294967295,1\n"
      "undefined 7,7,7,7,7,7,7,7,7\n"
      "undefined 7,7,7,7,7,7,7,7,7"_kj);
}

KJ_TEST("urlParse and urlCanParse convert their arguments to USVStrings") {
  expectHelperScript(R"js(
    const out = new Uint32Array(9);
    return [
      helpers.urlParse('http://a/\uD800', undefined, out),
      helpers.urlCanParse('\uD800', 'http://a/'),
      helpers.urlCanParse('http://a/'),
      helpers.urlCanParse('x'),
    ].join(' ');
  )js"_kj,
      "http://a/%EF%BF%BD true true false"_kj);
}

KJ_TEST("urlUpdate applies each setter and reports only a rejected href") {
  expectHelperScript(R"js(
    const out = new Uint32Array(9);
    const base = 'http://u:p@a.com:81/p?q=1#h';
    const cases = [
      [0, 'https://b.com/'], [0, 'nope'],
      [1, 'https'], [1, '1bad'],
      [2, 'us er'], [3, 'pa:ss'],
      [4, 'b.com:99'], [4, 'a b'], [5, 'c.com'], [5, 'a b'],
      [6, '8080'], [6, ''], [6, 'abc'],
      [7, '/a b'], [8, 'x=1'], [8, ''], [9, 'h i'], [9, ''],
    ];
    return cases.map(([attr, value]) => {
      out.fill(7);
      const href = helpers.urlUpdate(base, attr, value, out);
      return `${attr} ${value}: ${href}${href === undefined ? ` ${out[0]}` : ''}`;
    }).join('\n');
  )js"_kj,
      "0 https://b.com/: https://b.com/\n"
      "0 nope: undefined 7\n"
      "1 https: https://u:p@a.com:81/p?q=1#h\n"
      "1 1bad: http://u:p@a.com:81/p?q=1#h\n"
      "2 us er: http://us%20er:p@a.com:81/p?q=1#h\n"
      "3 pa:ss: http://u:pa%3Ass@a.com:81/p?q=1#h\n"
      "4 b.com:99: http://u:p@b.com:99/p?q=1#h\n"
      "4 a b: http://u:p@a.com:81/p?q=1#h\n"
      "5 c.com: http://u:p@c.com:81/p?q=1#h\n"
      "5 a b: http://u:p@a.com:81/p?q=1#h\n"
      "6 8080: http://u:p@a.com:8080/p?q=1#h\n"
      "6 : http://u:p@a.com/p?q=1#h\n"
      "6 abc: http://u:p@a.com:81/p?q=1#h\n"
      "7 /a b: http://u:p@a.com:81/a%20b?q=1#h\n"
      "8 x=1: http://u:p@a.com:81/p?x=1#h\n"
      "8 : http://u:p@a.com:81/p#h\n"
      "9 h i: http://u:p@a.com:81/p?q=1#h%20i\n"
      "9 : http://u:p@a.com:81/p?q=1"_kj);
}

KJ_TEST("urlUpdate writes the components of the result") {
  expectHelperScript(R"js(
    const out = new Uint32Array(9);
    const href = helpers.urlUpdate('http://a.com/', 8, 'x=1', out);
    return `${href} ${out.join(',')}`;
  )js"_kj,
      "http://a.com/?x=1 5,7,7,12,4294967295,12,13,4294967295,0"_kj);
}

KJ_TEST("The URL helpers write the components into the view's own elements") {
  expectHelperScript(R"js(
    const backing = new Uint32Array(11);
    const out = backing.subarray(1, 10);
    const href = helpers.urlParse('http://a.com/', undefined, out);
    return `${href} ${backing.join(',')}`;
  )js"_kj,
      "http://a.com/ 0,5,7,7,12,4294967295,12,4294967295,4294967295,0,0"_kj);
}

KJ_TEST("urlOrigin serializes the origin, blob URLs included") {
  expectHelperScript(R"js(
    return [
      'http://a.com:81/x', 'https://a.com:443/', 'blob:https://a.com/x', 'blob:nope',
      'file:///x', 'data:,x',
    ].map((href) => helpers.urlOrigin(href)).join(' ');
  )js"_kj,
      "http://a.com:81 https://a.com https://a.com null null null"_kj);
}

KJ_TEST("The URL helpers reject arguments of the wrong type") {
  expectHelperScript(R"js(
    const out = new Uint32Array(9);
    const detached = new Uint32Array(9);
    detached.buffer.transfer();
    const attempt = (fn) => {
      try {
        fn();
        return 'ok';
      } catch (e) {
        return `${e.name}: ${e.message}`;
      }
    };
    return [
      attempt(() => helpers.urlParse(1, undefined, out)),
      attempt(() => helpers.urlParse('http://a/', 1, out)),
      attempt(() => helpers.urlParse('http://a/', undefined, new Uint32Array(8))),
      attempt(() => helpers.urlParse('http://a/', undefined, new Int32Array(9))),
      attempt(() => helpers.urlParse('http://a/', undefined, [])),
      attempt(() => helpers.urlParse('http://a/', undefined, detached)),
      attempt(() => helpers.urlUpdate('nope', 0, 'http://a/', out)),
      attempt(() => helpers.urlUpdate('http://a/', 10, 'x', out)),
      attempt(() => helpers.urlUpdate('http://a/', -1, 'x', out)),
      attempt(() => helpers.urlUpdate('http://a/', '0', 'x', out)),
      attempt(() => helpers.urlUpdate('http://a/', 0, 1, out)),
      attempt(() => helpers.urlOrigin('nope')),
      attempt(() => helpers.urlCanParse(undefined)),
    ].join('\n');
  )js"_kj,
      "TypeError: input must be a string\n"
      "TypeError: base must be a string\n"
      "TypeError: out must have 9 elements\n"
      "TypeError: out must be a Uint32Array\n"
      "TypeError: out must be a Uint32Array\n"
      "TypeError: out must have 9 elements\n"
      "TypeError: href must be a URL\n"
      "TypeError: attribute must be a URL attribute number\n"
      "TypeError: attribute must be a URL attribute number\n"
      "TypeError: attribute must be a URL attribute number\n"
      "TypeError: value must be a string\n"
      "TypeError: href must be a URL\n"
      "TypeError: input must be a string"_kj);
}

KJ_TEST("createLegacyUrl parses with the legacy parser and throws its errors") {
  expectHelperScript(R"js(
    const attempt = (fn) => {
      try {
        return fn();
      } catch (e) {
        return `${e.name}: ${e.message}`;
      }
    };
    return [
      helpers.createLegacyUrl('http://a.com:80/x?a=b c').getHref(),
      helpers.createLegacyUrl('../y', 'http://a.com/b/c').getHref(),
      // The legacy parser accepts this with a base, rather than rejecting it.
      helpers.createLegacyUrl('http://[', 'http://a/').getHref(),
      attempt(() => helpers.createLegacyUrl('nope')),
      attempt(() => helpers.createLegacyUrl('x', 'nope')),
      attempt(() => helpers.createLegacyUrl('http://a/', 1)),
      String(helpers.tryCreateLegacyUrl('nope')),
      String(helpers.tryCreateLegacyUrl('x', 'nope')),
      helpers.tryCreateLegacyUrl('/z', 'http://a/').getHref(),
    ].join('\n');
  )js"_kj,
      "http://a.com/x?a=b+c\n"
      "http://a.com/y\n"
      "http://[\n"
      "TypeError: Invalid URL string.\n"
      "TypeError: Invalid base URL string.\n"
      "TypeError: base must be a string\n"
      "undefined\n"
      "undefined\n"
      "http://a/z"_kj);
}

KJ_TEST("A LegacyUrlHandle's attributes and setters are the legacy URL's") {
  expectHelperScript(R"js(
    const url = helpers.createLegacyUrl('http://u:p@a.com:81/p?q=1#h');
    const get = () => [
      url.getHref(), url.getOrigin(), url.getProtocol(), url.getUsername(), url.getPassword(),
      url.getHost(), url.getHostname(), url.getPort(), url.getPathname(), url.getSearch(),
      url.getHash(),
    ].join(' ');
    const before = get();
    const rejected = url.setHref('nope');
    url.setProtocol('https:');
    url.setUsername('v');
    url.setPassword('');
    url.setHostname('b.com');
    url.setPort('99');
    url.setPathname('/x/../y z');
    url.setSearch('?a=b c');
    url.setHash('#k');
    return [before, String(rejected), get()].join('\n');
  )js"_kj,
      "http://u:p@a.com:81/p?q=1#h http://a.com:81 http: u p a.com:81 a.com 81 /p ?q=1 #h\n"
      "false\n"
      "https://v@b.com:99/y%20z?a=b+c#k https://b.com:99 https: v  b.com:99 b.com 99 /y%20z "
      "?a=b+c #k"_kj);
}

KJ_TEST("A LegacyUrlHandle's query is a URLSearchParams list") {
  expectHelperScript(R"js(
    const url = helpers.createLegacyUrl('http://a/?b=1&a=2&b=3&c');
    const list = () => {
      const entries = [];
      for (let i = 0; i < url.size(); i++) entries.push(`${url.nameAt(i)}=${url.valueAt(i)}`);
      return entries.join('&');
    };
    const lines = [
      list(), url.serialize(), String(url.get('b')), String(url.get('z')), url.getAll('b').join(),
      String(url.has('b')), String(url.has('b', '3')), String(url.has('b', '4')),
    ];
    url.append('d', '\uD800');
    url.delete('b', '1');
    lines.push(list());
    url.delete('b');
    url.set('a', 'x');
    url.set('e', 'y');
    url.sort();
    lines.push(list(), url.getHref());
    try {
      url.nameAt(url.size());
    } catch (e) {
      lines.push(`${e.name}: ${e.message}`);
    }

    const params = helpers.createLegacySearchParams();
    lines.push(`${params.size()} ${params.serialize()}`);
    params.assignQueryString('?x=1&y=a+b');
    lines.push(params.serialize());
    params.assignQueryPairs(['m', 'n', 'o', 'p']);
    lines.push(params.serialize());
    try {
      params.assignQueryPairs(['m']);
    } catch (e) {
      lines.push(`${e.name}: ${e.message}`);
    }
    return lines.join('\n');
  )js"_kj,
      "b=1&a=2&b=3&c=\n"
      "b=1&a=2&b=3&c=\n"
      "1\n"
      "null\n"
      "1,3\n"
      "true\n"
      "true\n"
      "false\n"
      "a=2&b=3&c=&d=\uFFFD\n"
      "a=x&c=&d=\uFFFD&e=y\n"
      "http://a/?a=x&c&d=%EF%BF%BD&e=y\n"
      "RangeError: index out of range\n"
      "0 \n"
      "x=1&y=a+b\n"
      "m=n&o=p\n"
      "TypeError: pairs must alternate names and values"_kj);
}

}  // namespace
}  // namespace workerd::api
