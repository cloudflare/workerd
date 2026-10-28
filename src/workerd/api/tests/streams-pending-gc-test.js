// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { ok } from 'node:assert';

// Creates an object with `setup`, drops every reference to it, and returns whether GC collected it.
async function collected(setup) {
  let ref;
  (() => {
    ref = new WeakRef(setup());
  })();
  for (let i = 0; i < 5; i++) {
    await scheduler.wait(1);
    gc();
  }
  return ref.deref() === undefined;
}

export const idleStreamIsCollected = {
  async test() {
    ok(await collected(() => new ReadableStream({})));
    ok(
      await collected(() => {
        const stream = new ReadableStream({});
        stream.getReader();
        return stream;
      })
    );
  },
};

export const streamWithPendingReadIsCollected = {
  async test() {
    ok(
      await collected(() => {
        const stream = new ReadableStream({});
        stream.getReader().read();
        return stream;
      })
    );
  },
};

export const readerWithPendingReadIsCollected = {
  async test() {
    ok(
      await collected(() => {
        const reader = new ReadableStream({}).getReader();
        reader.read();
        return reader;
      })
    );
  },
};

export const byteStreamWithPendingByobReadIsCollected = {
  async test() {
    ok(
      await collected(() => {
        const stream = new ReadableStream({ type: 'bytes' });
        stream.getReader({ mode: 'byob' }).read(new Uint8Array(8));
        return stream;
      })
    );
  },
};

async function collectGarbage() {
  for (let i = 0; i < 5; i++) {
    await scheduler.wait(1);
    gc();
  }
}

export const pendingReadStillSettles = {
  async test() {
    let controller;
    const reader = new ReadableStream({
      start(c) {
        controller = c;
      },
    }).getReader();
    const read = reader.read();
    await collectGarbage();
    controller.enqueue('chunk');
    const { value, done } = await read;
    ok(value === 'chunk' && !done);
  },
};

// The controller can still settle a read, so a live controller keeps the read alive even when
// nothing else refers to the stream or the reader.
export const pendingReadKeptByLiveController = {
  async test() {
    let controller;
    let read;
    (() => {
      const reader = new ReadableStream({
        start(c) {
          controller = c;
        },
      }).getReader();
      read = reader.read();
    })();
    await collectGarbage();
    controller.enqueue('chunk');
    const { value, done } = await read;
    ok(value === 'chunk' && !done);
  },
};

export const responseTextKeptByLiveController = {
  async test() {
    let controller;
    let text;
    (() => {
      const stream = new ReadableStream({
        start(c) {
          controller = c;
        },
      });
      text = new Response(stream).text();
    })();
    await collectGarbage();
    controller.enqueue(new TextEncoder().encode('kept alive'));
    controller.close();
    ok((await text) === 'kept alive');
  },
};
