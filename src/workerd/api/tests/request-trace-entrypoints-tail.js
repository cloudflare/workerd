// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';

const events = [];
export default {
  tailStream(onset) {
    events.push(onset);
    return (event) => {
      events.push(event);
    };
  },
};

export const validate = {
  async test() {
    for (
      let i = 0;
      i < 100 && events.filter((e) => e.event.type === 'spanClose').length < 4;
      i++
    ) {
      await new Promise((resolve) => setTimeout(resolve, 10));
    }
    for (const name of [
      'entry-constructor',
      'entry-block',
      'entry-message',
      'entry-close',
    ]) {
      const open = events.find(
        (e) => e.event.type === 'spanOpen' && e.event.name === name
      );
      assert.ok(open, name);
      const onset = events.find(
        (e) => e.invocationId === open.invocationId && e.event.type === 'onset'
      );
      assert.strictEqual(open.spanContext.traceId, onset.spanContext.traceId);
      assert.strictEqual(open.spanContext.spanId, onset.event.spanId);
      const log = events.find(
        (e) =>
          e.invocationId === open.invocationId &&
          e.event.type === 'log' &&
          JSON.stringify(e.event.message).includes(name)
      );
      assert.ok(log, name);
      assert.strictEqual(log.spanContext.traceId, onset.spanContext.traceId);
      assert.strictEqual(log.spanContext.spanId, onset.event.spanId);
      const values =
        typeof log.event.message === 'string'
          ? JSON.parse(log.event.message)
          : log.event.message;
      const identity = JSON.parse(values[0]);
      assert.strictEqual(identity.traceId, onset.spanContext.traceId);
      assert.strictEqual(identity.spanId, onset.event.spanId);
    }
  },
};
