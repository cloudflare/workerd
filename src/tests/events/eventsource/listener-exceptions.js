// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Listener exceptions under spec_compliant_dispatch_exceptions. A throwing
// 'open' or 'message' listener's exception is reported, the remaining
// listeners run, and then the EventSource errors out with it, dropping the
// rest of the messages. A throwing 'error' listener's exception is only
// reported.

import { deepStrictEqual, strictEqual } from 'node:assert';
import { connect, nextEvent, streamOf } from 'helpers';

// Collects the exceptions reported to the global scope while `fn` runs.
async function withReported(fn) {
  const reported = [];
  const listener = (event) => reported.push(event.error);
  addEventListener('error', listener);
  try {
    await fn(reported);
  } finally {
    removeEventListener('error', listener);
  }
  return reported;
}

export const throwingMessageListener = {
  async test() {
    const boom = new Error('boom');
    const order = [];
    const reported = await withReported(async (seen) => {
      const source = EventSource.from(
        streamOf(['data: first\n\ndata: second\n\n'])
      );
      source.addEventListener('message', (event) => {
        order.push(`l1:${event.data}`);
        throw boom;
      });
      source.addEventListener('message', (event) => {
        order.push(`l2:${event.data}:${source.readyState}:${seen.length}`);
      });
      const error = await nextEvent(source, 'error');
      strictEqual(error.error, boom);
      strictEqual(source.readyState, EventSource.CLOSED);
    });
    deepStrictEqual(order, ['l1:first', `l2:first:${EventSource.OPEN}:1`]);
    deepStrictEqual(reported, [boom]);
  },
};

export const throwingOpenListener = {
  async test(ctrl, env) {
    const boom = new Error('boom');
    const reported = await withReported(async () => {
      const source = connect(env, '/request-headers');
      source.onopen = () => {
        throw boom;
      };
      let messages = 0;
      source.onmessage = () => messages++;
      const error = await nextEvent(source, 'error');
      strictEqual(error.error, boom);
      strictEqual(source.readyState, EventSource.CLOSED);
      await scheduler.wait(10);
      strictEqual(messages, 0);
    });
    deepStrictEqual(reported, [boom]);
  },
};

export const throwingErrorListener = {
  async test() {
    const boom = new Error('boom');
    let later = false;
    const reported = await withReported(async () => {
      const source = EventSource.from(streamOf([]));
      source.addEventListener('error', () => {
        throw boom;
      });
      source.addEventListener('error', () => (later = true));
      await nextEvent(source, 'error');
      strictEqual(source.readyState, EventSource.CLOSED);
    });
    strictEqual(later, true);
    deepStrictEqual(reported, [boom]);
  },
};
