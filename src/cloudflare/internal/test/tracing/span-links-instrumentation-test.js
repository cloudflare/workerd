// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert/strict';

const invocations = [];
const completed = [];
const started = Promise.withResolvers();
const attrs = (link) =>
  Object.fromEntries(link.attributes.map(({ name, value }) => [name, value]));

export default {
  tailStream(onset) {
    const spans = new Map();
    const root = { links: [], spanId: onset.event.spanId };
    spans.set(root.spanId, root);
    invocations.push(spans);
    let finish;
    completed.push(
      new Promise((resolve) => {
        finish = resolve;
      })
    );
    started.resolve();
    // Exercise object-handler dispatch as well as the event's JS serialization.
    return {
      spanOpen(event) {
        spans.set(event.event.spanId, {
          spanId: event.event.spanId,
          name: event.event.name,
          parent: event.spanContext.spanId,
          links: [],
        });
      },
      spanLinks(event) {
        const span = spans.get(event.spanContext.spanId);
        assert.ok(span, 'links must target an opened span');
        span.links.push(...event.event.links);
      },
      outcome() {
        finish();
      },
    };
  },
};

export const validate = {
  async test() {
    await scheduler.wait(1);
    await started.promise;
    await Promise.all(completed);
    assert.equal(invocations.length, 1);
    const spans = [...invocations[0].values()];
    const named = (name) => {
      const span = spans.find((span) => span.name === name);
      assert.ok(span, `missing span ${name}`);
      return span;
    };
    const root = spans.find((span) => !span.name);
    const receiver = named('receiver');
    assert.equal(receiver.parent, named('active').spanId);
    assert.equal(named('active').parent, root.spanId);
    assert.equal(named('active').links.length, 0);
    assert.equal(receiver.links.length, 4);
    const [first, sameTrace, duplicate1, duplicate2] = receiver.links;
    assert.deepEqual(first.context, {
      traceId: 'fedcba98765432100123456789abcdef',
      spanId: 'f123456789abcdef',
      traceFlags: 0xfe,
      isRemote: true,
      traceState: 'vendor=state',
    });
    assert.deepEqual(attrs(first), {
      message: 'original',
      number: 42,
      flag: false,
    });
    assert.equal(first.droppedAttributesCount, 0);
    assert.equal(sameTrace.context.spanId, named('target').spanId);
    assert.equal('isRemote' in sameTrace.context, false);
    assert.deepEqual(duplicate1, duplicate2);
    assert.deepEqual(
      root.links.map((link) => attrs(link).case),
      ['invocation-before-end', 'invocation-after-end']
    );

    const counts = named('counts').links;
    assert.equal(counts.length, 130);
    assert.deepEqual(
      counts.slice(0, 129).map((link) => attrs(link).index),
      Array.from({ length: 129 }, (_, i) => i)
    );
    assert.equal(counts[129].attributes.length, 129);
    assert.equal(attrs(counts[129]).key128, 128);

    const validated = named('validation').links;
    assert.equal(validated.length, 4);
    assert.equal(validated[0].context.traceId, '0'.repeat(32));
    assert.equal(validated[0].context.spanId, '0'.repeat(16));
    assert.equal(attrs(validated[0]).meaningful, true);
    assert.equal(validated[1].context.traceState, 'a=state');
    assert.equal(validated[2].context.isRemote, false);
    assert.equal('traceState' in validated[2].context, false);
    assert.equal(validated[3].droppedAttributesCount, 0);

    assert.equal(named('batch-limit').links.length, 513);

    const sanitized = named('sanitization').links;
    assert.equal(sanitized.length, 11);
    assert.deepEqual(attrs(sanitized[0]), {
      message: 'kept',
      number: 42,
      flag: false,
    });
    assert.equal(sanitized[0].droppedAttributesCount, 7);
    for (const link of sanitized.slice(1, 8)) {
      assert.deepEqual(attrs(link), {});
      assert.equal(link.droppedAttributesCount, 0);
    }
    assert.deepEqual(attrs(sanitized[8]), { 0: 1, 1: 'two', 2: false });
    assert.equal(sanitized[8].droppedAttributesCount, 0);
    assert.equal(sanitized[9].droppedAttributesCount, 0);
    assert.deepEqual(attrs(sanitized[10]), { after: true });
    assert.deepEqual(named('getter-failure').links.map(attrs), [
      { before: true },
      { after: true },
    ]);

    const budget = named('budget').links;
    assert.equal(budget.length, 3);
    assert.equal(attrs(budget[0]).kept, 'before');
    assert.equal(attrs(budget[1]).within.length, 40 * 1024);
    assert.equal(attrs(budget[2]).kept, 'after');
    assert.equal(named('ended-during-conversion').links.length, 0);
  },
};
