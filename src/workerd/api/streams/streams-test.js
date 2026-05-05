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

export const arrayBufferOfReadable = {
  async test() {
    const cs = new CompressionStream('gzip');
    const cw = cs.writable.getWriter();
    await cw.write(new TextEncoder().encode('0123456789'.repeat(1000)));
    await cw.close();
    const data = await new Response(cs.readable).arrayBuffer();
    assert.equal(66, data.byteLength);

    const ds = new DecompressionStream('gzip');
    const dw = ds.writable.getWriter();
    await dw.write(data);
    await dw.close();

    const read = await new Response(ds.readable).arrayBuffer();
    assert.equal(10_000, read.byteLength);
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
// This test simulates the production scenario where:
// 1. A TransformStream's readable is tee'd
// 2. The transform function synchronously cancels one of the tee branches
// 3. When enqueue is called, the push loop tries to push to the cancelled consumer
export const transformTeeReentrancySynchronousCancel = {
  async test() {
    let reader2;
    let cancelledBranch2 = false;

    // Create a TransformStream whose transform function cancels branch2
    const ts = new TransformStream({
      transform(chunk, controller) {
        // First time through, cancel branch2 BEFORE enqueueing
        // This simulates the production scenario where user code in the
        // transform function affects another consumer
        if (!cancelledBranch2 && reader2) {
          reader2.cancel('cancelled synchronously in transform');
          cancelledBranch2 = true;
        }
        controller.enqueue(chunk);
      },
    });

    const writer = ts.writable.getWriter();
    const [branch1, branch2] = ts.readable.tee();
    const reader1 = branch1.getReader();
    reader2 = branch2.getReader();

    // Start pending reads on both branches
    const read1Promise = reader1.read();
    const read2Promise = reader2.read();

    // Write to the transform - this triggers the transform function which:
    // 1. Cancels branch2 (closing/erroring its consumer)
    // 2. Calls controller.enqueue() which pushes to all consumers
    // Before the fix, step 2 would crash when trying to push to cancelled branch2
    await writer.write('test data');

    // Verify branch1 got the data
    const result1 = await read1Promise;
    assert.strictEqual(result1.value, 'test data');

    // branch2 was cancelled, so its read should complete (done or with data before cancel)
    const result2 = await read2Promise;
    assert.ok(result2 !== undefined);

    await writer.close();
    await reader1.cancel();
  },
};

// Test with TransformStream to match the production stack trace more closely.
// The production bug occurred during: TransformStream → enqueue → QueueImpl::push → consumer iteration
export const transformStreamTeeReentrancy = {
  async test() {
    const { readable, writable } = new TransformStream();
    const writer = writable.getWriter();

    const [branch1, branch2] = readable.tee();
    const reader1 = branch1.getReader();
    const reader2 = branch2.getReader();

    // Start pending reads on both branches
    const read1Promise = reader1.read();
    const read2Promise = reader2.read();

    // When read1 resolves, cancel branch2
    // This simulates the production scenario where a .then() handler
    // attached to the read promise cancels another branch
    read1Promise.then(() => {
      reader2.cancel('cancelled during transform');
    });

    // Write through the transform - this triggers enqueue on the readable side.
    // Before the fix, this would crash when the push loop tried to push to
    // the now-cancelled branch2 consumer.
    await writer.write('transform data');

    // Verify read1 succeeded
    const result1 = await read1Promise;
    assert.strictEqual(result1.value, 'transform data');

    // read2 may have received data or be done - either is fine
    // The important thing is no crash occurred
    const result2 = await read2Promise;
    assert.ok(result2 !== undefined);

    await writer.close();
    await reader1.cancel();
  },
};

// Test that multiple writes through a tee'd ReadableStream work correctly
// even when one branch is cancelled mid-stream
export const teeWithCancelMidStream = {
  async test() {
    let controller;
    const stream = new ReadableStream({
      start(c) {
        controller = c;
      },
    });

    const [branch1, branch2] = stream.tee();
    const reader1 = branch1.getReader();
    const reader2 = branch2.getReader();

    // Start reads on both branches
    let read1Promise = reader1.read();
    let read2Promise = reader2.read();

    // Enqueue first chunk - both branches should get it
    controller.enqueue('chunk1');
    const r1a = await read1Promise;
    const r2a = await read2Promise;
    assert.strictEqual(r1a.value, 'chunk1');
    assert.strictEqual(r2a.value, 'chunk1');

    // Now cancel branch2
    await reader2.cancel('done with branch2');

    // Start another read on branch1
    read1Promise = reader1.read();

    // Enqueue second chunk - only branch1 should get it
    // This should not crash even though branch2's consumer is now closed
    controller.enqueue('chunk2');
    const r1b = await read1Promise;
    assert.strictEqual(r1b.value, 'chunk2');

    // Start another read and enqueue third chunk to confirm continued operation
    read1Promise = reader1.read();
    controller.enqueue('chunk3');
    const r1c = await read1Promise;
    assert.strictEqual(r1c.value, 'chunk3');

    // Close and verify
    read1Promise = reader1.read();
    controller.close();
    const r1d = await read1Promise;
    assert.strictEqual(r1d.done, true);
  },
};

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

export const ResponseTextLargeBody = {
  async test() {
    const targetSize = 2147483648 + 1024 * 1024;
    let sent = 0;
    const chunkSize = 64 * 1024 * 1024;

    const stream = new ReadableStream({
      pull(controller) {
        if (sent >= targetSize) {
          controller.close();
          return;
        }
        const size = Math.min(chunkSize, targetSize - sent);
        controller.enqueue(new Uint8Array(size).fill(0x41));
        sent += size;
      },
    });

    await assert.rejects(
      new Response(stream).text(),
      (e) => e instanceof RangeError
    );
  },
};
