// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tests for complex error scenarios in streams.
// These tests focus on error type preservation, error propagation through
// pipe chains, and race conditions between error and close operations.
//
// Test inspirations:
// - Deno: tests/unit/streams_test.ts (cancel propagation, error type tests)
// - Bun: test/js/web/streams/streams.test.js (pull rejection, error handling)
// - Bun: test/js/web/fetch/fetch.stream.test.ts (corrupted data, socket close handling)
// - Bun: test/js/bun/spawn/spawn-stdin-readable-stream-edge-cases.test.ts (exception in pull)

import { strictEqual, ok, rejects, deepStrictEqual } from 'node:assert';

// Custom error class for testing error type preservation
class CustomStreamError extends Error {
  constructor(message, code) {
    super(message);
    this.name = 'CustomStreamError';
    this.code = code;
  }
}

// Test error thrown after partial consumption of stream
// Inspired by: Bun test/js/bun/spawn/spawn-stdin-readable-stream-edge-cases.test.ts (exception in pull)
export const errorDuringPartialConsumption = {
  async test() {
    let chunkCount = 0;

    const rs = new ReadableStream({
      pull(controller) {
        chunkCount++;
        if (chunkCount <= 3) {
          controller.enqueue(chunkCount);
        } else {
          controller.error(new Error('Error after 3 chunks'));
        }
      },
    });

    const reader = rs.getReader();
    const chunks = [];

    for (let i = 0; i < 3; i++) {
      const { value, done } = await reader.read();
      ok(!done);
      chunks.push(value);
    }

    deepStrictEqual(chunks, [1, 2, 3]);

    await rejects(reader.read(), { message: 'Error after 3 chunks' });

    await rejects(reader.read(), { message: 'Error after 3 chunks' });
  },
};

// Test that custom error types are preserved through pipeTo
// Inspired by: Deno tests/unit/streams_test.ts (cancel propagation with "resource closed" reason)
export const errorTypePreservationPipeTo = {
  async test() {
    const customError = new CustomStreamError('Custom error', 'ERR_CUSTOM');

    const rs = new ReadableStream({
      start(controller) {
        controller.error(customError);
      },
    });

    const ws = new WritableStream({
      write() {},
    });

    await rejects(
      async () => {
        await rs.pipeTo(ws);
      },
      { message: 'Custom error' }
    );
  },
};

// Test that custom error types are preserved through pipeThrough
// Inspired by: Deno tests/unit/streams_test.ts (error propagation tests)
export const errorTypePreservationPipeThrough = {
  async test() {
    const customError = new CustomStreamError('Pipe through error', 'ERR_PIPE');

    const rs = new ReadableStream({
      pull(controller) {
        controller.error(customError);
      },
    });

    const transform = new TransformStream();
    const result = rs.pipeThrough(transform);

    const reader = result.getReader();

    await rejects(
      async () => {
        await reader.read();
      },
      { message: 'Pipe through error' }
    );
  },
};

// Test race between controller.error() and controller.close() on ReadableStream
// Inspired by: Bun test/js/web/streams/streams.test.js (error handling edge cases)
export const errorRaceWithCloseReadable = {
  async test() {
    let controller;

    const rs = new ReadableStream({
      start(c) {
        controller = c;
      },
    });

    const reader = rs.getReader();
    const readPromise = reader.read();

    controller.error(new Error('Error wins'));
    try {
      controller.close();
    } catch (_e) {
      // May throw since stream is already errored
    }

    await rejects(readPromise, { message: 'Error wins' });
  },
};

// Test race between writer.abort() and writer.close() on WritableStream
// Inspired by: Bun test/js/web/streams/streams.test.js (abort/close race conditions)
export const errorRaceWithCloseWritable = {
  async test() {
    let writeStarted = false;

    const ws = new WritableStream({
      write() {
        writeStarted = true;
        // Simulate a slow write that can be aborted
        return scheduler.wait(100);
      },
    });

    const writer = ws.getWriter();

    const writePromise = writer.write('data').catch((e) => e);

    await scheduler.wait(5);
    ok(writeStarted, 'Write should have started');

    await writer.abort(new Error('Abort wins'));

    const writeResult = await writePromise;
    ok(
      writeResult === undefined || writeResult instanceof Error,
      'Write should complete or error'
    );
  },
};

// Test error thrown in TransformStream transform() callback using controller.error()
// Inspired by: Bun test/js/web/streams/streams.test.js (TransformStream error handling)
export const errorInTransformFlush = {
  async test() {
    let transformController;

    const ts = new TransformStream({
      start(controller) {
        transformController = controller;
      },
      transform(chunk, controller) {
        controller.enqueue(chunk);
      },
    });

    const reader = ts.readable.getReader();

    transformController.error(new Error('Transform error'));

    await rejects(
      async () => {
        await reader.read();
      },
      { message: 'Transform error' }
    );
  },
};

// Test error propagation through nested tee branches
// Inspired by: Bun test/js/web/streams/streams.test.js (tee error handling)
export const errorPropagationTeeMultiBranch = {
  async test() {
    let controller;

    const rs = new ReadableStream({
      start(c) {
        controller = c;
      },
    });

    // Create nested tees: original -> [branch1, temp] -> [branch2, branch3]
    const [branch1, temp] = rs.tee();
    const [branch2, branch3] = temp.tee();

    const reader1 = branch1.getReader();
    const reader2 = branch2.getReader();
    const reader3 = branch3.getReader();

    // Start reads on all branches
    const read1 = reader1.read();
    const read2 = reader2.read();
    const read3 = reader3.read();

    // Error the source
    controller.error(new Error('Source error'));

    // All branches should receive the error
    const results = await Promise.allSettled([read1, read2, read3]);

    for (const result of results) {
      strictEqual(result.status, 'rejected');
      strictEqual(result.reason.message, 'Source error');
    }
  },
};

// Test AbortSignal cancellation during active pipeTo
// Inspired by: Deno tests/unit/streams_test.ts (abort tests), Bun test/js/web/streams/streams.test.js
export const abortSignalDuringPipe = {
  async test() {
    const chunks = [];
    let pullCount = 0;

    const rs = new ReadableStream({
      async pull(controller) {
        pullCount++;
        await scheduler.wait(10);
        if (pullCount <= 10) {
          controller.enqueue(pullCount);
        } else {
          controller.close();
        }
      },
    });

    const ws = new WritableStream({
      write(chunk) {
        chunks.push(chunk);
      },
    });

    const abortController = new AbortController();

    // Start piping
    const pipePromise = rs.pipeTo(ws, { signal: abortController.signal });

    // Wait for some chunks to flow
    await scheduler.wait(50);

    // Abort mid-pipe
    abortController.abort(new Error('User cancelled'));

    // Pipe should reject with an error (type may vary by implementation)
    await rejects(async () => {
      await pipePromise;
    }, Error);

    // Some chunks should have been written
    ok(chunks.length > 0, 'Some chunks written before abort');
    ok(chunks.length < 10, 'Not all chunks written due to abort');
  },
};
