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
// the next chunk; the sibling is unaffected. DIVERGENCE (ledger #32): the
// read takes the released bytes and the chunk together under TS (spec);
// C++ gives it the released bytes alone.
export const teeReleasedPartialReadByob = {
  async test() {
    const { a, b, controller } = await teeWithReleasedPartialRead();
    const reader = a.getReader({ mode: 'byob' });
    const read = reader.read(new Uint8Array(4));
    controller.enqueue(new Uint8Array([3, 4, 5, 6]));
    controller.close();
    const [first, second] = usingTsImpl
      ? [
          [1, 2, 3, 4],
          [5, 6],
        ]
      : [
          [1, 2],
          [3, 4, 5, 6],
        ];
    deepStrictEqual([...(await read).value], first);
    deepStrictEqual([...(await reader.read(new Uint8Array(4))).value], second);
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

// A byte stream whose source took byobRequest for a pending read(view)
// (after writing `filled` into it with respond()), whose reader then
// released, and which was teed.
async function teeWithHeldByobRequest(filled = []) {
  let controller;
  const rs = new ReadableStream({
    type: 'bytes',
    start(c) {
      controller = c;
    },
  });
  const reader = rs.getReader({ mode: 'byob' });
  const read = reader.read(new Uint8Array(4), { min: 4 });
  await scheduler.wait(5);
  if (filled.length > 0) {
    controller.byobRequest.view.set(filled);
    controller.byobRequest.respond(filled.length);
  }
  const request = controller.byobRequest;
  reader.releaseLock();
  await rejectionOf(read);
  const [a, b] = rs.tee();
  return { a, b, controller, request };
}

const INVALIDATED = {
  name: 'TypeError',
  message: usingTsImpl
    ? 'This BYOB request has been invalidated'
    : 'This ReadableStreamBYOBRequest has been invalidated.',
};

// A byobRequest held across tee() keeps working, as the spec has it: the
// responded byte reaches both branches (parity). Ledger #25: C++ leaves
// the request's view attached (zero-length) and then throws on the
// source's next enqueue(); TS invalidates it (spec) and delivers the chunk.
export const teeKeepsHeldByobRequest = {
  async test() {
    const { a, b, controller, request } = await teeWithHeldByobRequest();
    strictEqual(controller.byobRequest, request);
    request.view[0] = 7;
    request.respond(1);
    if (usingTsImpl) {
      strictEqual(request.view, null);
    } else {
      strictEqual(request.view.byteLength, 0);
    }
    const readerA = a.getReader();
    const readerB = b.getReader();
    deepStrictEqual([...(await readerA.read()).value], [7]);
    deepStrictEqual([...(await readerB.read()).value], [7]);
    if (!usingTsImpl) {
      throws(() => controller.enqueue(new Uint8Array([8])), {
        name: 'TypeError',
        message: 'The byobRequest.view is zero-length or was detached',
      });
      controller.error(new Error('cleanup'));
      return;
    }
    strictEqual(controller.byobRequest, null);
    controller.enqueue(new Uint8Array([8]));
    controller.close();
    deepStrictEqual([...(await drainBytes(a, readerA))], [8]);
    deepStrictEqual([...(await drainBytes(b, readerB))], [8]);
  },
};

// Once the sibling cancels, the held request still comes first and fills
// the remaining branch's read (parity, spec).
export const teeSoleBranchUsesHeldByobRequest = {
  async test() {
    const { a, b, controller, request } = await teeWithHeldByobRequest();
    b.cancel('bye');
    await scheduler.wait(5);
    const reader = a.getReader({ mode: 'byob' });
    const read = reader.read(new Uint8Array(8));
    strictEqual(controller.byobRequest, request);
    request.view[0] = 7;
    request.respond(1);
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

// DIVERGENCE (ledger #7, on a tee branch): a fractional element fill under
// a branch's read(Uint16Array) at close(). TypeScript errors that branch
// alone, as the spec's per-branch close does: the read rejects with
// TypeError, and so does closed, while the source's close() succeeds and
// the sibling receives every byte. C++ ends the branch cleanly without the
// trailing byte.
//
// Two shapes. A read issued after close() (3 bytes enqueued; the first
// read, issued before or after the enqueue, takes the whole element, and
// the next meets the trailing byte at the close). And a read still pending
// at close() with a fractional fill: 1 byte under the default min, or 3
// bytes under { min: 2 }.
export const teeBranchFractionalCloseErrorsBranch = {
  async test() {
    const expected = 'Insufficient bytes to fill elements in the given view';
    const makeTee = () => {
      let controller;
      const rs = new ReadableStream({
        type: 'bytes',
        start(c) {
          controller = c;
        },
      });
      return [controller, ...rs.tee()];
    };

    for (const readBeforeEnqueue of [true, false]) {
      const [controller, a, b] = makeTee();
      const reader = a.getReader({ mode: 'byob' });
      const first = readBeforeEnqueue ? reader.read(new Uint16Array(4)) : null;
      await scheduler.wait(5);
      controller.enqueue(new Uint8Array([1, 2, 3]));
      controller.close();
      const r1 = await (first ?? reader.read(new Uint16Array(4)));
      deepStrictEqual([...new Uint8Array(r1.value.buffer, 0, 2)], [1, 2]);
      strictEqual(r1.value.length, 1);
      const r2 = reader.read(new Uint16Array(4));
      if (usingTsImpl) {
        strictEqual((await rejectionOf(r2)).message, expected);
        strictEqual((await rejectionOf(reader.closed)).message, expected);
      } else {
        const r = await r2;
        strictEqual(r.value.byteLength, 0);
        strictEqual(await reader.closed, undefined);
      }
      deepStrictEqual([...(await drainBytes(b))], [1, 2, 3]);
    }

    for (const [bytes, min] of [
      [[1], undefined],
      [[1, 2, 3], 2],
    ]) {
      const [controller, a, b] = makeTee();
      const reader = a.getReader({ mode: 'byob' });
      const read = reader.read(new Uint16Array(4), min ? { min } : undefined);
      await scheduler.wait(5);
      controller.enqueue(new Uint8Array(bytes));
      controller.close();
      if (usingTsImpl) {
        strictEqual((await rejectionOf(read)).message, expected);
        strictEqual((await rejectionOf(reader.closed)).message, expected);
      } else {
        const r = await read;
        strictEqual(r.done, false);
        // The whole elements only.
        strictEqual(r.value.length, bytes.length >> 1);
        strictEqual(await reader.closed, undefined);
      }
      deepStrictEqual([...(await drainBytes(b))], bytes);
    }
  },
};

// DIVERGENCE (ledger #7, on the sole remaining tee branch): its sibling
// cancelled, the branch errors on a fractional fill at close(). The source
// requested close, and the spec never forwards a branch's error to it: its
// cancel() never runs, and the sibling's cancel() resolves undefined as the
// source ends. C++ never errors the branch; the source ends the same way.
// Both shapes of teeBranchFractionalCloseErrorsBranch.
export const teeSoleBranchFractionalCloseSkipsSourceCancel = {
  async test() {
    const expected = 'Insufficient bytes to fill elements in the given view';
    for (const pendingAtClose of [true, false]) {
      let controller;
      let cancelled = false;
      const rs = new ReadableStream({
        type: 'bytes',
        start(c) {
          controller = c;
        },
        cancel() {
          cancelled = true;
        },
      });
      const [a, b] = rs.tee();
      const siblingCancel = b.cancel('sibling');
      const reader = a.getReader({ mode: 'byob' });
      const first = reader.read(new Uint16Array(4));
      await scheduler.wait(5);
      controller.enqueue(new Uint8Array(pendingAtClose ? [1] : [1, 2, 3]));
      controller.close();
      const last = pendingAtClose ? first : reader.read(new Uint16Array(4));
      if (!pendingAtClose) strictEqual((await first).value.length, 1);
      if (usingTsImpl) {
        strictEqual((await rejectionOf(last)).message, expected);
      } else {
        strictEqual((await last).value.byteLength, 0);
      }
      strictEqual(await siblingCancel, undefined);
      strictEqual(cancelled, false);
    }
  },
};

// Ledger #25: bytes the released read already held. The request, first
// taken after tee(), is over the rest of the read's buffer; responding
// delivers the held bytes and the new one together to both branches, and
// enqueue() instead discards the request and delivers the held bytes
// ahead of the chunk (spec). C++ drops the held bytes in both cases.
export const teeHeldByobRequestWithReleasedBytes = {
  async test() {
    {
      const { a, b, controller } = await teeWithHeldByobRequest([1, 2]);
      const request = controller.byobRequest;
      strictEqual(request.view.byteLength, 2);
      request.view[0] = 7;
      request.respond(1);
      const expected = usingTsImpl ? [1, 2, 7] : [7];
      deepStrictEqual([...(await a.getReader().read()).value], expected);
      deepStrictEqual([...(await b.getReader().read()).value], expected);
      controller.error(new Error('cleanup'));
    }
    {
      const { a, b, controller, request } = await teeWithHeldByobRequest([
        1, 2,
      ]);
      controller.enqueue(new Uint8Array([9]));
      strictEqual(request.view, null);
      strictEqual(controller.byobRequest, null);
      throws(() => request.respond(1), INVALIDATED);
      const readerA = a.getReader();
      const readerB = b.getReader();
      if (!usingTsImpl) {
        deepStrictEqual([...(await readerA.read()).value], [9]);
        deepStrictEqual([...(await readerB.read()).value], [9]);
        controller.error(new Error('cleanup'));
        return;
      }
      controller.close();
      deepStrictEqual([...(await drainBytes(a, readerA))], [1, 2, 9]);
      deepStrictEqual([...(await drainBytes(b, readerB))], [1, 2, 9]);
    }
  },
};

// Ledger #32 through a held request: enqueue() retires it, and a branch's
// pending read(view) takes the released read's bytes and the chunk
// together under TS (spec). C++ has dropped those bytes (ledger #25).
export const teeHeldByobRequestEnqueueFillsByobRead = {
  async test() {
    const { a, b, controller } = await teeWithHeldByobRequest([1, 2]);
    const read = a.getReader({ mode: 'byob' }).read(new Uint8Array(8));
    await scheduler.wait(5);
    controller.enqueue(new Uint8Array([9]));
    deepStrictEqual([...(await read).value], usingTsImpl ? [1, 2, 9] : [9]);
    const expectedB = usingTsImpl ? [1, 2] : [9];
    deepStrictEqual([...(await b.getReader().read()).value], expectedB);
    controller.error(new Error('cleanup'));
  },
};

// The held request survives a sibling's cancel and a further tee() of the
// remaining branch, and fills a BYOB read on a new branch (parity; the
// held bytes are ledger #25, as above).
export const teeHeldByobRequestAcrossNestedTee = {
  async test() {
    const { a, b, controller, request } = await teeWithHeldByobRequest([1, 2]);
    b.cancel('bye');
    await scheduler.wait(5);
    const [a1, a2] = a.tee();
    strictEqual(controller.byobRequest, request);
    const read = a1.getReader({ mode: 'byob' }).read(new Uint8Array(8));
    await scheduler.wait(5);
    request.view[0] = 7;
    request.respond(1);
    const expected = usingTsImpl ? [1, 2, 7] : [7];
    deepStrictEqual([...(await read).value], expected);
    deepStrictEqual([...(await a2.getReader().read()).value], expected);
  },
};

// respondWithNewView() on a held request, and a request held for a
// released auto-allocated default read, work across tee() (parity).
export const teeHeldByobRequestNewViewAndAutoAllocate = {
  async test() {
    {
      const { a, b, request } = await teeWithHeldByobRequest();
      const view = request.view;
      view.set([3, 4]);
      request.respondWithNewView(
        new Uint8Array(view.buffer, view.byteOffset, 2)
      );
      deepStrictEqual([...(await a.getReader().read()).value], [3, 4]);
      deepStrictEqual([...(await b.getReader().read()).value], [3, 4]);
    }
    {
      let controller;
      const rs = new ReadableStream({
        type: 'bytes',
        autoAllocateChunkSize: 8,
        start(c) {
          controller = c;
        },
      });
      const reader = rs.getReader();
      const read = reader.read();
      await scheduler.wait(5);
      const request = controller.byobRequest;
      reader.releaseLock();
      await rejectionOf(read);
      const [a, b] = rs.tee();
      strictEqual(controller.byobRequest, request);
      strictEqual(request.view.byteLength, 8);
      request.view[0] = 5;
      request.respond(1);
      deepStrictEqual([...(await a.getReader().read()).value], [5]);
      deepStrictEqual([...(await b.getReader().read()).value], [5]);
    }
  },
};

// After close() the held request takes only respond(0), which retires it
// (parity, spec). Ledger #25: error() invalidates it under TS (spec); C++
// leaves its view attached and rejects respond() as if closed.
export const teeHeldByobRequestAfterCloseOrError = {
  async test() {
    {
      const { a, controller, request } = await teeWithHeldByobRequest();
      controller.close();
      strictEqual(controller.byobRequest, request);
      throws(() => request.respond(1), {
        name: 'TypeError',
        message: usingTsImpl
          ? 'bytesWritten must be zero after the stream is closed'
          : 'The bytesWritten must be zero after the stream is closed.',
      });
      request.respond(0);
      strictEqual(controller.byobRequest, null);
      strictEqual((await a.getReader().read()).done, true);
    }
    {
      const { controller, request } = await teeWithHeldByobRequest();
      controller.error(new Error('boom'));
      if (usingTsImpl) {
        strictEqual(request.view, null);
        throws(() => request.respond(1), INVALIDATED);
      } else {
        strictEqual(request.view.byteLength, 4);
        throws(() => request.respond(1), {
          name: 'TypeError',
          message: 'The bytesWritten must be zero after the stream is closed.',
        });
      }
    }
  },
};

// tee() of a closed native body (drained, or cancelled) locks it like any
// other tee(): a second tee() throws, and both branches are closed
// (parity).
export const teeClosedNativeBodyLocksOriginal = {
  async test() {
    const drained = new Response('abc').body;
    const reader = drained.getReader();
    await drainBytes(drained, reader);
    reader.releaseLock();
    const cancelled = new Response('abc').body;
    await cancelled.cancel();
    for (const body of [drained, cancelled]) {
      const [a, b] = body.tee();
      strictEqual(body.locked, true);
      throws(() => body.tee(), TypeError);
      throws(() => body.getReader(), TypeError);
      for (const branch of [a, b]) {
        strictEqual((await branch.getReader().read()).done, true);
      }
    }
  },
};
