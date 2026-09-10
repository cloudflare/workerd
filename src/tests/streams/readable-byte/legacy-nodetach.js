// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The pre-2021-11-10 window WITH constructors enabled: the
// streams_byob_reader_detaches_buffer and internal_stream_byob_return_
// view flags govern NATIVE streams only. JS-backed byte streams detach
// the caller's buffer unconditionally, in every era.

import { strictEqual, ok } from 'node:assert';

// JS-backed read(view) detaches at call time even in this window.
export const nodetachJsBackedStillDetaches = {
  async test() {
    let controller;
    const rs = new ReadableStream({
      type: 'bytes',
      start(c) {
        controller = c;
      },
    });
    const reader = rs.getReader({ mode: 'byob' });
    const view = new Uint8Array(4);
    const read = reader.read(view);
    strictEqual(view.byteLength, 0); // detached immediately
    const req = controller.byobRequest;
    ok(req !== null);
    req.view[0] = 5;
    req.respond(1);
    const r = await read;
    strictEqual(r.value.byteLength, 1);
    strictEqual(r.value[0], 5);
    ok(r.value.buffer !== view.buffer);
    // End-of-stream on a JS-backed stream resolves with an EMPTY VIEW
    // in every era (internal_stream_byob_return_view is native-only).
    controller.close();
    const end = await reader.read(new Uint8Array(4));
    strictEqual(end.done, true);
    ok(end.value instanceof Uint8Array);
    strictEqual(end.value.byteLength, 0);
  },
};

// Native-body reads keep the window's no-detach + done-undefined
// semantics.
export const nodetachNativeKeepsCallerBuffer = {
  async test() {
    const resp = new Response('hello');
    const reader = resp.body.getReader({ mode: 'byob' });
    const view = new Uint8Array(8);
    const r = await reader.read(view);
    strictEqual(r.value.buffer, view.buffer);
    strictEqual(view.byteLength, 8);
    const end = await reader.read(new Uint8Array(8));
    strictEqual(end.done, true);
    strictEqual(end.value, undefined);
  },
};
