// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as assert from 'node:assert';
import * as util from 'node:util';

export const partiallyReadStream = {
  async test(ctrl, env, ctx) {
    const enc = new TextEncoder();
    const rs = new ReadableStream({
      type: 'bytes',
      start(controller) {
        controller.enqueue(enc.encode('hello'));
        controller.enqueue(enc.encode('world'));
        controller.close();
      },
    });
    const reader = rs.getReader({ mode: 'byob' });
    await reader.read(new Uint8Array(5));
    reader.releaseLock();

    // Should not throw!
    await env.KV.put('key', rs);
  },
};

export const inspect = {
  async test() {
    const inspectOpts = { breakLength: Infinity };

    // Check with JavaScript regular ReadableStream
    {
      let pulls = 0;
      const readableStream = new ReadableStream({
        pull(controller) {
          if (pulls === 0) controller.enqueue('hello');
          if (pulls === 1) controller.close();
          pulls++;
        },
      });
      assert.strictEqual(
        util.inspect(readableStream, inspectOpts),
        "ReadableStream { locked: false, [state]: 'readable', [supportsBYOB]: false, [length]: undefined }"
      );

      const reader = readableStream.getReader();
      assert.strictEqual(
        util.inspect(readableStream, inspectOpts),
        "ReadableStream { locked: true, [state]: 'readable', [supportsBYOB]: false, [length]: undefined }"
      );

      await reader.read();
      assert.strictEqual(
        util.inspect(readableStream, inspectOpts),
        "ReadableStream { locked: true, [state]: 'readable', [supportsBYOB]: false, [length]: undefined }"
      );

      await reader.read();
      assert.strictEqual(
        util.inspect(readableStream, inspectOpts),
        "ReadableStream { locked: true, [state]: 'closed', [supportsBYOB]: false, [length]: undefined }"
      );
    }

    // Check with errored JavaScript regular ReadableStream
    {
      const readableStream = new ReadableStream({
        start(controller) {
          controller.error(new Error('Oops!'));
        },
      });
      assert.strictEqual(
        util.inspect(readableStream, inspectOpts),
        "ReadableStream { locked: false, [state]: 'errored', [supportsBYOB]: false, [length]: undefined }"
      );
    }

    // Check with JavaScript bytes ReadableStream
    {
      const readableStream = new ReadableStream({
        type: 'bytes',
        pull(controller) {
          controller.enqueue(new Uint8Array([1]));
        },
      });
      assert.strictEqual(
        util.inspect(readableStream, inspectOpts),
        "ReadableStream { locked: false, [state]: 'readable', [supportsBYOB]: true, [length]: undefined }"
      );
    }

    // Check with JavaScript WritableStream
    {
      const writableStream = new WritableStream({
        write(chunk, controller) {},
      });
      assert.strictEqual(
        util.inspect(writableStream, inspectOpts),
        "WritableStream { locked: false, [state]: 'writable', [expectsBytes]: false }"
      );

      const writer = writableStream.getWriter();
      assert.strictEqual(
        util.inspect(writableStream, inspectOpts),
        "WritableStream { locked: true, [state]: 'writable', [expectsBytes]: false }"
      );

      await writer.write('chunk');
      assert.strictEqual(
        util.inspect(writableStream, inspectOpts),
        "WritableStream { locked: true, [state]: 'writable', [expectsBytes]: false }"
      );

      await writer.close();
      assert.strictEqual(
        util.inspect(writableStream, inspectOpts),
        "WritableStream { locked: true, [state]: 'closed', [expectsBytes]: false }"
      );
    }

    // Check with errored JavaScript WritableStream
    {
      const writableStream = new WritableStream({
        write(chunk, controller) {
          controller.error(new Error('Oops!'));
        },
      });
      assert.strictEqual(
        util.inspect(writableStream, inspectOpts),
        "WritableStream { locked: false, [state]: 'writable', [expectsBytes]: false }"
      );

      const writer = writableStream.getWriter();
      const promise = writer.write('chunk');
      assert.strictEqual(
        util.inspect(writableStream, inspectOpts),
        "WritableStream { locked: true, [state]: 'erroring', [expectsBytes]: false }"
      );

      await promise;
      assert.strictEqual(
        util.inspect(writableStream, inspectOpts),
        "WritableStream { locked: true, [state]: 'errored', [expectsBytes]: false }"
      );
    }

    // Check with internal known-length TransformStream
    {
      const inspectOpts = { breakLength: 100 };
      const transformStream = new FixedLengthStream(5);
      assert.strictEqual(
        util.inspect(transformStream, inspectOpts),
        `FixedLengthStream {
  readable: ReadableStream { locked: false, [state]: 'readable', [supportsBYOB]: true, [length]: 5n },
  writable: WritableStream { locked: false, [state]: 'writable', [expectsBytes]: true }
}`
      );

      const { writable, readable } = transformStream;
      const writer = writable.getWriter();
      assert.strictEqual(
        util.inspect(transformStream, inspectOpts),
        `FixedLengthStream {
  readable: ReadableStream { locked: false, [state]: 'readable', [supportsBYOB]: true, [length]: 5n },
  writable: WritableStream { locked: true, [state]: 'writable', [expectsBytes]: true }
}`
      );

      void writer.write(new Uint8Array([1, 2, 3]));
      void writer.write(new Uint8Array([4, 5]));
      assert.strictEqual(
        util.inspect(transformStream, inspectOpts),
        `FixedLengthStream {
  readable: ReadableStream { locked: false, [state]: 'readable', [supportsBYOB]: true, [length]: 5n },
  writable: WritableStream { locked: true, [state]: 'writable', [expectsBytes]: true }
}`
      );

      void writer.close();
      assert.strictEqual(
        util.inspect(transformStream, inspectOpts),
        `FixedLengthStream {
  readable: ReadableStream { locked: false, [state]: 'readable', [supportsBYOB]: true, [length]: 5n },
  writable: WritableStream { locked: true, [state]: 'closed', [expectsBytes]: true }
}`
      );

      const reader = readable.getReader();
      assert.strictEqual(
        util.inspect(transformStream, inspectOpts),
        `FixedLengthStream {
  readable: ReadableStream { locked: true, [state]: 'readable', [supportsBYOB]: true, [length]: 5n },
  writable: WritableStream { locked: true, [state]: 'closed', [expectsBytes]: true }
}`
      );

      await reader.read();
      assert.strictEqual(
        util.inspect(transformStream, inspectOpts),
        `FixedLengthStream {
  readable: ReadableStream { locked: true, [state]: 'readable', [supportsBYOB]: true, [length]: 2n },
  writable: WritableStream { locked: true, [state]: 'closed', [expectsBytes]: true }
}`
      );

      await reader.read();
      assert.strictEqual(
        util.inspect(transformStream, inspectOpts),
        `FixedLengthStream {
  readable: ReadableStream { locked: true, [state]: 'readable', [supportsBYOB]: true, [length]: 0n },
  writable: WritableStream { locked: true, [state]: 'closed', [expectsBytes]: true }
}`
      );

      await reader.read();
      assert.strictEqual(
        util.inspect(transformStream, inspectOpts),
        `FixedLengthStream {
  readable: ReadableStream { locked: true, [state]: 'closed', [supportsBYOB]: true, [length]: 0n },
  writable: WritableStream { locked: true, [state]: 'closed', [expectsBytes]: true }
}`
      );
    }

    // Check with errored internal TransformStream
    {
      const inspectOpts = { breakLength: 100 };
      const transformStream = new IdentityTransformStream();
      assert.strictEqual(
        util.inspect(transformStream, inspectOpts),
        `IdentityTransformStream {
  readable: ReadableStream { locked: false, [state]: 'readable', [supportsBYOB]: true, [length]: undefined },
  writable: WritableStream { locked: false, [state]: 'writable', [expectsBytes]: true }
}`
      );

      const { writable, readable } = transformStream;
      const writer = writable.getWriter();
      void writer.abort(new Error('Oops!'));
      assert.strictEqual(
        util.inspect(transformStream, inspectOpts),
        `IdentityTransformStream {
  readable: ReadableStream { locked: false, [state]: 'readable', [supportsBYOB]: true, [length]: undefined },
  writable: WritableStream { locked: true, [state]: 'errored', [expectsBytes]: true }
}`
      );

      const reader = readable.getReader();
      assert.strictEqual(
        util.inspect(transformStream, inspectOpts),
        `IdentityTransformStream {
  readable: ReadableStream { locked: true, [state]: 'readable', [supportsBYOB]: true, [length]: undefined },
  writable: WritableStream { locked: true, [state]: 'errored', [expectsBytes]: true }
}`
      );

      await reader.read().catch(() => {});
      assert.strictEqual(
        util.inspect(transformStream, inspectOpts),
        `IdentityTransformStream {
  readable: ReadableStream { locked: true, [state]: 'errored', [supportsBYOB]: true, [length]: undefined },
  writable: WritableStream { locked: true, [state]: 'errored', [expectsBytes]: true }
}`
      );
    }
  },
};

// Test for re-entrancy bug: when pushing to multiple consumers (via tee),
// the transform function can directly cancel another consumer synchronously.
// This should not crash - the cancelled consumer should gracefully ignore the push.
// Before the fix, this would crash with:
// "expected state.template tryGet<Ready>() != nullptr; The consumer is either closed or errored."
//
// ============================================================================

export const testCancelPipethrough = {
  async test() {
    const enc = new TextEncoder();
    const transform = new IdentityTransformStream();
    const rs = new ReadableStream({
      start(c) {
        c.enqueue(enc.encode('hello'));
      },
    });
    const readable = rs.pipeThrough(transform);

    const reader = readable.getReader();

    assert.ok(rs.locked);
    assert.ok(transform.writable.locked);

    reader.cancel(new Error('boom'));
    reader.releaseLock();

    // We've got to wait a tick to allow the cancel to propagate
    await scheduler.wait(1);

    assert.ok(!rs.locked);
    assert.ok(!transform.readable.locked);
    assert.ok(!transform.writable.locked);

    // Our JavaScript ReadableStream should be closed (not errored).
    // Cancel propagates back and closes the source stream.
    const reader2 = rs.getReader();
    const result = await reader2.read();
    assert.ok(result.done);
    assert.strictEqual(result.value, undefined);
  },
};

// Same as testCancelPipethrough but uses a JavaScript-backed TransformStream
// instead of IdentityTransformStream. The behavior should be the same.
export const testCancelPipethrough2 = {
  async test() {
    const enc = new TextEncoder();
    const transform = new TransformStream();
    const rs = new ReadableStream({
      start(c) {
        c.enqueue(enc.encode('hello'));
      },
    });
    const readable = rs.pipeThrough(transform);

    const reader = readable.getReader();

    assert.ok(rs.locked);
    assert.ok(transform.writable.locked);

    reader.cancel(new Error('boom'));
    reader.releaseLock();

    // We've got to wait a tick to allow the cancel to propagate
    await scheduler.wait(1);

    assert.ok(!rs.locked);
    assert.ok(!transform.readable.locked);
    assert.ok(!transform.writable.locked);

    // Our JavaScript ReadableStream should be closed (not errored).
    // Cancel propagates back and closes the source stream.
    const reader2 = rs.getReader();
    const result = await reader2.read();
    assert.ok(result.done);
    assert.strictEqual(result.value, undefined);
  },
};
