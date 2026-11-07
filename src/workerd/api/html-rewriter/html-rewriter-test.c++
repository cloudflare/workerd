// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tests writeAsync() and endAsync(), which run lol-html on a fiber, in the requests of a real
// worker. rewriter-test.rs tests the rest of the crate.

#include <workerd/api/html-rewriter/ffi.rs.h>
#include <workerd/rust/jsg/ffi-inl.h>
#include <workerd/tests/test-fixture.h>

#include <kj/test.h>

namespace workerd::api::html_rewriter::test {
namespace {

// `rewriter(ons, docs, sink, encoding, streams)` creates a rewriter with the handlers of `on()`
// calls, `[selector, handlers]`, and of `onDocument()` calls, `handlers`, for a document in
// `encoding`; `sink` gets the output the dispatcher gets, as a string, and `streams[id](sink)`
// writes the streaming content numbered `id`. `log` collects what the tests read, `error(f)` returns the
// message of what `f` throws, and `enc` and `dec` convert between Latin-1 strings and bytes.
constexpr kj::StringPtr PRELUDE = R"(
const enc = (s) => Uint8Array.from(s, (c) => c.charCodeAt(0));
const dec = (b) => (b === undefined ? '' : String.fromCharCode(...b));
const names = ['element', 'comments', 'text', 'doctype', 'comments', 'text', 'end'];
const log = [];
function rewriter(ons, docs = [], sink = () => {}, encoding = 'utf-8', streams = []) {
  const element = ons.map(([selector, h]) => ({
    selector: utils.parseSelector(selector),
    element: !!h.element, comments: !!h.comments, text: !!h.text,
  }));
  const document = docs.map((h) => ({
    doctype: !!h.doctype, comments: !!h.comments, text: !!h.text, end: !!h.end,
  }));
  return utils.newRewriter(element, document, encoding, (kind, id, token, output) => {
    sink(dec(output));
    if (kind === 8) return streams[id](token);
    return (kind < 3 ? ons[id][1] : docs[id])[names[kind]](token);
  });
}
function error(f) {
  try { f(); } catch (e) { return `${e.name}: ${e.message}`; }
  return 'no error';
}
)"_kj;

// The key of the async context frame value that `runInFrame()` sets and `frameValue()` reads.
const kj::Arc<jsg::AsyncContextFrame::StorageKey>& frameKey() {
  static const auto key = kj::arc<jsg::AsyncContextFrame::StorageKey>();
  return key;
}

// runInFrame(value, f): calls `f` in a new async context frame with `value`, and returns what it
// returns.
void runInFrame(const v8::FunctionCallbackInfo<v8::Value>& info) {
  auto& js = jsg::Lock::from(info.GetIsolate());
  jsg::AsyncContextFrame::StorageScope scope(js, frameKey().addRef(), js.v8Ref(info[0]));
  v8::Local<v8::Value> result;
  if (info[1]
          .As<v8::Function>()
          ->Call(js.v8Context(), js.v8Undefined(), 0, nullptr)
          .ToLocal(&result)) {
    info.GetReturnValue().Set(result);
  }
}

// frameValue(): the value of the current async context frame, or "none".
void frameValue(const v8::FunctionCallbackInfo<v8::Value>& info) {
  auto& js = jsg::Lock::from(info.GetIsolate());
  KJ_IF_SOME(frame, jsg::AsyncContextFrame::current(js)) {
    KJ_IF_SOME(value, frame.get(*frameKey())) {
      info.GetReturnValue().Set(value.getHandle(js));
      return;
    }
  }
  info.GetReturnValue().Set(v8::Local<v8::Value>(js.str("none"_kj)));
}

v8::Local<v8::Value> runScript(const TestFixture::Environment& env, kj::StringPtr source) {
  auto v8Context = env.isolate->GetCurrentContext();
  auto script =
      jsg::check(v8::Script::Compile(v8Context, v8::Local<v8::String>(env.js.str(source))));
  return jsg::check(script->Run(v8Context));
}

// Runs `source`, a script, in `request`, and returns its value as a string.
kj::String runIn(TestFixture& fixture, IoContext::IncomingRequest& request, kj::StringPtr source) {
  kj::String value;
  fixture.enterContext(request, [&](const TestFixture::Environment& env) {
    value = env.js.toString(jsg::JsValue(runScript(env, source)));
  });
  return value;
}

// Runs `source` in a request of its own. A request whose last pending event completes is aborted
// as hung once the thread is idle, so a test reads, and continues, in requests of their own.
kj::String evaluate(TestFixture& fixture, kj::StringPtr source) {
  auto request = fixture.newIncomingRequest();
  auto value = runIn(fixture, *request, source);
  fixture.drainAndDestroy(kj::mv(request));
  return value;
}

// Defines `utils`, the HtmlRewriterUtils class, `runInFrame()`, `frameValue()`, and the globals of
// PRELUDE.
void setUp(TestFixture& fixture) {
  auto request = fixture.newIncomingRequest();
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    auto context = env.isolate->GetCurrentContext();
    auto define = [&](kj::StringPtr name, v8::Local<v8::Value> value) {
      KJ_ASSERT(jsg::check(
          context->Global()->Set(context, v8::Local<v8::Value>(env.js.str(name)), value)));
    };
    define(
        "utils"_kj, jsg::check(rust::jsg::maybe_local_from_ffi<v8::Value>(new_utils(env.isolate))));
    define("runInFrame"_kj, jsg::check(v8::Function::New(context, runInFrame)));
    define("frameValue"_kj, jsg::check(v8::Function::New(context, frameValue)));
    runScript(env, PRELUDE);
  });
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("an async handler keeps lol-html waiting, and can use its token until it settles") {
  TestFixture fixture;
  setUp(fixture);
  auto request = fixture.newIncomingRequest();
  runIn(fixture, *request, R"(
    var out = '';
    var release;
    const gate = new Promise((resolve) => { release = resolve; });
    var kept;
    var rw = rewriter([['p', { async element(el) {
      kept = el;
      log.push('handler');
      await gate;
      el.setAttribute('a', '1');
      el.append('!', false);
      log.push('resumed');
    } }]], [], (s) => { out += s; });
    rw.writeAsync(enc('<div><p>x</p>')).then((v) => { out += dec(v); log.push('written'); });
  )");
  fixture.pollEventLoop();
  KJ_EXPECT(evaluate(fixture, "log.join()") == "handler");
  KJ_EXPECT(evaluate(fixture, "out") == "<div>");

  evaluate(fixture, "release()");
  fixture.pollEventLoop();
  KJ_EXPECT(evaluate(fixture, "log.join()") == "handler,resumed,written");
  KJ_EXPECT(evaluate(fixture, "out") == "<div><p a=\"1\">x!</p>");
  KJ_EXPECT(evaluate(fixture, "error(() => kept.tagName())") ==
      "TypeError: This content token is no longer valid. Content tokens are only valid during "
      "the execution of the relevant content handler.");

  auto ender = fixture.newIncomingRequest();
  runIn(fixture, *ender, "rw.endAsync().then((v) => { log.push(`ended ${dec(v)}`); })");
  fixture.pollEventLoop();
  fixture.drainAndDestroy(kj::mv(ender));
  KJ_EXPECT(evaluate(fixture, "log.at(-1)") == "ended ");
  KJ_EXPECT(evaluate(fixture, "error(() => rw.writeAsync(enc('x')))") ==
      "TypeError: This HTMLRewriter has already ended.");
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("a handler's rejection rejects the call with its reason, and fails the rewriter") {
  TestFixture fixture;
  setUp(fixture);
  auto request = fixture.newIncomingRequest();
  runIn(fixture, *request, R"(
    const thrown = new RangeError('boom');
    var rw = rewriter([['p', { async element() { await null; throw thrown; } }]]);
    rw.writeAsync(enc('<p>')).then(() => log.push('fulfilled'), (e) => {
      log.push(e === thrown);
      log.push(error(() => rw.writeAsync(enc('x'))));
    });
  )");
  fixture.pollEventLoop();
  KJ_EXPECT(
      evaluate(fixture, "log.join('\\n')") == "true\nTypeError: This HTMLRewriter has failed.");
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("a handler that throws rejects the call with its exception") {
  TestFixture fixture;
  setUp(fixture);
  auto request = fixture.newIncomingRequest();
  runIn(fixture, *request, R"(
    const thrown = new RangeError('boom');
    let calls = 0;
    var rw = rewriter([['p', { element() { ++calls; throw thrown; } }]]);
    rw.writeAsync(enc('<p><p>')).then(() => log.push('fulfilled'), (e) => {
      log.push(e === thrown, calls);
    });
  )");
  fixture.pollEventLoop();
  KJ_EXPECT(evaluate(fixture, "log.join()") == "true,1");
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("lol-html's errors reject the call with a parser error") {
  TestFixture fixture;
  setUp(fixture);
  auto request = fixture.newIncomingRequest();
  runIn(fixture, *request, R"(
    var rw = rewriter([['br', { element(el) { el.onEndTag(0); } }]]);
    rw.writeAsync(enc('<br>')).catch((e) => log.push(`${e.name}: ${e.message}`));
  )");
  fixture.pollEventLoop();
  // onEndTag() throws in the handler, so the call fails with that error.
  KJ_EXPECT(evaluate(fixture, "log.join()") == "TypeError: Parser error: br can't have content");
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("a handler can't use the rewriter that is running it") {
  TestFixture fixture;
  setUp(fixture);
  auto request = fixture.newIncomingRequest();
  runIn(fixture, *request, R"(
    var rw = rewriter([['p', { element() {
      log.push(error(() => rw.writeAsync(enc('x'))));
      log.push(error(() => rw.write(enc('x'))));
      log.push(error(() => rw.endAsync()));
    } }]]);
    rw.writeAsync(enc('<p>')).then((v) => log.push(dec(v)));
  )");
  fixture.pollEventLoop();
  KJ_EXPECT(evaluate(fixture, "log.join('\\n')") ==
      "TypeError: An HTMLRewriter handler can't use the rewriter that is running it.\n"
      "TypeError: An HTMLRewriter handler can't use the rewriter that is running it.\n"
      "TypeError: An HTMLRewriter handler can't use the rewriter that is running it.\n"
      "<p>");
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("endAsync() runs the document end handlers") {
  TestFixture fixture;
  setUp(fixture);
  auto request = fixture.newIncomingRequest();
  runIn(fixture, *request, R"(
    var rw = rewriter([], [{ async end(end) { await null; end.append('<!--end-->', true); } }]);
    rw.writeAsync(enc('<p>x'))
        .then((v) => { log.push(dec(v)); return rw.endAsync(); })
        .then((v) => log.push(dec(v)));
  )");
  fixture.pollEventLoop();
  KJ_EXPECT(evaluate(fixture, "log.join('|')") == "<p>x|<!--end-->");
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("handlers run in the async context frame the rewriter was created in") {
  TestFixture fixture;
  setUp(fixture);
  auto request = fixture.newIncomingRequest();
  runIn(fixture, *request, R"(
    var rw = runInFrame('created', () => rewriter([['p', { async element() {
      log.push(frameValue());
      await null;
      log.push(frameValue());
    } }]]));
    runInFrame('called', () => rw.writeAsync(enc('<p>'))
        .then(() => log.push(`then ${frameValue()}`)));
    var sync = runInFrame('created', () => rewriter([['b', { element() {
      log.push(`sync ${frameValue()}`);
    } }]]));
  )");
  fixture.pollEventLoop();
  KJ_EXPECT(evaluate(fixture, "log.join()") == "created,created,then called");
  KJ_EXPECT(evaluate(fixture, "runInFrame('called', () => sync.write(enc('<b>'))); log.at(-1)") ==
      "sync created");
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("lol-html decodes and encodes a document on the fiber's stack") {
  TestFixture fixture;
  setUp(fixture);
  auto request = fixture.newIncomingRequest();
  // "\xe9" is "é" in windows-1252, which encoding_rs decodes on the fiber.
  runIn(fixture, *request, R"(
    const text = 'caf\xe9 '.repeat(4096);
    let seen = '';
    let out = '';
    var rw = rewriter([['p', { text(t) { seen += t.text(); t.after('\xe9', false); } }]],
                      [], (s) => { out += s; }, 'windows-1252');
    rw.writeAsync(enc(`<p>${text}</p>`))
        .then((v) => {
          out += dec(v);
          log.push(seen === text.replaceAll('\xe9', '\u00e9'));
          log.push(out.startsWith(`<p>caf\xe9 caf\xe9`) && out.endsWith(' \xe9\xe9</p>'));
        });
  )");
  fixture.pollEventLoop();
  KJ_EXPECT(evaluate(fixture, "log.join()") == "true,true");
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("streaming content can be written after an await") {
  TestFixture fixture;
  setUp(fixture);
  auto request = fixture.newIncomingRequest();
  runIn(fixture, *request, R"(
    let out = '';
    const streams = [async (sink) => {
      sink.write(enc('<b>'));
      await null;
      sink.write(enc('</b>'));
    }];
    var rw = rewriter([['p', { element(el) { el.replaceStream(0, true); } }]], [],
                      (s) => { out += s; }, 'utf-8', streams);
    rw.writeAsync(enc('<div><p>x</p></div>')).then((v) => log.push(out + dec(v)));
  )");
  fixture.pollEventLoop();
  KJ_EXPECT(evaluate(fixture, "log.join()") == "<div><b></b></div>");
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("streaming content goes to its rewriter when calls interleave") {
  TestFixture fixture;
  setUp(fixture);
  auto request = fixture.newIncomingRequest();
  // A waits in a handler while B runs, then writes its content.
  runIn(fixture, *request, R"(
    var release;
    const gate = new Promise((resolve) => { release = resolve; });
    const outs = { a: '', b: '' };
    const make = (name, wait) => rewriter([['p', { async element(el) {
      el.replaceStream(0, false);
      if (wait) await gate;
    } }]], [], (s) => { outs[name] += s; }, 'utf-8', [(sink) => { sink.write(enc(name)); }]);
    const a = make('a', true);
    const b = make('b', false);
    a.writeAsync(enc('<p>1</p>!')).then((v) => {
      outs.a += dec(v);
      log.push(`a ${outs.a}`);
    });
    b.writeAsync(enc('<p>2</p>!')).then((v) => {
      outs.b += dec(v);
      log.push(`b ${outs.b}`);
      release();
    });
  )");
  fixture.pollEventLoop();
  KJ_EXPECT(evaluate(fixture, "log.join()") == "b b!,a a!");
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("destroying the IoContext cancels a waiting call, and fails the rewriter") {
  TestFixture fixture;
  setUp(fixture);
  auto request = fixture.newIncomingRequest();
  runIn(fixture, *request, R"(
    var kept;
    var rw = rewriter([['p', { async element(el) { kept = el; await new Promise(() => {}); } }]]);
    rw.writeAsync(enc('<p>')).then(() => log.push('fulfilled'), () => log.push('rejected'));
  )");
  fixture.pollEventLoop();
  fixture.drainAndDestroy(kj::mv(request));

  KJ_EXPECT(evaluate(fixture, "log.join()") == "");
  KJ_EXPECT(evaluate(fixture, "error(() => kept.tagName())") ==
      "TypeError: This content token is no longer valid. Content tokens are only valid during "
      "the execution of the relevant content handler.");
  KJ_EXPECT(evaluate(fixture, "error(() => rw.writeAsync(enc('x')))") ==
      "TypeError: This HTMLRewriter has failed.");
}

}  // namespace
}  // namespace workerd::api::html_rewriter::test
