// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The request an EventSource sends.

import { deepStrictEqual, strictEqual } from 'node:assert';
import { connect, nextEvent, record } from 'helpers';

export const requestHeaders = {
  async test(ctrl, env) {
    const source = connect(env, '/request-headers');
    const event = await nextEvent(source, 'message');
    strictEqual(event.data, 'text/event-stream|no-cache|null');
    source.close();
  },
};

// Without a fetcher, the request goes out as the global fetch() sends it;
// a rejected fetch closes the EventSource without reconnecting.
export const failedFetchCloses = {
  async test() {
    const source = new EventSource('http://127.0.0.1:1/');
    const events = record(source);
    await nextEvent(source, 'error');
    await scheduler.wait(10);
    deepStrictEqual(
      events.map(([type, readyState]) => [type, readyState]),
      [['error', EventSource.CLOSED]]
    );
  },
};
