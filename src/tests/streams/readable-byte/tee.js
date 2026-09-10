// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// tee() on byte streams: per-branch chunk cloning, mixed reader types,
// cancel composition, and error propagation.

import { strictEqual, ok, deepStrictEqual } from 'node:assert';
import { usingTsImpl } from 'which-impl';

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
