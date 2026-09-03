// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Transformer hook invocation: start/transform/flush ordering, sync and
// async hooks, and chunk-type freedom. Migrated from
// transform-streams-test.js.

import { strictEqual, ok } from 'node:assert';
import { consume, consumeBytes } from 'helpers';

// start/transform/flush enqueues arrive in order.
export const simpleTransform = {
  async test() {
    const transform = new TransformStream({
      start(controller) {
        controller.enqueue('<');
      },
      transform(value, controller) {
        controller.enqueue(value.toUpperCase());
      },
      flush(controller) {
        controller.enqueue('>');
      },
    });
    ok(!(transform instanceof IdentityTransformStream));

    const { readable, writable } = transform;

    const writer = writable.getWriter();

    const res = await Promise.allSettled([
      writer.write('hello'),
      writer.write('there'),
      writer.close(),
      consume(readable),
    ]);

    strictEqual(res[3].value, '<HELLOTHERE>');
  },
};

// Async hooks are awaited; output order is unchanged.
export const delayTransform = {
  async test() {
    const transform = new TransformStream({
      async start(controller) {
        await scheduler.wait(1);
        controller.enqueue('<');
      },
      async transform(value, controller) {
        await scheduler.wait(1);
        controller.enqueue(value.toUpperCase());
      },
      async flush(controller) {
        await scheduler.wait(1);
        controller.enqueue('>');
      },
    });
    ok(!(transform instanceof IdentityTransformStream));

    const { readable, writable } = transform;

    const writer = writable.getWriter();

    const res = await Promise.allSettled([
      writer.write('hello'),
      writer.write('there'),
      writer.close(),
      consume(readable),
    ]);

    strictEqual(res[3].value, '<HELLOTHERE>');
  },
};

// The writable and readable sides may carry different chunk types.
export const differentTypesTransform = {
  async test() {
    const enc = new TextEncoder();

    const transform = new TransformStream({
      async start(controller) {
        await scheduler.wait(1);
        controller.enqueue(enc.encode('<'));
      },
      async transform(value, controller) {
        await scheduler.wait(1);
        controller.enqueue(enc.encode(value.toUpperCase()));
      },
      async flush(controller) {
        await scheduler.wait(1);
        controller.enqueue(enc.encode('>'));
      },
    });
    ok(!(transform instanceof IdentityTransformStream));

    const { readable, writable } = transform;

    const writer = writable.getWriter();

    const res = await Promise.allSettled([
      writer.write('hello'),
      writer.write('there'),
      writer.close(),
      consumeBytes(readable),
    ]);

    strictEqual(res[3].value, '<HELLOTHERE>');
  },
};

// Hook invocation shape: start/transform/flush receive 1/2/1 arguments
// with the transformer itself as the receiver — including when the hooks
// live on the prototype chain (parity; WPT properties.any's C++
// expectedFailures do not reproduce under direct observation).
export const hookInvocationShape = {
  async test() {
    const counts = {};
    const recv = {};
    const transformer = {
      start(...a) {
        counts.start = a.length;
        recv.start = this === transformer;
      },
      transform(...a) {
        counts.transform = a.length;
        recv.transform = this === transformer;
        a[1].enqueue(a[0]);
      },
      flush(...a) {
        counts.flush = a.length;
        recv.flush = this === transformer;
      },
    };
    const ts = new TransformStream(transformer);
    const writer = ts.writable.getWriter();
    const reader = ts.readable.getReader();
    await Promise.allSettled([writer.write('x'), reader.read()]);
    await writer.close();

    strictEqual(counts.start, 1);
    strictEqual(counts.transform, 2);
    strictEqual(counts.flush, 1);
    strictEqual(recv.start, true);
    strictEqual(recv.transform, true);
    strictEqual(recv.flush, true);
  },
};

// Hooks found on the transformer's prototype chain are used (parity).
export const prototypeChainTransformer = {
  async test() {
    const seen = [];
    const proto = {
      start() {
        seen.push('start');
      },
      transform(chunk, controller) {
        seen.push('transform');
        controller.enqueue(chunk);
      },
      flush() {
        seen.push('flush');
      },
    };
    const transformer = Object.create(proto);
    const ts = new TransformStream(transformer);
    const writer = ts.writable.getWriter();
    const reader = ts.readable.getReader();
    const [, r] = await Promise.all([writer.write('x'), reader.read()]);
    strictEqual(r.value, 'x');
    await writer.close();
    strictEqual(seen.join(','), 'start,transform,flush');
  },
};
