// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Without spec_compliant_dispatch_exceptions, the first throwing 'message'
// listener ends the dispatch and errors the EventSource out with its
// exception, which is not reported.

import { deepStrictEqual, strictEqual } from 'node:assert';
import { nextEvent, streamOf } from 'helpers';

export const legacyThrowingMessageListener = {
  async test() {
    const boom = new Error('boom');
    const order = [];
    const reported = [];
    const listener = (event) => reported.push(event.error);
    addEventListener('error', listener);
    try {
      const source = EventSource.from(
        streamOf(['data: first\n\ndata: second\n\n'])
      );
      source.addEventListener('message', (event) => {
        order.push(`l1:${event.data}`);
        throw boom;
      });
      source.addEventListener('message', (event) => {
        order.push(`l2:${event.data}`);
      });
      const error = await nextEvent(source, 'error');
      strictEqual(error.error, boom);
      strictEqual(source.readyState, EventSource.CLOSED);
    } finally {
      removeEventListener('error', listener);
    }
    deepStrictEqual(order, ['l1:first']);
    deepStrictEqual(reported, []);
  },
};
