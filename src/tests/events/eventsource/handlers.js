// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// onopen, onmessage and onerror are HTML event handler attributes, and any
// other on<type> property is called as EventTarget calls it.

import { deepStrictEqual, strictEqual } from 'node:assert';
import { nextEvent, streamOf } from 'helpers';

export const handlersDefaultToNull = {
  test() {
    const source = EventSource.from(streamOf([], { close: false }));
    for (const name of ['onopen', 'onmessage', 'onerror']) {
      strictEqual(source[name], null);
    }
    source.close();
  },
};

// The handler occupies the position in the listener list of its first
// assignment, which reassignment keeps.
export const handlerPositionalOrdering = {
  async test() {
    const source = EventSource.from(streamOf(['data: hello\n\n']));
    const order = [];
    source.addEventListener('message', () => order.push('a'));
    const b1 = () => order.push('b1');
    source.onmessage = b1;
    strictEqual(source.onmessage, b1);
    source.addEventListener('message', () => order.push('c'));
    source.onmessage = () => order.push('b2');
    await nextEvent(source, 'message');
    deepStrictEqual(order, ['a', 'b2', 'c']);
  },
};

export const onPropertyForANamedEvent = {
  async test() {
    const source = EventSource.from(
      streamOf(['event: test\ndata: first\n\nevent: test\ndata: second\n\n'])
    );
    const data = [];
    source.ontest = (event) => data.push(event.data);
    await nextEvent(source, 'error');
    deepStrictEqual(data, ['first', 'second']);
  },
};
