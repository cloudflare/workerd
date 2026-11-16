// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// util.inspect() formats an EventSource as a JSG resource type: its
// prototype accessors' values, except function-valued ones.

import { strictEqual } from 'node:assert';
import { inspect } from 'node:util';
import { streamOf } from 'helpers';

const oneLine = { breakLength: Infinity };

export const inspectInstance = {
  test() {
    const source = EventSource.from(streamOf([], { close: false }));
    strictEqual(
      inspect(source, oneLine),
      "EventSource { url: '', withCredentials: false, readyState: 1, " +
        'onopen: null, onmessage: null, onerror: null }'
    );
    source.onmessage = function handler() {};
    source.close();
    strictEqual(
      inspect(source, oneLine),
      "EventSource { url: '', withCredentials: false, readyState: 2, " +
        'onopen: null, onerror: null }'
    );
  },
};

export const inspectPrototype = {
  test() {
    strictEqual(
      inspect(EventSource.prototype, oneLine).replace(
        'EventTarget [EventSource]',
        'EventTarget'
      ),
      'EventTarget { close: [Function: close], url: [Getter], ' +
        'withCredentials: [Getter], readyState: [Getter], ' +
        'onopen: [Getter/Setter], onmessage: [Getter/Setter], ' +
        'onerror: [Getter/Setter], CONNECTING: 0, OPEN: 1, CLOSED: 2 }'
    );
  },
};
