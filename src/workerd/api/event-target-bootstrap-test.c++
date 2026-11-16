// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/event-target-bootstrap.h>
#include <workerd/api/events.h>
#include <workerd/jsg/script.h>
#include <workerd/tests/test-fixture.h>

#include <capnp/message.h>
#include <kj/test.h>

namespace workerd::api {
namespace {

using Callback = jsg::Function<v8::Local<v8::Value>(const v8::FunctionCallbackInfo<v8::Value>&)>;

// The seam's functions as JS functions, taking the same arguments as the bootstrap's utils
// members.
jsg::JsObject makeSeam(jsg::Lock& js) {
  auto seam = js.obj();
  auto context = js.v8Context();
  const auto add = [&](kj::StringPtr name, Callback callback) {
    seam.set(js, name, jsg::JsValue(js.wrapReturningFunction(context, kj::mv(callback))));
  };

  add("setListenerCountHook"_kj,
      [](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& info) -> v8::Local<v8::Value> {
    EventTargetBootstrap::setListenerCountHook(
        js, jsg::JsValue(info[0]), jsg::JsValue(info[1]), jsg::JsValue(info[2]));
    return js.undefined();
  });
  add("getEventHandlerAttribute"_kj,
      [](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& info) -> v8::Local<v8::Value> {
    return EventTargetBootstrap::getEventHandlerAttribute(
        js, jsg::JsValue(info[0]), jsg::JsValue(info[1]));
  });
  add("setEventHandlerAttribute"_kj,
      [](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& info) -> v8::Local<v8::Value> {
    EventTargetBootstrap::setEventHandlerAttribute(
        js, jsg::JsValue(info[0]), jsg::JsValue(info[1]), jsg::JsValue(info[2]));
    return js.undefined();
  });
  add("dispatchEvent"_kj,
      [](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& info) -> v8::Local<v8::Value> {
    return EventTargetBootstrap::dispatchEvent(js, jsg::JsValue(info[0]), jsg::JsValue(info[1]),
        ReportListenerExceptions(info[2]->BooleanValue(js.v8Isolate)));
  });
  add("createTrustedMessageEvent"_kj,
      [](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& info) -> v8::Local<v8::Value> {
    return EventTargetBootstrap::createTrustedMessageEvent(js, jsg::JsValue(info[0]),
        jsg::JsValue(info[1]), jsg::JsValue(info[2]), jsg::JsValue(info[3]), jsg::JsValue(info[4]));
  });
  add("createTrustedOpenEvent"_kj,
      [](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>&) -> v8::Local<v8::Value> {
    return EventTargetBootstrap::createTrustedOpenEvent(js);
  });
  add("createTrustedErrorEvent"_kj,
      [](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& info) -> v8::Local<v8::Value> {
    return EventTargetBootstrap::createTrustedErrorEvent(js, jsg::JsValue(info[0]));
  });
  // Stands in for a TypeScript MessagePort, which its constructor brands.
  add("brandAsMessagePort"_kj,
      [](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& info) -> v8::Local<v8::Value> {
    auto object = KJ_ASSERT_NONNULL(jsg::JsValue(info[0]).tryCast<jsg::JsObject>());
    object.setPrivate(js, messageevent::MessagePort::BRAND, js.boolean(true));
    return object;
  });
  return seam;
}

// Runs `body` as the body of a function of `seam`, returning its result as a string.
kj::String runSeamScript(const TestFixture::Environment& env, kj::StringPtr body) {
  auto& js = env.js;
  auto code = kj::str("((seam) => {", body, "})");
  auto fn = KJ_ASSERT_NONNULL(jsg::NonModuleScript::compile(js, code, "seam-test.js"_kj)
                                  .runAndReturn(js)
                                  .tryCast<jsg::JsFunction>());
  return fn.call(js, js.undefined(), jsg::JsValue(makeSeam(js))).toString(js);
}

void expectSeamScript(kj::StringPtr body,
    kj::StringPtr expected,
    kj::Maybe<CompatibilityFlags::Reader> featureFlags = kj::none) {
  TestFixture fixture({.featureFlags = featureFlags});
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto result = runSeamScript(env, body);
    KJ_EXPECT(result == expected, result, expected);
  });
}

KJ_TEST("Listener-count hooks see every change in the number of listeners of their type") {
  expectSeamScript(R"js(
    const target = new EventTarget();
    const calls = [];
    seam.setListenerCountHook(target, 'message', (type, count) => calls.push(type + count));

    const a = () => {};
    target.addEventListener('message', a);
    // A duplicate registration changes nothing, and other types are not reported.
    target.addEventListener('message', a);
    target.addEventListener('other', a);

    // A once-listener is removed as it is dispatched.
    target.addEventListener('message', () => {}, { once: true });
    seam.dispatchEvent(target, new Event('message'), true);

    // A listener registered with an AbortSignal is removed when it aborts.
    const controller = new AbortController();
    target.addEventListener('message', () => {}, { signal: controller.signal });
    controller.abort();

    target.removeEventListener('message', a);

    // The handler attribute's trampoline occupies a listener slot of its own, kept across
    // reassignment.
    seam.setEventHandlerAttribute(target, 'message', () => {});
    seam.setEventHandlerAttribute(target, 'message', () => {});
    seam.setEventHandlerAttribute(target, 'message', null);
    return calls.join();
  )js"_kj,
      "message1,message2,message1,message2,message1,message0,message1,message0"_kj);
}

KJ_TEST("A listener-count hook replaces the previous hook for its type") {
  expectSeamScript(R"js(
    const target = new EventTarget();
    const calls = [];
    seam.setListenerCountHook(target, 'a', (type, count) => calls.push('first:' + type));
    seam.setListenerCountHook(target, 'b', (type, count) => calls.push('b:' + type));
    seam.setListenerCountHook(target, 'a', (type, count) => calls.push('second:' + type));
    target.addEventListener('a', () => {});
    target.addEventListener('b', () => {});
    return calls.join();
  )js"_kj,
      "second:a,b:b"_kj);
}

KJ_TEST("The handler attribute keeps its position across reassignment, and fires once") {
  expectSeamScript(R"js(
    const target = new EventTarget();
    const order = [];
    target.addEventListener('message', () => order.push('a'));
    const first = () => order.push('first');
    seam.setEventHandlerAttribute(target, 'message', first);
    const same = seam.getEventHandlerAttribute(target, 'message') === first;
    target.addEventListener('message', () => order.push('c'));
    seam.setEventHandlerAttribute(target, 'message', () => order.push('b'));
    // A managed type is not also read as a plain on<type> property.
    target.onmessage = () => order.push('reflected');
    seam.dispatchEvent(target, new Event('message'), true);

    // Clearing it, then assigning again, takes a fresh position at the end.
    seam.setEventHandlerAttribute(target, 'message', undefined);
    const cleared = seam.getEventHandlerAttribute(target, 'message');
    seam.setEventHandlerAttribute(target, 'message', () => order.push('last'));
    seam.dispatchEvent(target, new Event('message'), true);
    return [same, cleared, order.join()].join(';');
  )js"_kj,
      "true;;a,b,c,a,c,last"_kj);
}

KJ_TEST("The handler attribute retains non-callable objects and drops primitives") {
  expectSeamScript(R"js(
    const target = new EventTarget();
    const object = {};
    seam.setEventHandlerAttribute(target, 'x', object);
    const retained = seam.getEventHandlerAttribute(target, 'x') === object;
    seam.dispatchEvent(target, new Event('x'), true);
    seam.setEventHandlerAttribute(target, 'x', 5);
    return [retained, seam.getEventHandlerAttribute(target, 'x')].join();
  )js"_kj,
      "true,"_kj);
}

KJ_TEST("The handler attribute is called on the global object and cancels by returning true") {
  expectSeamScript(R"js(
    const target = new EventTarget();
    let receiver;
    seam.setEventHandlerAttribute(target, 'x', function () {
      receiver = this;
      return true;
    });
    const event = new Event('x', { cancelable: true });
    seam.dispatchEvent(target, event, true);
    return [receiver === globalThis, event.defaultPrevented].join();
  )js"_kj,
      "true,true"_kj);
}

KJ_TEST("A reporting dispatch reports each listener exception and returns the first") {
  expectSeamScript(R"js(
    const reported = [];
    const onError = (event) => {
      reported.push(event.error instanceof Error ? event.error.message : String(event.error));
      event.preventDefault();
    };
    addEventListener('error', onError);
    try {
      const target = new EventTarget();
      const order = [];
      const boom = new Error('first');
      target.addEventListener('x', () => { order.push(1); throw boom; });
      target.addEventListener('x', () => { order.push(2); throw new Error('second'); });
      target.addEventListener('x', () => order.push(3));
      const outcome = seam.dispatchEvent(target, new Event('x'), true);

      const quiet = seam.dispatchEvent(new EventTarget(), new Event('x'), true);

      // A thrown undefined is still distinguishable from no exception.
      const undefinedThrower = new EventTarget();
      undefinedThrower.addEventListener('x', () => { throw undefined; });
      const thrownUndefined =
          seam.dispatchEvent(undefinedThrower, new Event('x'), true);

      return [
        outcome.exception === boom,
        Object.getPrototypeOf(outcome) === null,
        order.join(''),
        reported.join('+'),
        quiet,
        'exception' in thrownUndefined && thrownUndefined.exception === undefined,
      ].join();
    } finally {
      removeEventListener('error', onError);
    }
  )js"_kj,
      "true,true,123,first+second+undefined,,true"_kj);
}

KJ_TEST("A propagating dispatch ends at the first listener exception and throws it") {
  expectSeamScript(R"js(
    const target = new EventTarget();
    const order = [];
    target.addEventListener('x', () => { order.push(1); throw new Error('boom'); });
    target.addEventListener('x', () => order.push(2));
    try {
      seam.dispatchEvent(target, new Event('x'), false);
      return 'did not throw';
    } catch (e) {
      return [e.message, order.join('')].join();
    }
  )js"_kj,
      "boom,1"_kj);
}

KJ_TEST("A trusted MessageEvent looks as the runtime fires one") {
  expectSeamScript(R"js(
    const data = { x: 1 };
    const port = seam.brandAsMessagePort({});
    const event = seam.createTrustedMessageEvent('messageerror', data, port);
    const sourceless = seam.createTrustedMessageEvent('message', 1, null);
    return [
      event instanceof MessageEvent,
      event.type,
      event.isTrusted,
      event.data === data,
      event.source === port,
      event.origin,
      JSON.stringify(event.lastEventId),
      event.ports.length,
      sourceless.source,
      seam.createTrustedMessageEvent('message', 1, undefined).source,
      new MessageEvent('message').isTrusted,
    ].join();
  )js"_kj,
      "true,messageerror,true,true,true,,\"\",0,,,false"_kj);
}

KJ_TEST("A trusted MessageEvent carries the lastEventId and origin given") {
  expectSeamScript(R"js(
    const event = seam.createTrustedMessageEvent('update', 'x', null, '7', 'https://a.example');
    const nullOrigin = seam.createTrustedMessageEvent('message', 'x', null, '', null);
    return [
      event.type,
      event.isTrusted,
      event.lastEventId,
      event.origin,
      event.source,
      JSON.stringify(nullOrigin.lastEventId),
      nullOrigin.origin,
    ].join();
  )js"_kj,
      "update,true,7,https://a.example,,\"\","_kj);
}

KJ_TEST("Trusted open and error events look as the runtime fires them") {
  expectSeamScript(R"js(
    const open = seam.createTrustedOpenEvent();
    const error = new Error('boom');
    const errorEvent = seam.createTrustedErrorEvent(error);
    return [
      open instanceof Event,
      Object.getPrototypeOf(open).constructor.name,
      open.type,
      open.isTrusted,
      open.bubbles,
      open.cancelable,
      errorEvent instanceof ErrorEvent,
      errorEvent.type,
      errorEvent.isTrusted,
      errorEvent.bubbles,
      errorEvent.cancelable,
      errorEvent.error === error,
      JSON.stringify(errorEvent.message),
      JSON.stringify(errorEvent.filename),
      errorEvent.lineno,
      errorEvent.colno,
    ].join();
  )js"_kj,
      "true,OpenEvent,open,true,false,false,"
      "true,error,true,false,false,true,\"\",\"\",0,0"_kj);
}

KJ_TEST("A MessageEvent init accepts branded MessagePorts and rejects anything else") {
  expectSeamScript(R"js(
    const port = seam.brandAsMessagePort({});
    const other = seam.brandAsMessagePort(new EventTarget());
    const event = new MessageEvent('message', { source: port, ports: [port, other] });
    const results = [
      event.source === port,
      event.ports.length,
      event.ports[0] === port,
      event.ports[1] === other,
      new MessageEvent('message', { source: null }).source,
    ];
    const attempt = (init) => {
      try {
        new MessageEvent('message', init);
        results.push('ok');
      } catch (e) {
        results.push(e.constructor.name + ': ' + e.message);
      }
    };
    // Copying every visible property of a port does not make an impostor one. (Without
    // dont_substitute_null_on_type_error, JSG reads a nullable member of the wrong type as
    // null.)
    const impostor = {};
    for (const key of Reflect.ownKeys(port)) impostor[key] = port[key];
    results.push(new MessageEvent('message', { source: impostor }).source);
    attempt({ ports: [port, new EventTarget()] });
    return results.join('\n');
  )js"_kj,
      "true\n2\ntrue\ntrue\n\n\n"
      "TypeError: Incorrect type for array element 1: the provided value is not of type "
      "'MessagePort'."_kj);
}

KJ_TEST("A MessageEvent init rejects an impostor source when null is not substituted") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setNoSubstituteNull(true);
  expectSeamScript(R"js(
    try {
      new MessageEvent('message', { source: new EventTarget() });
      return 'ok';
    } catch (e) {
      return e.constructor.name + ': ' + e.message;
    }
  )js"_kj,
      "TypeError: Incorrect type for the 'source' field on 'Initializer': the provided value is "
      "not of type 'MessagePort'."_kj,
      flags.asReader());
}

KJ_TEST("The seam rejects bad arguments with TypeErrors") {
  expectSeamScript(R"js(
    const messages = [];
    const attempt = (fn) => {
      try {
        fn();
        messages.push('ok');
      } catch (e) {
        messages.push(e.constructor.name + ': ' + e.message);
      }
    };
    const target = new EventTarget();
    attempt(() => seam.setListenerCountHook({}, 'x', () => {}));
    attempt(() => seam.setListenerCountHook(target, 1, () => {}));
    attempt(() => seam.setListenerCountHook(target, 'x', {}));
    attempt(() => seam.getEventHandlerAttribute({}, 'x'));
    attempt(() => seam.setEventHandlerAttribute(target, Symbol(), null));
    attempt(() => seam.dispatchEvent(target, {}, true));
    attempt(() => seam.dispatchEvent(null, new Event('x'), true));
    attempt(() => seam.createTrustedMessageEvent(1, null, null));
    attempt(() => seam.createTrustedMessageEvent('message', null, {}));
    attempt(() => seam.createTrustedMessageEvent('message', null, null, 7));
    attempt(() => seam.createTrustedMessageEvent('message', null, null, '', {}));
    return messages.join('\n');
  )js"_kj,
      "TypeError: setListenerCountHook() expects an EventTarget\n"
      "TypeError: setListenerCountHook() expects a string event type\n"
      "TypeError: setListenerCountHook() expects a function\n"
      "TypeError: getEventHandlerAttribute() expects an EventTarget\n"
      "TypeError: setEventHandlerAttribute() expects a string event type\n"
      "TypeError: dispatchEvent() expects an Event\n"
      "TypeError: dispatchEvent() expects an EventTarget\n"
      "TypeError: createTrustedMessageEvent() expects a string event type\n"
      "TypeError: createTrustedMessageEvent() expects a MessagePort source\n"
      "TypeError: createTrustedMessageEvent() expects a string lastEventId\n"
      "TypeError: createTrustedMessageEvent() expects a string origin"_kj);
}

}  // namespace
}  // namespace workerd::api
