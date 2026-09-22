// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { deepStrictEqual, strictEqual, throws } from 'node:assert';
import { mock } from 'node:test';

// With spec_compliant_dispatch_exceptions disabled (pinned off via the disable flag so
// every test variant runs this path), event dispatch keeps the legacy PROPAGATE
// semantics: the first throwing listener ends the dispatch and the exception propagates
// to the dispatching code.

export const dispatchEventPropagates = {
  test() {
    const target = new EventTarget();
    const boom = new Error('boom');
    const l2 = mock.fn();
    target.addEventListener('foo', () => {
      throw boom;
    });
    target.addEventListener('foo', l2);
    // The exception propagates out of dispatchEvent() and the remaining listeners for
    // the event are skipped.
    throws(() => target.dispatchEvent(new Event('foo')), boom);
    strictEqual(l2.mock.callCount(), 0);
  },
};

export const abortPropagates = {
  test() {
    const ac = new AbortController();
    const boom = new Error('abort boom');
    const l2 = mock.fn();
    ac.signal.addEventListener('abort', () => {
      throw boom;
    });
    ac.signal.addEventListener('abort', l2);
    throws(() => ac.abort(), boom);
    strictEqual(l2.mock.callCount(), 0);
  },
};

// A throwing 'message' listener's exception is swallowed and re-dispatched as a second
// 'message' event carrying the exception as its data. No 'messageerror' event fires, and
// the port keeps delivering afterwards.
export const messagePortLegacyRedispatch = {
  async test() {
    const { port1, port2 } = new MessageChannel();
    const boom = new Error('port boom');
    const seen = [];
    const done = Promise.withResolvers();
    const messageerror = mock.fn();
    port2.addEventListener('messageerror', messageerror);
    port2.addEventListener('message', (event) => {
      seen.push(event.data);
      if (event.data === 'bad') throw boom;
      if (event.data === 'after') done.resolve();
    });

    port1.postMessage('bad');
    port1.postMessage('after');
    await done.promise;
    deepStrictEqual(seen, ['bad', boom, 'after']);
    strictEqual(messageerror.mock.callCount(), 0);
  },
};

// A throwing 'message' listener errors out the EventSource: the 'error' event fires with
// the listener's exception and the stream closes.
export const eventSourceLegacyError = {
  async test() {
    const enc = new TextEncoder();
    const rs = new ReadableStream({
      pull(c) {
        c.enqueue(enc.encode('data: first\n\n'));
        c.close();
      },
    });
    const boom = new Error('es boom');
    const eventsource = EventSource.from(rs);
    const errorPromise = new Promise((resolve) => {
      eventsource.addEventListener('error', (event) => resolve(event.error));
    });
    eventsource.addEventListener('message', () => {
      throw boom;
    });
    strictEqual(await errorPromise, boom);
    strictEqual(eventsource.readyState, EventSource.CLOSED);
  },
};
