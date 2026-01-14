// Test for streams_no_default_auto_allocate_chunk_size compat flag
import { strictEqual, ok, deepStrictEqual } from 'node:assert';

export const byobRequestIsNullWithoutAutoAllocate = {
  async test() {
    // When autoAllocateChunkSize is NOT set, byobRequest should be null
    // for non-BYOB reads (spec-compliant behavior)
    let byobRequestWasNull = false;
    let pullCalled = false;

    const rs = new ReadableStream({
      type: 'bytes',
      pull(controller) {
        pullCalled = true;
        byobRequestWasNull = controller.byobRequest === null;
        // Must use enqueue() since byobRequest is null
        controller.enqueue(new Uint8Array([1, 2, 3, 4]));
        controller.close();
      },
    });

    const reader = rs.getReader();
    const result = await reader.read();

    ok(pullCalled, 'pull should have been called');
    ok(
      byobRequestWasNull,
      'byobRequest should be null when autoAllocateChunkSize is not set'
    );
    strictEqual(result.done, false);
    deepStrictEqual([...result.value], [1, 2, 3, 4]);
  },
};

export const byobRequestAvailableWithAutoAllocate = {
  async test() {
    // When autoAllocateChunkSize IS set, byobRequest should be available
    let byobRequestWasAvailable = false;
    let pullCalled = false;

    const rs = new ReadableStream({
      type: 'bytes',
      autoAllocateChunkSize: 1024,
      pull(controller) {
        pullCalled = true;
        byobRequestWasAvailable = controller.byobRequest !== null;
        if (controller.byobRequest) {
          // Use the BYOB request
          const view = controller.byobRequest.view;
          new Uint8Array(view.buffer, view.byteOffset, 4).set([5, 6, 7, 8]);
          controller.byobRequest.respond(4);
        } else {
          controller.enqueue(new Uint8Array([5, 6, 7, 8]));
        }
        controller.close();
      },
    });

    const reader = rs.getReader();
    const result = await reader.read();

    ok(pullCalled, 'pull should have been called');
    ok(
      byobRequestWasAvailable,
      'byobRequest should be available when autoAllocateChunkSize is set'
    );
    strictEqual(result.done, false);
    deepStrictEqual([...result.value], [5, 6, 7, 8]);
  },
};

export const byobReadStillWorks = {
  async test() {
    // BYOB reads should still work regardless of autoAllocateChunkSize
    let byobRequestWasAvailable = false;

    const rs = new ReadableStream({
      type: 'bytes',
      // No autoAllocateChunkSize set
      pull(controller) {
        byobRequestWasAvailable = controller.byobRequest !== null;
        if (controller.byobRequest) {
          const view = controller.byobRequest.view;
          new Uint8Array(view.buffer, view.byteOffset, 4).set([9, 10, 11, 12]);
          controller.byobRequest.respond(4);
        }
        controller.close();
      },
    });

    // Use BYOB reader
    const reader = rs.getReader({ mode: 'byob' });
    const buffer = new Uint8Array(16);
    const result = await reader.read(buffer);

    ok(
      byobRequestWasAvailable,
      'byobRequest should be available for BYOB reads'
    );
    strictEqual(result.done, false);
    deepStrictEqual([...result.value], [9, 10, 11, 12]);
  },
};

export const multipleReadsWithEnqueue = {
  async test() {
    // Test that multiple reads work correctly with enqueue()
    let pullCount = 0;

    const rs = new ReadableStream({
      type: 'bytes',
      pull(controller) {
        pullCount++;
        if (pullCount <= 3) {
          controller.enqueue(new Uint8Array([pullCount]));
        } else {
          controller.close();
        }
      },
    });

    const reader = rs.getReader();
    const chunks = [];

    while (true) {
      const { value, done } = await reader.read();
      if (done) break;
      chunks.push([...value]);
    }

    strictEqual(chunks.length, 3);
    deepStrictEqual(chunks[0], [1]);
    deepStrictEqual(chunks[1], [2]);
    deepStrictEqual(chunks[2], [3]);
  },
};

export const pipeToWorksWithoutAutoAllocate = {
  async test() {
    // Test that pipeTo works correctly without autoAllocateChunkSize
    const allBytes = [];

    const rs = new ReadableStream({
      type: 'bytes',
      start(controller) {
        controller.enqueue(new Uint8Array([1, 2]));
        controller.enqueue(new Uint8Array([3, 4]));
        controller.close();
      },
    });

    const ws = new WritableStream({
      write(chunk) {
        allBytes.push(...chunk);
      },
    });

    await rs.pipeTo(ws);

    // Verify all bytes were received (chunks may be combined)
    deepStrictEqual(allBytes, [1, 2, 3, 4]);
  },
};
