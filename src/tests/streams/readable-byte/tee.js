// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// tee() on byte streams: per-branch chunk cloning, mixed reader types,
// cancel composition, error propagation, released pending reads (incl. a
// native body released mid-read), and a byobRequest held across tee().

import { strictEqual, ok, deepStrictEqual, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';
import { drainBytes, rejectionOf } from 'helpers';

// Chunks are CLONED per branch: neither branch's chunk shares a buffer
// with the other or with the (detached) original, and mutation does not
// leak across branches (parity; the WPT tee.any cloning seeds).
export const teeClonesChunksPerBranch = {
  async test() {
    let controller;
    const rs = new ReadableStream({
      type: 'bytes',
      start(c) {
        controller = c;
      },
    });
    const [b1, b2] = rs.tee();
    const p1 = b1.getReader().read();
    const p2 = b2.getReader().read();
    const chunk = new Uint8Array([1, 2, 3]);
    controller.enqueue(chunk);
    const [v1, v2] = [(await p1).value, (await p2).value];
    ok(v1.buffer !== v2.buffer);
    ok(v1.buffer !== chunk.buffer);
    ok(v2.buffer !== chunk.buffer);
    strictEqual(chunk.byteLength, 0); // enqueue detached the original
    v1[0] = 99;
    strictEqual(v2[0], 1);
  },
};

// Both branches drain identical bytes through default readers (migrated
// from streams-tee-edge-cases-test.js).
export const teeByteStreamDefaultReaders = {
  async test() {
    const data = new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8]);
    let offset = 0;
    const rs = new ReadableStream({
      type: 'bytes',
      pull(controller) {
        if (offset < data.length) {
          const chunk = data.slice(offset, offset + 2);
          offset += 2;
          controller.enqueue(chunk);
        } else {
          controller.close();
        }
      },
    });
    const [branch1, branch2] = rs.tee();
    const reader1 = branch1.getReader();
    const reader2 = branch2.getReader();
    const bytes1 = [];
    const bytes2 = [];
    for (;;) {
      const { value, done } = await reader1.read();
      if (done) break;
      bytes1.push(...value);
    }
    for (;;) {
      const { value, done } = await reader2.read();
      if (done) break;
      bytes2.push(...value);
    }
    deepStrictEqual(bytes1, [1, 2, 3, 4, 5, 6, 7, 8]);
    deepStrictEqual(bytes2, [1, 2, 3, 4, 5, 6, 7, 8]);
  },
};

// BYOB reader on one branch, default reader on the other: both receive
// the bytes (migrated from streams-tee-edge-cases-test.js).
export const teeByteStreamMixedReaders = {
  async test() {
    const enc = new TextEncoder();
    const dec = new TextDecoder();
    let controller;
    const rs = new ReadableStream({
      type: 'bytes',
      start(c) {
        controller = c;
      },
    });
    const [branch1, branch2] = rs.tee();
    const reader1 = branch1.getReader({ mode: 'byob' });
    const reader2 = branch2.getReader();
    const read1Promise = reader1.read(new Uint8Array(5));
    const read2Promise = reader2.read();
    controller.enqueue(enc.encode('hello'));
    controller.close();
    const [result1, result2] = await Promise.all([read1Promise, read2Promise]);
    strictEqual(dec.decode(result1.value), 'hello');
    strictEqual(dec.decode(result2.value), 'hello');
  },
};

// DIVERGENCE (the readable suite's ledger #11, byte flavor): when both
// branches cancel, the source cancel hook receives an AggregateError of
// [r1, r2] under TypeScript but only the pair-completing branch's
// reason under C++. NOTE: never await a lone branch's cancel under
// TypeScript — it pends until the other branch cancels.
export const teeCancelComposite = {
  async test() {
    let cancelReason = 'not-called';
    let signalDone;
    const gotCancel = new Promise((resolve) => (signalDone = resolve));
    const rs = new ReadableStream({
      type: 'bytes',
      cancel(r) {
        cancelReason = r;
        signalDone();
      },
    });
    const [b1, b2] = rs.tee();
    b1.cancel('r1');
    b2.cancel('r2');
    await gotCancel;
    if (usingTsImpl) {
      ok(cancelReason instanceof AggregateError);
      deepStrictEqual(cancelReason.errors, ['r1', 'r2']);
    } else {
      strictEqual(cancelReason, 'r2');
    }
  },
};

// Erroring the source propagates the SAME error object to reads on both
// branches (parity).
export const teeErrorPropagatesToBothBranches = {
  async test() {
    let controller;
    const rs = new ReadableStream({
      type: 'bytes',
      start(c) {
        controller = c;
      },
    });
    const [b1, b2] = rs.tee();
    const p1 = b1.getReader().read();
    const p2 = b2.getReader().read();
    const err = new Error('boom');
    controller.error(err);
    let e1, e2;
    await p1.catch((e) => (e1 = e));
    await p2.catch((e) => (e2 = e));
    strictEqual(e1, err);
    strictEqual(e2, err);
  },
};

// A branch reader released with a pending read(view): the branch's next
// reader gets every later byte, as does the sibling (parity).
export const teeReleasedPendingRead = {
  async test() {
    let controller;
    const rs = new ReadableStream({
      type: 'bytes',
      start(c) {
        controller = c;
      },
    });
    const [a, b] = rs.tee();
    const r1 = a.getReader({ mode: 'byob' });
    const read1 = r1.read(new Uint8Array(4));
    await scheduler.wait(5);
    r1.releaseLock();
    await rejectionOf(read1);
    const r2 = a.getReader({ mode: 'byob' });
    const read2 = r2.read(new Uint8Array(4));
    controller.enqueue(new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8]));
    controller.close();
    deepStrictEqual([...(await read2).value], [1, 2, 3, 4]);
    deepStrictEqual(
      [...(await r2.read(new Uint8Array(8))).value],
      [5, 6, 7, 8]
    );
    deepStrictEqual([...(await drainBytes(b))], [1, 2, 3, 4, 5, 6, 7, 8]);
  },
};

// DIVERGENCE: pipeTo() from an autoAllocateChunkSize branch, aborted with
// preventCancel while its read is pending. TypeScript settles the pipe,
// and the branch's next reader receives the next chunk. C++ keeps the
// pipe pending until a chunk arrives, which the aborted pipe's read then
// consumes and drops; asserted up to the pending pipe (bounded).
export const teePipeAbortReleasesPendingRead = {
  async test() {
    let controller;
    const rs = new ReadableStream({
      type: 'bytes',
      autoAllocateChunkSize: 8,
      start(c) {
        controller = c;
      },
    });
    const [a, b] = rs.tee();
    const ac = new AbortController();
    const pipe = a.pipeTo(new WritableStream(), {
      signal: ac.signal,
      preventCancel: true,
    });
    await scheduler.wait(5);
    ac.abort(new Error('stop'));
    const outcome = await Promise.race([
      pipe.then(
        () => 'fulfilled',
        () => 'rejected'
      ),
      scheduler.wait(50).then(() => 'pending'),
    ]);
    if (!usingTsImpl) {
      strictEqual(outcome, 'pending');
      controller.error(new Error('cleanup'));
      await pipe.catch(() => {});
      return;
    }
    strictEqual(outcome, 'rejected');
    const read = a.getReader().read();
    controller.enqueue(new Uint8Array([1, 2, 3]));
    controller.close();
    deepStrictEqual([...(await read).value], [1, 2, 3]);
    deepStrictEqual([...(await drainBytes(b))], [1, 2, 3]);
  },
};

// A byte stream whose reader was released while read(view, { min: 4 })
// held [1, 2].
async function releasedPartialRead() {
  let controller;
  const rs = new ReadableStream({
    type: 'bytes',
    start(c) {
      controller = c;
    },
  });
  const reader = rs.getReader({ mode: 'byob' });
  const read = reader.read(new Uint8Array(4), { min: 4 });
  controller.enqueue(new Uint8Array([1, 2]));
  await scheduler.wait(5);
  reader.releaseLock();
  await rejectionOf(read);
  return { rs, controller };
}

// The same, on branch `a` of a tee.
async function teeWithReleasedPartialRead() {
  let controller;
  const rs = new ReadableStream({
    type: 'bytes',
    start(c) {
      controller = c;
    },
  });
  const [a, b] = rs.tee();
  const reader = a.getReader({ mode: 'byob' });
  const read = reader.read(new Uint8Array(4), { min: 4 });
  controller.enqueue(new Uint8Array([1, 2]));
  await scheduler.wait(5);
  reader.releaseLock();
  await rejectionOf(read);
  return { a, b, controller };
}

// The released bytes reach the branch's next pending read(view) ahead of
// the next chunk; the sibling is unaffected (parity).
export const teeReleasedPartialReadByob = {
  async test() {
    const { a, b, controller } = await teeWithReleasedPartialRead();
    const reader = a.getReader({ mode: 'byob' });
    const read = reader.read(new Uint8Array(4));
    controller.enqueue(new Uint8Array([3, 4, 5, 6]));
    controller.close();
    deepStrictEqual([...(await read).value], [1, 2]);
    deepStrictEqual(
      [...(await reader.read(new Uint8Array(4))).value],
      [3, 4, 5, 6]
    );
    deepStrictEqual([...(await drainBytes(b))], [1, 2, 3, 4, 5, 6]);
  },
};

// ... and a pending default read (parity).
export const teeReleasedPartialReadDefault = {
  async test() {
    const { a, controller } = await teeWithReleasedPartialRead();
    const reader = a.getReader();
    const read = reader.read();
    controller.enqueue(new Uint8Array([3, 4]));
    controller.close();
    deepStrictEqual([...(await read).value], [1, 2]);
    deepStrictEqual([...(await reader.read()).value], [3, 4]);
  },
};

// Buffered before the next reader: one read(view) takes the released bytes
// and the next chunk together (parity).
export const teeReleasedPartialReadBuffered = {
  async test() {
    const { a, controller } = await teeWithReleasedPartialRead();
    controller.enqueue(new Uint8Array([3, 4]));
    controller.close();
    const reader = a.getReader({ mode: 'byob' });
    deepStrictEqual(
      [...(await reader.read(new Uint8Array(8))).value],
      [1, 2, 3, 4]
    );
  },
};

// Buffered, then piped: the destination receives them in order (parity).
export const teeReleasedPartialReadPiped = {
  async test() {
    const { a, controller } = await teeWithReleasedPartialRead();
    controller.enqueue(new Uint8Array([3, 4]));
    controller.close();
    const written = [];
    await a.pipeTo(
      new WritableStream({
        write(chunk) {
          written.push(...chunk);
        },
      })
    );
    deepStrictEqual(written, [1, 2, 3, 4]);
  },
};

// tee() after the release: both branches receive the released bytes ahead
// of the next chunk (parity).
export const teeAfterReleasedPartialRead = {
  async test() {
    const { rs, controller } = await releasedPartialRead();
    const [a, b] = rs.tee();
    controller.enqueue(new Uint8Array([3, 4]));
    controller.close();
    deepStrictEqual([...(await drainBytes(a))], [1, 2, 3, 4]);
    deepStrictEqual([...(await drainBytes(b))], [1, 2, 3, 4]);
  },
};

// tee() of a branch holding released bytes: both new branches receive
// them, and its sibling receives no extra bytes (parity).
export const teeOfBranchWithReleasedPartialRead = {
  async test() {
    const { a, b, controller } = await teeWithReleasedPartialRead();
    const [a1, a2] = a.tee();
    controller.enqueue(new Uint8Array([3, 4]));
    controller.close();
    deepStrictEqual([...(await drainBytes(a1))], [1, 2, 3, 4]);
    deepStrictEqual([...(await drainBytes(a2))], [1, 2, 3, 4]);
    deepStrictEqual([...(await drainBytes(b))], [1, 2, 3, 4]);
  },
};

// A byte stream whose source took byobRequest for a pending read(view),
// whose reader then released, and which was teed.
async function teeWithHeldByobRequest() {
  let controller;
  const rs = new ReadableStream({
    type: 'bytes',
    start(c) {
      controller = c;
    },
  });
  const reader = rs.getReader({ mode: 'byob' });
  const read = reader.read(new Uint8Array(4));
  await scheduler.wait(5);
  const request = controller.byobRequest;
  reader.releaseLock();
  await rejectionOf(read);
  const [a, b] = rs.tee();
  return { a, b, controller, request };
}

const INVALIDATED = {
  name: 'TypeError',
  message: 'This BYOB request has been invalidated',
};

// DIVERGENCE: a byobRequest held across tee(). TypeScript invalidates it
// (branches share the queue and see no byobRequest); later chunks still
// reach both branches. C++ keeps it working, as the spec does: the
// responded byte reaches both branches.
export const teeInvalidatesHeldByobRequest = {
  async test() {
    const { a, b, controller, request } = await teeWithHeldByobRequest();
    if (!usingTsImpl) {
      strictEqual(controller.byobRequest, request);
      request.view[0] = 7;
      request.respond(1);
      deepStrictEqual([...(await a.getReader().read()).value], [7]);
      deepStrictEqual([...(await b.getReader().read()).value], [7]);
      controller.error(new Error('cleanup'));
      return;
    }
    strictEqual(controller.byobRequest, null);
    strictEqual(request.view, null);
    throws(() => request.respond(1), INVALIDATED);
    controller.enqueue(new Uint8Array([8]));
    controller.close();
    deepStrictEqual([...(await drainBytes(a))], [8]);
    deepStrictEqual([...(await drainBytes(b))], [8]);
  },
};

// DIVERGENCE: once the sibling cancels, the remaining branch's read gets a
// fresh byobRequest under TypeScript, and the held one stays invalid.
// C++ keeps exposing the held request, which fills the read (spec).
export const teeSoleBranchMintsFreshByobRequest = {
  async test() {
    const { a, b, controller, request } = await teeWithHeldByobRequest();
    b.cancel('bye');
    await scheduler.wait(5);
    const reader = a.getReader({ mode: 'byob' });
    const read = reader.read(new Uint8Array(8));
    const current = controller.byobRequest;
    if (!usingTsImpl) {
      strictEqual(current, request);
      request.view[0] = 7;
      request.respond(1);
      deepStrictEqual([...(await read).value], [7]);
      return;
    }
    ok(current !== request);
    strictEqual(current.view.byteLength, 8);
    throws(() => request.respond(1), INVALIDATED);
    current.view[0] = 7;
    current.respond(1);
    deepStrictEqual([...(await read).value], [7]);
  },
};

// DIVERGENCE (ledger #29): a native body's reader released while its read
// is in flight, then tee() or clone(). C++ refuses the release (TypeError:
// outstanding read promises). TypeScript rejects the read, and both
// branches receive the whole body, including the bytes the in-flight read
// produced.
export const teeNativeBodyAfterReleaseMidRead = {
  async test(ctrl, env) {
    const text = async (readable) =>
      new TextDecoder().decode(await drainBytes(readable));
    const releasedMidRead = async (mode) => {
      const response = await env.SELF.fetch('http://test/delayed');
      const reader = response.body.getReader(mode ? { mode } : undefined);
      const read = mode ? reader.read(new Uint8Array(16)) : reader.read();
      if (!usingTsImpl) {
        throws(() => reader.releaseLock(), TypeError);
        await read;
        return undefined;
      }
      reader.releaseLock();
      strictEqual((await rejectionOf(read)).name, 'TypeError');
      return response;
    };

    for (const mode of [undefined, 'byob']) {
      const response = await releasedMidRead(mode);
      if (response === undefined) continue;
      const [a, b] = response.body.tee();
      deepStrictEqual(await Promise.all([text(a), text(b)]), [
        'foobarbaz',
        'foobarbaz',
      ]);
    }

    const response = await releasedMidRead(undefined);
    if (response !== undefined) {
      const clone = response.clone();
      deepStrictEqual(await Promise.all([response.text(), clone.text()]), [
        'foobarbaz',
        'foobarbaz',
      ]);
    }
  },
};
