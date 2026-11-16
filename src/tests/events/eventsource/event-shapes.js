// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The events an EventSource fires: a trusted OpenEvent, MessageEvents and
// ErrorEvents, none of them bubbling or cancelable.

import { deepStrictEqual, ok, strictEqual } from 'node:assert';
import { connect, nextEvent, streamOf } from 'helpers';

function flags(event) {
  return [event.isTrusted, event.bubbles, event.cancelable];
}

export const openEvent = {
  async test(ctrl, env) {
    const source = connect(env, '/request-headers');
    let readyState;
    source.onopen = () => (readyState = source.readyState);
    const event = await nextEvent(source, 'open');
    source.close();
    const proto = Object.getPrototypeOf(event);
    strictEqual(proto.constructor.name, 'OpenEvent');
    strictEqual(Object.getPrototypeOf(proto), Event.prototype);
    ok(!('OpenEvent' in globalThis));
    strictEqual(event.type, 'open');
    deepStrictEqual(flags(event), [true, false, false]);
    strictEqual(event.target, source);
    strictEqual(readyState, EventSource.OPEN);
  },
};

// A connected EventSource's messages carry its URL's origin; an
// EventSource.from() source's a null origin.
export const messageEvent = {
  async test(ctrl, env) {
    const connected = connect(env, '/last-event-id');
    const event = await nextEvent(connected, 'message');
    connected.close();
    strictEqual(Object.getPrototypeOf(event), MessageEvent.prototype);
    strictEqual(event.type, 'message');
    deepStrictEqual(flags(event), [true, false, false]);
    strictEqual(event.data, 'first');
    strictEqual(event.lastEventId, '1');
    strictEqual(event.origin, 'http://example.org');
    strictEqual(event.source, null);
    deepStrictEqual(event.ports, []);
    strictEqual(event.target, connected);

    const fromStream = EventSource.from(streamOf(['data: x\n\n']));
    strictEqual((await nextEvent(fromStream, 'message')).origin, null);
  },
};

export const errorEvent = {
  async test() {
    const source = EventSource.from(streamOf([]));
    const event = await nextEvent(source, 'error');
    strictEqual(Object.getPrototypeOf(event), ErrorEvent.prototype);
    strictEqual(event.type, 'error');
    deepStrictEqual(flags(event), [true, false, false]);
    strictEqual(event.error.constructor, Error);
    strictEqual(event.error.message, 'The server disconnected.');
    deepStrictEqual(
      [event.message, event.filename, event.lineno, event.colno],
      ['', '', 0, 0]
    );
    strictEqual(event.target, source);
  },
};
