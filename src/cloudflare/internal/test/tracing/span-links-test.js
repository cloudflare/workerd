// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert/strict';

const remote = {
  traceId: 'fedcba98765432100123456789abcdef',
  spanId: 'f123456789abcdef',
  traceFlags: 0xfe,
  isRemote: true,
  traceState: 'vendor=state',
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
        context: { ...remote },
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
          { context: remote },
          { context: remote },
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
      context: remote,
      attributes: { case: 'invocation-before-end' },
    });
    invocation.end();
    invocation.addLink({
      context: remote,
      attributes: { case: 'invocation-after-end' },
    });
    assert.deepEqual(invocation.spanContext(), invocationContext);

    tracing.enterSpan('counts', (span) => {
      span.addLinks(
        Array.from({ length: 129 }, (_, index) => ({
          context: remote,
          attributes: { index },
        }))
      );
      span.addLink({
        context: remote,
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
        { context: { ...remote, traceId: remote.traceId.toUpperCase() } },
        { context: { ...remote, spanId: '123' } },
        { context: { ...remote, traceFlags: '1' } },
        {
          context: {
            ...remote,
            traceId: {
              toString() {
                throw new Error('coerced ID');
              },
            },
          },
        },
        ...[256, -1, NaN, Infinity, 0.5].map((traceFlags) => ({
          context: { ...remote, traceFlags },
        })),
        { context: zero },
        { context: { ...zero, traceState: 'invalid' } },
        { context: zero, attributes: { meaningful: true } },
        { context: { ...zero, traceState: 'a=state' } },
        { context: { ...remote, traceState: 'a=one,a=two', isRemote: false } },
        { context: remote, droppedAttributesCount: 2 ** 40 },
      ]);
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
          context: remote,
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
        span.addLink({ context: remote, attributes });
      }
      span.addLink({ context: remote, attributes: [1, 'two', false] });
      assert.equal(
        span.addLinks([
          { context: remote, droppedAttributesCount: 1n },
          { context: remote, attributes: { after: true } },
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
            { context: remote, attributes: { before: true } },
            {
              context: remote,
              attributes: {
                get value() {
                  throw error;
                },
              },
            },
            {
              get context() {
                reachedSibling = true;
                return remote;
              },
            },
          ]),
        (thrown) => thrown === error
      );
      assert.equal(reachedSibling, false);
      span.addLink({ context: remote, attributes: { after: true } });
    });

    tracing.enterSpan('budget', (span) => {
      span.addLink({ context: remote, attributes: { kept: 'before' } });
      span.addLink({
        context: remote,
        attributes: { within: 'x'.repeat(40 * 1024) },
      });
      span.addLink({
        context: remote,
        attributes: { cumulative: 'x'.repeat(40 * 1024) },
      });
      span.addLink({
        context: remote,
        attributes: { oversized: 'x'.repeat(96 * 1024) },
      });
      span.addLink({ context: remote, attributes: { kept: 'after' } });
    });

    // Input getters can end the receiver while the link is being converted.
    const ended = tracing.startSpan('ended-during-conversion');
    ended.addLinks([
      {
        get context() {
          ended.end();
          return remote;
        },
      },
    ]);
    await scheduler.wait(1);
  },
};
