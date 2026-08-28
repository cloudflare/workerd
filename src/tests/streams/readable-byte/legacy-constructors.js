// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Fully-unflagged legacy cell: the byte-stream constructor is gated
// behind streams_enable_constructors like the value-stream one, while
// NATIVE (Response body) BYOB reads work with the pre-flag semantics:
// read(view) does NOT detach the caller's buffer (pre
// streams_byob_reader_detaches_buffer) and end-of-stream reads resolve
// with value UNDEFINED (pre internal_stream_byob_return_view).

import { strictEqual, throws } from 'node:assert';

export const legacyByteConstructorThrows = {
  test() {
    const expected = {
      name: 'Error',
      message: /streams_enable_constructors/,
    };
    throws(() => new ReadableStream({ type: 'bytes' }), expected);
    throws(
      () => new ReadableStream({ type: 'bytes', autoAllocateChunkSize: 64 }),
      expected
    );
  },
};

export const legacyNativeByobNoDetach = {
  async test() {
    const resp = new Response('hello');
    const reader = resp.body.getReader({ mode: 'byob' });
    const view = new Uint8Array(8);
    const r = await reader.read(view);
    strictEqual(r.done, false);
    strictEqual(new TextDecoder().decode(r.value), 'hello');
    // Pre-flag semantics: the result view shares the CALLER's buffer,
    // which was never detached.
    strictEqual(r.value.buffer, view.buffer);
    strictEqual(view.byteLength, 8);
    const end = await reader.read(new Uint8Array(8));
    strictEqual(end.done, true);
    strictEqual(end.value, undefined);
  },
};

export const legacyNativeReadAtLeast = {
  async test() {
    const resp = new Response('foobarbaz');
    const reader = resp.body.getReader({ mode: 'byob' });
    const r = await reader.readAtLeast(4, new Uint8Array(16));
    strictEqual(r.done, false);
    strictEqual(r.value.byteLength, 9);
    const tail = await reader.readAtLeast(16, new Uint8Array(16));
    strictEqual(tail.done, true);
    strictEqual(tail.value, undefined);
  },
};
