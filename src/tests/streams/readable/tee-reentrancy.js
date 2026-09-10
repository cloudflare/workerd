// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Reentrant cancellation against tee's push loop: user code cancels one
// branch while the tee machinery is mid-delivery to both. Migrated from
// api/streams/streams-test.js — the C++ regressions where pushing to a
// just-cancelled branch consumer crashed the isolate.

import { strictEqual, ok } from 'node:assert';
import { usingTsImpl } from 'which-impl';

// A transform's transform() hook cancels branch2 synchronously before
// enqueueing: the enqueue's push loop must skip the cancelled branch and
// still deliver to branch1 (no-crash regression + delivery assertion).
export const transformTeeReentrancySynchronousCancel = {
  async test() {
    let reader2;
    let cancelledBranch2 = false;
    const ts = new TransformStream({
      transform(chunk, controller) {
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

    const read1Promise = reader1.read();
    const read2Promise = reader2.read();

    await writer.write('test data');

    const result1 = await read1Promise;
    strictEqual(result1.value, 'test data');
    // branch2 was cancelled; its pending read settles (done) rather than
    // hanging or crashing.
    const result2 = await read2Promise;
    ok(result2 !== undefined);
    ok(cancelledBranch2);

    await writer.close();
    await reader1.cancel();
  },
};

// A .then() handler on branch1's read cancels branch2 while the tee push
// loop is still delivering (asynchronous reentrancy variant of the same
// crash).
export const transformStreamTeeReentrancy = {
  async test() {
    const { readable, writable } = new TransformStream();
    const writer = writable.getWriter();

    const [branch1, branch2] = readable.tee();
    const reader1 = branch1.getReader();
    const reader2 = branch2.getReader();

    const read1Promise = reader1.read();
    const read2Promise = reader2.read();

    read1Promise.then(() => {
      reader2.cancel('cancelled during transform');
    });

    await writer.write('transform data');

    const result1 = await read1Promise;
    strictEqual(result1.value, 'transform data');
    const result2 = await read2Promise;
    ok(result2 !== undefined);

    await writer.close();
    await reader1.cancel();
  },
};

// Cancelling one branch between chunks: the surviving branch keeps
// receiving enqueues (mid-stream branch teardown regression).
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

    let read1Promise = reader1.read();
    const read2Promise = reader2.read();

    controller.enqueue('chunk1');
    strictEqual((await read1Promise).value, 'chunk1');
    strictEqual((await read2Promise).value, 'chunk1');

    // Not awaited under TypeScript: a lone branch's cancel promise stays
    // pending until the other branch cancels (see teeCancelReasonComposite).
    const cancel2 = reader2.cancel('done with branch2');
    if (!usingTsImpl) await cancel2;
    await scheduler.wait(10);

    read1Promise = reader1.read();
    controller.enqueue('chunk2');
    strictEqual((await read1Promise).value, 'chunk2');

    read1Promise = reader1.read();
    controller.enqueue('chunk3');
    strictEqual((await read1Promise).value, 'chunk3');

    read1Promise = reader1.read();
    controller.close();
    strictEqual((await read1Promise).done, true);
  },
};
