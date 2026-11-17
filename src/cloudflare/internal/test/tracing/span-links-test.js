// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert/strict';

const linked = {
  traceId: 'fedcba98765432100123456789abcdef',
  spanId: 'f123456789abcdef',
  traceFlags: 0xfe,
};
const zero = {
  traceId: '00000000000000000000000000000000',
  spanId: '0000000000000000',
  traceFlags: 0,
};

export const links = {
  async test(ctrl, env, ctx) {
    // Let the streaming-tail session open before producing a synchronous burst.
    await scheduler.wait(1);
    const tracing = ctx.tracing;
    const invocation = tracing.getActiveSpan();
    const invocationContext = invocation.spanContext();
    const target = tracing.startSpan('target');
    const targetContext = target.spanContext();
    target.end();

    await tracing.enterSpan('active', async (active) => {
      const receiver = tracing.startSpan('receiver');
      const before = receiver.spanContext();
      const input = {
        context: { ...linked },
        attributes: {
          message: 'original',
          number: 42,
          flag: false,
          absent: undefined,
        },
        droppedAttributesCount: 7,
      };
      assert.equal(receiver.addLink(input), receiver);
      input.context.traceId = zero.traceId;
      input.attributes.message = 'mutated';
      await Promise.resolve();
      assert.equal(
        receiver.addLinks([
          { context: targetContext },
          { context: linked },
          { context: linked },
        ]),
        receiver
      );
      assert.deepEqual(receiver.spanContext(), before);
      assert.deepEqual(
        tracing.getActiveSpan().spanContext(),
        active.spanContext()
      );
      receiver.end();
      const poison = {
        get context() {
          throw new Error('ended span read its input');
        },
      };
      assert.equal(receiver.addLink(poison), receiver);
      assert.equal(receiver.addLinks([poison]), receiver);
    });

    // Invocation end() is owned by the runtime, so it does not stop link recording.
    invocation.addLink({
      context: linked,
      attributes: { case: 'invocation-before-end' },
    });
    invocation.end();
    invocation.addLink({
      context: linked,
      attributes: { case: 'invocation-after-end' },
    });
    assert.deepEqual(invocation.spanContext(), invocationContext);

    tracing.enterSpan('counts', (span) => {
      span.addLinks(
        Array.from({ length: 129 }, (_, index) => ({
          context: linked,
          attributes: { index },
        }))
      );
      span.addLink({
        context: linked,
        attributes: Object.fromEntries(
          Array.from({ length: 129 }, (_, i) => [`key${i}`, i])
        ),
      });
    });

    tracing.enterSpan('validation', (span) => {
      span.addLinks([
        null,
        {},
        { context: 7 },
        {
          context: {
            ...linked,
            traceId: linked.traceId.toUpperCase(),
            spanId: 'F123456789ABCdef',
          },
        },
        { context: { ...linked, spanId: '123' } },
        { context: { ...linked, traceFlags: '1' } },
        {
          context: {
            ...linked,
            traceId: {
              toString() {
                throw new Error('coerced ID');
              },
            },
          },
        },
        ...[256, -1, NaN, Infinity, 0.5].map((traceFlags) => ({
          context: { ...linked, traceFlags },
        })),
        { context: zero },
        // OpenTelemetry's optional isRemote and traceState are not supported; they are ignored.
        { context: { ...zero, isRemote: true, traceState: 'vendor=state' } },
        { context: zero, attributes: { meaningful: true } },
        { context: { ...linked, isRemote: true, traceState: 'vendor=state' } },
        { context: linked, droppedAttributesCount: 2 ** 40 },
      ]);
    });

    tracing.enterSpan('batch-limit', (span) => {
      for (const length of [513, 0xffffffff]) {
        const oversized = new Array(length);
        Object.defineProperty(oversized, 0, {
          get() {
            throw new Error('oversized link arrays must not read any entries');
          },
        });
        assert.equal(span.addLinks(oversized), span);
      }

      assert.equal(
        span.addLinks(Array.from({ length: 512 }, () => ({ context: linked }))),
        span
      );
      assert.equal(span.addLinks([{ context: linked }]), span);
    });

    tracing.enterSpan('sanitization', (span) => {
      const attributes = Object.assign(
        Object.create({ inherited: 'ignored' }),
        {
          message: 'kept',
          number: 42,
          flag: false,
          absent: undefined,
          '': 'invalid key',
          bigint: 1n,
          symbol: Symbol('invalid'),
          function: () => {},
          object: {
            toString() {
              throw new Error('coerced attribute');
            },
          },
          null: null,
          array: [1, 2],
        }
      );
      Object.defineProperty(attributes, 'hidden', {
        get() {
          throw new Error('read non-enumerable attribute');
        },
      });
      Object.defineProperty(attributes, Symbol('hidden'), {
        enumerable: true,
        get() {
          throw new Error('read symbol attribute');
        },
      });
      assert.equal(
        span.addLink({
          context: linked,
          attributes,
          get droppedAttributesCount() {
            throw new Error('read supplied dropped-attribute count');
          },
        }),
        span
      );
      attributes.message = 'mutated';

      for (const attributes of [
        null,
        undefined,
        7,
        'ignored',
        true,
        Object.defineProperty(() => {}, 'ignored', {
          enumerable: true,
          get() {
            throw new Error('read callable attribute container');
          },
        }),
        [],
      ]) {
        span.addLink({ context: linked, attributes });
      }
      span.addLink({ context: linked, attributes: [1, 'two', false] });
      assert.equal(
        span.addLinks([
          { context: linked, droppedAttributesCount: 1n },
          { context: linked, attributes: { after: true } },
        ]),
        span
      );
    });

    tracing.enterSpan('getter-failure', (span) => {
      const error = new Error('attribute getter failed');
      let reachedSibling = false;
      assert.throws(
        () =>
          span.addLinks([
            { context: linked, attributes: { before: true } },
            {
              context: linked,
              attributes: {
                get value() {
                  throw error;
                },
              },
            },
            {
              get context() {
                reachedSibling = true;
                return linked;
              },
            },
          ]),
        (thrown) => thrown === error
      );
      assert.equal(reachedSibling, false);
      span.addLink({ context: linked, attributes: { after: true } });
    });

    tracing.enterSpan('budget', (span) => {
      span.addLink({ context: linked, attributes: { kept: 'before' } });
      span.addLink({
        context: linked,
        attributes: { within: 'x'.repeat(40 * 1024) },
      });
      span.addLink({
        context: linked,
        attributes: { cumulative: 'x'.repeat(40 * 1024) },
      });
      span.addLink({
        context: linked,
        attributes: { oversized: 'x'.repeat(96 * 1024) },
      });
      span.addLink({ context: linked, attributes: { kept: 'after' } });
    });

    // Input getters can end the receiver while the link is being converted.
    const ended = tracing.startSpan('ended-during-conversion');
    ended.addLinks([
      {
        get context() {
          ended.end();
          return linked;
        },
      },
    ]);
    await scheduler.wait(1);
  },
};
