// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
const requests = [];
let pending = 0;
export default {
  tailStream(onset) {
    const events = [onset];
    requests.push(events);
    pending++;
    return (event) => {
      events.push(event);
      if (event.event.type === 'outcome') pending--;
    };
  },
};
export const validate = {
  async test() {
    for (let i = 0; pending && i < 100; i++)
      await new Promise((r) => setTimeout(r, 10));
    assert.strictEqual(pending, 0);
    let roots = 0;
    let uncaught = false;
    for (const events of requests) {
      const onset = events[0];
      const opens = new Map([
        [onset.event.spanId, onset],
        ...events
          .filter((e) => e.event.type === 'spanOpen')
          .map((e) => [e.event.spanId, e]),
      ]);
      const marked = events.filter((e) => e.event.root);
      roots += marked.length;
      for (let i = 1; i < events.length; i++)
        assert(events[i].sequence > events[i - 1].sequence);
      for (const event of events) {
        const { type } = event.event;
        const ctx = event.spanContext;
        if (type === 'onset') continue;
        assert.strictEqual(ctx.traceFlags, onset.spanContext.traceFlags);
        if (event.event.root) {
          assert.strictEqual(ctx.spanId, undefined);
          assert.notStrictEqual(ctx.traceId, onset.spanContext.traceId);
          assert.strictEqual(ctx.traceFlags, onset.spanContext.traceFlags);
          assert.strictEqual(
            events.filter(
              (e) =>
                e.event.type === 'spanClose' &&
                e.spanContext.spanId === event.event.spanId
            ).length,
            1
          );
        } else assert(ctx.spanId);
        const owner = opens.get(ctx.spanId);
        if (owner) {
          assert.strictEqual(ctx.traceId, owner.spanContext.traceId);
          assert.strictEqual(ctx.traceFlags, owner.spanContext.traceFlags);
        }
        if (type === 'spanOpen') {
          const parent = opens.get(ctx.spanId);
          if (parent)
            assert.strictEqual(ctx.traceFlags, parent.spanContext.traceFlags);
        }
        if (
          type === 'exception' &&
          event.event.message.includes('uncaught-root-error')
        )
          uncaught = true;
        if (['return', 'outcome'].includes(type)) {
          assert.strictEqual(ctx.traceId, onset.spanContext.traceId);
          assert.strictEqual(ctx.spanId, onset.event.spanId);
        }
        if (type === 'log')
          for (const text of event.event.message) {
            const data = JSON.parse(text);
            if (data.marker === 'root') {
              assert.strictEqual(ctx.traceId, data.traceId);
              assert.strictEqual(ctx.spanId, data.spanId);
            }
          }
      }
      const detached = marked.find((e) => e.event.name === 'detached');
      if (detached) {
        assert(events.some((e) => e.event.type === 'spanEvent'));
        assert(events.some((e) => e.event.type === 'spanLinks'));
        assert(events.some((e) => e.event.type === 'exception'));
        const native = events.find((e) => e.event.name === 'native-child');
        assert.strictEqual(native.spanContext.spanId, detached.event.spanId);
        assert.strictEqual(
          native.spanContext.traceId,
          detached.spanContext.traceId
        );
        const bare = events.find((e) => e.event.name === 'bare-native');
        assert.strictEqual(bare.spanContext.traceId, onset.spanContext.traceId);
        assert.strictEqual(bare.spanContext.spanId, onset.event.spanId);
        assert.strictEqual(
          events.find((e) => e.event.name === 'invalid-parent').event.root,
          undefined
        );
      }
      if (
        onset.spanContext.traceFlags === 0 ||
        onset.spanContext.traceFlags === 2
      )
        assert.strictEqual(marked.length, 0);
    }
    assert(roots >= 12);
    assert.ok(uncaught);
  },
};
