// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { deepStrictEqual, rejects, strictEqual, throws } from 'node:assert';

const cases = {
  partial: { origin: [[17, 34]], sibling: [34] },
  bufferedSibling: { origin: [[17, 34]], sibling: [34] },
  largeEnqueue: { origin: [[17, 34, 51, 68]], sibling: [34, 51, 68] },
  pendingReads: {
    origin: [
      [17, 34],
      [51, 68],
    ],
    sibling: [34, 51, 68],
  },
  respondedPrefix: { origin: [[17, 34, 51]], sibling: [51] },
  typedTail: {
    origin: [
      [17, 34],
      [51, 68],
    ],
    sibling: [68],
  },
  typedPrefix: {
    origin: [
      [17, 34, 51, 68],
      [85, 102],
    ],
    sibling: [102],
  },
  threeConsumers: {
    origin: [[17, 34]],
    sibling: [34],
    third: [17, 34],
  },
  followupPull: { origin: [[17, 34, 51]], sibling: [34, 51] },
  closeDuringResponse: { origin: [[17]], sibling: [] },
};

function progressTest(name) {
  return {
    async test(_controller, env) {
      const expected = cases[name];
      const typed = name.startsWith('typed');
      for (const method of typed ? ['respond'] : ['respond', 'new-view']) {
        const query = new URLSearchParams({ name, method });
        const origin = await env.PROGRESS.fetch(
          `https://progress/origin?${query}`
        );
        strictEqual(await origin.text(), 'origin complete');
        const response = await env.PROGRESS.fetch(
          `https://progress/settle?${query}`
        );
        const result = await response.json();
        deepStrictEqual(
          result.origin.map((read) => read.bytes),
          expected.origin,
          `${name}/${method}`
        );
        deepStrictEqual(result.sibling, expected.sibling, `${name}/${method}`);
        deepStrictEqual(
          result.third,
          expected.third ?? [],
          `${name}/${method}`
        );
        strictEqual(result.entries, 1);
        strictEqual(result.nestedError, null);
        strictEqual(result.wrapperView, null);
        strictEqual(result.retainedViewLength, 0);
        deepStrictEqual(
          result.origin.map((read) => read.done),
          expected.origin.map(() => false)
        );
        if (typed) {
          deepStrictEqual(
            result.origin.map((read) => read.type),
            ['Uint16Array', 'Uint16Array']
          );
          strictEqual(result.origin[0].offset, 4);
        }
        strictEqual(result.closed, true);
      }
    },
  };
}

export const partialResponseWithEnqueue = progressTest('partial');
export const partialResponseWithBufferedSibling =
  progressTest('bufferedSibling');
export const partialResponseWithLargerEnqueue = progressTest('largeEnqueue');
export const responseDrainsPendingReads = progressTest('pendingReads');
export const responsePreservesEarlierPrefix = progressTest('respondedPrefix');
export const typedResponseTailPrecedesEnqueue = progressTest('typedTail');
export const typedResponsePreservesEarlierPrefix = progressTest('typedPrefix');
export const responseOrdersThreeConsumers = progressTest('threeConsumers');
export const revokedResponseStillPulls = progressTest('followupPull');
export const reservedResponseFinishesBeforeClose = progressTest(
  'closeDuringResponse'
);

async function settled(promise) {
  let timer;
  try {
    return await Promise.race([
      promise,
      new Promise((_resolve, reject) => {
        timer = setTimeout(
          () =>
            reject(new Error('Read did not settle after response progress')),
          1000
        );
      }),
    ]);
  } finally {
    clearTimeout(timer);
  }
}

const bytes = (value) =>
  Array.from(new Uint8Array(value.buffer, value.byteOffset, value.byteLength));

async function queuedPrefix({ revoked, typed, method }) {
  let controller;
  const stream = new ReadableStream({
    type: 'bytes',
    start(value) {
      controller = value;
    },
  });
  const reader = stream.getReader({ mode: 'byob' });
  const first = reader.read(
    typed ? new Uint16Array(new ArrayBuffer(16), 4, 4) : new Uint8Array(8),
    { min: typed ? 1 : 2 }
  );
  const original = revoked ? controller.byobRequest : null;
  controller.enqueue(new Uint8Array([17]));
  const second =
    revoked || typed
      ? reader.read(
          typed ? new Uint16Array(new ArrayBuffer(12), 2, 4) : new Uint8Array(8)
        )
      : null;
  const request = controller.byobRequest;
  if (original) strictEqual(original.view, null);
  const supplied = typed ? [34, 51, 68, 85] : [34];
  if (revoked) {
    strictEqual(request, null);
    controller.enqueue(new Uint8Array(supplied));
  } else {
    strictEqual(request.view.byteOffset, typed ? 5 : 1);
    strictEqual(request.view.byteLength, 7);
    strictEqual(request.atLeast, 1);
    request.view.set(supplied);
    if (method === 'new-view') {
      request.respondWithNewView(request.view.subarray(0, supplied.length));
    } else {
      request.respond(supplied.length);
    }
  }
  const firstResult = await settled(first);
  strictEqual(firstResult.done, false);
  deepStrictEqual(
    bytes(firstResult.value),
    typed ? [17, 34, 51, 68] : [17, 34]
  );
  if (typed) strictEqual(firstResult.value.byteOffset, 4);
  if (second) {
    const next = controller.byobRequest;
    strictEqual(next.view.byteOffset, typed ? 3 : 0);
    next.view[0] = typed ? 102 : 51;
    if (method === 'new-view') {
      next.respondWithNewView(next.view.subarray(0, 1));
    } else {
      next.respond(1);
    }
    const secondResult = await settled(second);
    strictEqual(secondResult.done, false);
    deepStrictEqual(bytes(secondResult.value), typed ? [85, 102] : [51]);
    if (typed) strictEqual(secondResult.value.byteOffset, 2);
  }
  controller.close();
  strictEqual((await settled(reader.read(new Uint8Array(8)))).done, true);
}

export const queuedPrefixPrecedesResponse = {
  async test() {
    for (const method of ['respond', 'new-view']) {
      await queuedPrefix({ revoked: false, typed: false, method });
    }
  },
};

export const revokedFrontReadPrecedesPendingRead = {
  async test() {
    for (const method of ['respond', 'new-view']) {
      await queuedPrefix({ revoked: true, typed: false, method });
    }
  },
};

export const typedQueuedPrefixPreservesTail = {
  async test() {
    for (const revoked of [false, true]) {
      await queuedPrefix({ revoked, typed: true, method: 'respond' });
    }
  },
};

export const closeAfterPartialQueuedEntry = {
  async test() {
    let controller;
    const stream = new ReadableStream({
      type: 'bytes',
      start(value) {
        controller = value;
      },
    });
    const reader = stream.getReader({ mode: 'byob' });
    controller.enqueue(new Uint8Array([1, 2, 3]));
    const first = await settled(reader.read(new Uint8Array(1)));
    strictEqual(first.done, false);
    deepStrictEqual(bytes(first.value), [1]);
    controller.close();
    const tail = await settled(reader.read(new Uint8Array(8)));
    strictEqual(tail.done, false);
    deepStrictEqual(bytes(tail.value), [2, 3]);
    strictEqual((await settled(reader.read(new Uint8Array(8)))).done, true);
  },
};

export const cancelDiscardsPartialProgress = {
  async test() {
    for (const method of ['respond', 'new-view', 'enqueue']) {
      let controller;
      const stream = new ReadableStream({
        type: 'bytes',
        start(value) {
          controller = value;
        },
      });
      const reader = stream.getReader({ mode: 'byob' });
      const pending = reader.read(new Uint8Array(8), { min: 4 });
      if (method === 'enqueue') controller.enqueue(new Uint8Array([17, 34]));
      const request = controller.byobRequest;
      if (method !== 'enqueue') {
        request.view.set([17, 34]);
        if (method === 'new-view') {
          request.respondWithNewView(request.view.subarray(0, 2));
        } else {
          request.respond(2);
        }
      }
      strictEqual(request.atLeast, 2);
      await settled(reader.cancel());
      const result = await settled(pending);
      strictEqual(result.done, true);
      strictEqual(result.value.byteLength, 0);
      strictEqual(request.view, null);
      strictEqual(await settled(reader.closed), undefined);
    }
  },
};

export const detachedIdleViewStillRejectsEnqueue = {
  async test() {
    let controller;
    const stream = new ReadableStream({
      type: 'bytes',
      start(value) {
        controller = value;
      },
    });
    const reader = stream.getReader({ mode: 'byob' });
    const pending = reader.read(new Uint8Array(8));
    controller.byobRequest.view.buffer.transfer();
    throws(() => controller.enqueue(new Uint8Array([34])), {
      name: 'TypeError',
      message: 'The byobRequest.view is zero-length or was detached',
    });
    const reason = new Error('Source finished after detached-view validation');
    const readRejected = rejects(pending, (error) => error === reason);
    const closedRejected = rejects(reader.closed, (error) => error === reason);
    controller.error(reason);
    await settled(Promise.all([readRejected, closedRejected]));
  },
};

export const alignedShortTypedReadAtClose = {
  async test() {
    for (const queuedBeforeRead of [false, true]) {
      for (const exposeView of [false, true]) {
        let controller;
        const stream = new ReadableStream({
          type: 'bytes',
          start(value) {
            controller = value;
          },
        });
        const reader = stream.getReader({ mode: 'byob' });
        const prefix = new Uint8Array([17, 34, 51, 68]);
        if (queuedBeforeRead) controller.enqueue(prefix);
        const pending = reader.read(
          new Uint16Array(new ArrayBuffer(16), 4, 4),
          {
            min: 3,
          }
        );
        if (!queuedBeforeRead) controller.enqueue(prefix);
        if (exposeView) strictEqual(controller.byobRequest.view.byteOffset, 8);
        controller.close();
        const result = await settled(pending);
        strictEqual(result.done, false);
        strictEqual(result.value.constructor, Uint16Array);
        strictEqual(result.value.byteOffset, 4);
        deepStrictEqual(bytes(result.value), [17, 34, 51, 68]);
        strictEqual(
          (await settled(reader.read(new Uint16Array(4)))).done,
          true
        );
        await settled(reader.closed);
      }
    }
  },
};

export const shortTypedReadAtClose = {
  async test() {
    for (const prefix of [[17], [17, 34, 51]]) {
      for (const readAfterClose of [false, true]) {
        let controller;
        const stream = new ReadableStream({
          type: 'bytes',
          start(value) {
            controller = value;
          },
        });
        const reader = stream.getReader({ mode: 'byob' });
        if (readAfterClose) {
          controller.enqueue(new Uint8Array(prefix));
          controller.close();
        }
        const pending = reader.read(new Uint16Array(4), { min: 2 });
        const subsequent = reader.read(new Uint8Array(8));
        if (!readAfterClose) {
          controller.enqueue(new Uint8Array(prefix));
          controller.close();
        }
        const result = await settled(pending);
        strictEqual(result.value.constructor, Uint16Array);
        strictEqual(result.value.byteOffset, 0);
        strictEqual(result.done, readAfterClose && prefix.length === 1);
        deepStrictEqual(
          bytes(result.value),
          prefix.slice(0, prefix.length & ~1)
        );
        const next = await settled(subsequent);
        strictEqual(next.done, true);
        strictEqual(next.value.byteLength, 0);
        strictEqual(await settled(reader.closed), undefined);
        const last = await settled(reader.read(new Uint8Array(8)));
        strictEqual(last.done, true);
        strictEqual(last.value.byteLength, 0);
      }
    }
  },
};

export const typedCloseLeavesFractionalTailForNextRead = {
  async test() {
    let controller;
    const stream = new ReadableStream({
      type: 'bytes',
      start(value) {
        controller = value;
      },
    });
    controller.enqueue(new Uint8Array([17, 34, 51]));
    controller.close();
    const reader = stream.getReader({ mode: 'byob' });
    const first = await settled(reader.read(new Uint16Array(4)));
    strictEqual(first.done, false);
    deepStrictEqual(bytes(first.value), [17, 34]);
    const tail = await settled(reader.read(new Uint8Array(8)));
    strictEqual(tail.done, false);
    deepStrictEqual(bytes(tail.value), [51]);
    strictEqual((await settled(reader.read(new Uint8Array(8)))).done, true);
  },
};

export const replacementViewRetainsAlignmentBelowMinimum = {
  async test() {
    let controller;
    const stream = new ReadableStream({
      type: 'bytes',
      start(value) {
        controller = value;
      },
    });
    const reader = stream.getReader({ mode: 'byob' });
    const pending = reader.read(new Uint8Array(8), { min: 5 });
    const request = controller.byobRequest;
    request.view.set([17, 34, 51, 68]);
    request.respondWithNewView(new Uint16Array(request.view.buffer, 0, 2));
    controller.enqueue(new Uint8Array([85]));
    strictEqual(request.view, null);
    strictEqual(controller.byobRequest, null);
    controller.enqueue(new Uint8Array([102]));
    const result = await settled(pending);
    strictEqual(result.done, false);
    deepStrictEqual(bytes(result.value), [17, 34, 51, 68, 85, 102]);
    controller.close();
    strictEqual((await settled(reader.read(new Uint8Array(8)))).done, true);
  },
};
