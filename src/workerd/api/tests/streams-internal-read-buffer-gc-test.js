// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression test for a use-after-free of the temporary buffer that
// ReadableStreamInternalController::read() reads into.
//
// The read destination is a kj-heap buffer, and its only owner is a lambda held
// by a JS-heap object (an IoContext::addFunctor() functor installed as a
// jsg::Promise continuation). A garbage collection that runs while the
// underlying kj tryRead() is still in flight can therefore free the buffer out
// from under the in-progress read, which writes into freed memory.
//
// Each case below starts a read against a stream with no data available,
// collects, and only then supplies the data, so the bytes land in the buffer
// after the collection has had its chance to reclaim it. Under ASAN a
// regression shows up as a heap-use-after-free on the read; without ASAN it
// shows up as corrupted or truncated data.
import { strictEqual } from 'node:assert';

// A young-generation scavenge is what reclaims these buffers in practice: the
// functor's JS wrapper is freshly allocated, so it dies in new space.
function collect() {
  // Churn the young generation so the scavenger has something to do, then ask
  // for a scavenge explicitly, and finally a full GC to catch anything that got
  // promoted.
  for (let round = 0; round < 4; round++) {
    let _sink = null;
    for (let i = 0; i < 20000; i++) {
      _sink = { i, pad: [i, i + 1, i + 2, i + 3] };
    }
    gc({ type: 'minor', execution: 'sync' });
  }
  gc({ type: 'major', execution: 'sync' });
}

const FILL = 0x41;

function check(view, expectedLength) {
  strictEqual(view.byteLength, expectedLength);
  for (let i = 0; i < view.byteLength; i++) {
    if (view[i] !== FILL) {
      throw new Error(
        `byte ${i} of ${view.byteLength} is ${view[i]}, expected ${FILL}`
      );
    }
  }
}

// The buffer is sized to match the 128 KiB reads seen in production.
const BUFFER_SIZE = 131072;
const PAYLOAD_SIZE = 65536;

export const byobReadSurvivesCollection = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();
    const reader = readable.getReader({ mode: 'byob' });
    const writer = writable.getWriter();

    // Nothing has been written yet, so this leaves a tryRead() in flight with a
    // live destination buffer.
    const pending = reader.read(new Uint8Array(BUFFER_SIZE));

    collect();

    await writer.write(new Uint8Array(PAYLOAD_SIZE).fill(FILL));
    const { value, done } = await pending;
    strictEqual(done, false);
    check(value, PAYLOAD_SIZE);

    await writer.close();
  },
};

// The read promise itself is dropped before collecting, so nothing in the
// script's own scope keeps the continuation reachable.
export const abandonedByobReadSurvivesCollection = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();
    const reader = readable.getReader({ mode: 'byob' });
    const writer = writable.getWriter();

    reader.read(new Uint8Array(BUFFER_SIZE)).then(
      () => {},
      () => {}
    );

    collect();

    await writer.write(new Uint8Array(PAYLOAD_SIZE).fill(FILL));
    await writer.close();
  },
};

// Many concurrent in-flight reads, to widen the window and give the scavenger
// more young objects to reclaim.
export const manyByobReadsSurviveCollection = {
  async test() {
    const streams = [];
    for (let i = 0; i < 16; i++) {
      const { readable, writable } = new IdentityTransformStream();
      const reader = readable.getReader({ mode: 'byob' });
      const writer = writable.getWriter();
      streams.push({
        writer,
        pending: reader.read(new Uint8Array(BUFFER_SIZE)),
      });
    }

    collect();

    for (const { writer, pending } of streams) {
      await writer.write(new Uint8Array(PAYLOAD_SIZE).fill(FILL));
      const { value, done } = await pending;
      strictEqual(done, false);
      check(value, PAYLOAD_SIZE);
      await writer.close();
    }
  },
};
