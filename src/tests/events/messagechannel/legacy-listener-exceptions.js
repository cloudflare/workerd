// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Unflagged era (no spec_compliant_dispatch_exceptions): the first throwing
// 'message' listener ends the dispatch, and its exception is dispatched as
// the data of a second 'message' event. No 'messageerror' fires, and the
// port keeps delivering. If that second dispatch throws too, the exception
// escapes the delivery, whose promise reaction then rejects, unhandled.

import { deepStrictEqual, strictEqual } from 'node:assert';
import { mock } from 'node:test';
import { MessageChannel } from 'node:worker_threads';

export const legacyExceptionIsRedispatched = {
  async test() {
    const { port1, port2 } = new MessageChannel();
    const boom = new Error('port boom');
    const seen = [];
    const done = Promise.withResolvers();
    const second = mock.fn();
    const messageerror = mock.fn();
    port2.addEventListener('messageerror', messageerror);
    port2.addEventListener('message', (event) => {
      seen.push(event.data);
      if (event.data === 'bad') throw boom;
      if (event.data === 'after') done.resolve();
    });
    // Never reached for 'bad': the dispatch ended at the first listener.
    port2.addEventListener('message', (event) => second(event.data));

    port1.postMessage('bad');
    port1.postMessage('after');
    await done.promise;
    deepStrictEqual(seen, ['bad', boom, 'after']);
    deepStrictEqual(
      second.mock.calls.map((call) => call.arguments[0]),
      [boom, 'after']
    );
    strictEqual(messageerror.mock.callCount(), 0);
    port1.close();
  },
};

export const legacyEscapingExceptionIsAnUnhandledRejection = {
  async test() {
    const { port1, port2 } = new MessageChannel();
    const first = new Error('first');
    const second = new Error('second');
    const seen = [];
    port2.addEventListener('message', (event) => {
      seen.push(event.data);
      throw event.data === 'go' ? first : second;
    });
    const { promise, resolve } = Promise.withResolvers();
    const onRejection = (event) => {
      event.preventDefault();
      resolve(event.reason);
    };
    addEventListener('unhandledrejection', onRejection);
    try {
      port1.postMessage('go');
      strictEqual(await promise, second);
      deepStrictEqual(seen, ['go', first]);
    } finally {
      removeEventListener('unhandledrejection', onRejection);
      port1.close();
    }
  },
};
