// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Under spec_compliant_dispatch_exceptions, a throwing 'message' listener
// has its exception reported while the remaining listeners still run, and
// the port then dispatches a 'messageerror' event carrying the exception.
// The port keeps working. (legacy-listener-exceptions.js pins the
// unflagged behavior.)

import { deepStrictEqual, strictEqual } from 'node:assert';

export const throwingMessageListener = {
  async test() {
    const order = [];
    const boom = new Error('port boom');
    const { port1, port2 } = new MessageChannel();
    const globalHandler = (event) => {
      order.push('global-error');
      strictEqual(event.error, boom);
      // Injecting a message mid-report cannot jump the queue: delivery is
      // deferred, so it arrives after the current event's remaining
      // listeners, after the messageerror, and after any messages queued
      // before it.
      port1.postMessage('injected');
    };
    globalThis.addEventListener('error', globalHandler);
    try {
      const done = Promise.withResolvers();
      port2.addEventListener('message', (event) => {
        order.push(`l1:${event.data}`);
        if (event.data === 'bad') throw boom;
        if (event.data === 'injected') done.resolve();
      });
      port2.addEventListener('message', (event) =>
        order.push(`l2:${event.data}`)
      );
      port2.addEventListener('messageerror', (event) => {
        order.push('messageerror');
        strictEqual(event.data, boom);
        strictEqual(event.isTrusted, true);
        strictEqual(event.source, port2);
      });

      port1.postMessage('bad');
      port1.postMessage('after');
      await done.promise;
      deepStrictEqual(order, [
        'l1:bad',
        'global-error',
        'l2:bad',
        'messageerror',
        'l1:after',
        'l2:after',
        'l1:injected',
        'l2:injected',
      ]);
    } finally {
      globalThis.removeEventListener('error', globalHandler);
      port1.close();
    }
  },
};

// A throwing 'messageerror' listener is reported too, and triggers nothing
// further.
export const throwingMessageErrorListener = {
  async test() {
    const { port1, port2 } = new MessageChannel();
    const reported = [];
    const globalHandler = (event) => {
      reported.push(event.error.message);
      event.preventDefault();
    };
    globalThis.addEventListener('error', globalHandler);
    try {
      const done = Promise.withResolvers();
      port2.addEventListener('message', (event) => {
        if (event.data === 'bad') throw new Error('message');
        done.resolve();
      });
      port2.addEventListener('messageerror', () => {
        throw new Error('messageerror');
      });
      port1.postMessage('bad');
      port1.postMessage('after');
      await done.promise;
      deepStrictEqual(reported, ['message', 'messageerror']);
    } finally {
      globalThis.removeEventListener('error', globalHandler);
      port1.close();
    }
  },
};
