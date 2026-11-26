// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// An EventSource uses the built-ins it relies on as they were before any
// user code ran, even in a Worker that replaces them at its top level,
// before it first touches EventSource. The suite's other modules share an
// isolate whose earlier tests have already used EventSource, so this is a
// Worker of its own, which also serves the event stream it connects to.

import { deepStrictEqual } from 'node:assert';

const calls = [];

// Replaces each of `names`, on `object` or the prototype it is inherited
// from, with a function, or for an accessor a getter, that records its call
// and then does what the original does.
function spyOn(label, object, names) {
  for (const name of names) {
    let owner = object;
    while (owner !== null && !Object.hasOwn(owner, name)) {
      owner = Object.getPrototypeOf(owner);
    }
    if (owner === null) throw new Error(`${label}.${name} not found`);
    const descriptor = Object.getOwnPropertyDescriptor(owner, name);
    const replacement = { ...descriptor };
    if ('value' in descriptor) {
      replacement.value = function (...args) {
        calls.push(`${label}.${name}`);
        return descriptor.value.apply(this, args);
      };
    } else {
      replacement.get = function () {
        calls.push(`${label}.${name}`);
        return descriptor.get.call(this);
      };
    }
    Object.defineProperty(owner, name, replacement);
  }
}

spyOn('ReadableStream', ReadableStream.prototype, ['getReader']);
spyOn('ReadableStreamDefaultReader', ReadableStreamDefaultReader.prototype, [
  'read',
  'cancel',
]);
spyOn('Response', Response.prototype, [
  'ok',
  'status',
  'headers',
  'redirected',
  'url',
  'body',
]);
spyOn('Headers', Headers.prototype, ['get']);

function nextMessage(source) {
  return new Promise((resolve) => {
    source.addEventListener('message', (event) => resolve(event.data), {
      once: true,
    });
  });
}

export default {
  fetch() {
    return new Response('data: connected\n\n', {
      headers: { 'content-type': 'text/event-stream' },
    });
  },
};

export const fromStreamAfterFirstTouch = {
  async test() {
    calls.length = 0;
    const stream = new ReadableStream({
      start(controller) {
        controller.enqueue(new TextEncoder().encode('data: from\n\n'));
      },
    });
    const source = EventSource.from(stream);
    deepStrictEqual(await nextMessage(source), 'from');
    source.close();
    deepStrictEqual(calls, []);
  },
};

export const connectionAfterFirstTouch = {
  async test(ctrl, env) {
    calls.length = 0;
    const source = new EventSource('http://example.org/', {
      fetcher: env.self,
    });
    deepStrictEqual(await nextMessage(source), 'connected');
    source.close();
    deepStrictEqual(calls, []);
  },
};
