// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert/strict';

const invocations = [];

export default {
  tailStream() {
    const { promise: ended, resolve } = Promise.withResolvers();
    // Retain raw events to validate ordering without throwing from the tail handler.
    const events = [];
    invocations.push({ ended, events });
    return (item) => {
      events.push(item);
      if (item.event.type === 'outcome') resolve();
    };
  },
};

export const validate = {
  async test() {
    await Promise.all(invocations.map(({ ended }) => ended));
    const spans = invocations.flatMap(({ events }) => {
      const spans = new Map();
      for (const { event, spanContext } of events) {
        if (event.type === 'spanOpen') {
          spans.set(event.spanId, {
            name: event.name,
            attributes: {},
            closed: false,
          });
        } else {
          const span = spans.get(spanContext.spanId);
          if (!span) continue;
          assert.equal(
            span.closed,
            false,
            `${event.type} arrived after spanClose`
          );
          if (event.type === 'attributes') {
            for (const { name, value } of event.info)
              span.attributes[name] = value;
          } else if (event.type === 'spanClose') {
            span.closed = true;
          }
        }
      }
      return [...spans.values()].filter(({ name }) => name === 'fetch');
    });
    assert.equal(spans.length, 5);
    assert.deepEqual(
      spans.map(({ attributes }) => attributes['url.full']).sort(),
      [
        '/relative?q=1#f',
        'http://origin/start?hop=1',
        'http://origin/middle?hop=2',
        'http://other/final?hop=3',
        'http://REDACTED:REDACTED@origin/x#f',
      ].sort()
    );
    assert.ok(spans.every(({ closed }) => closed));
  },
};
