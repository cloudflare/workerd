// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Fully-unflagged legacy cell: JS-backed pipe endpoints are gated
// behind streams_enable_constructors, while NATIVE→NATIVE piping — the
// pre-flag core use — works: body pipes through/into
// IdentityTransformStream and out.

import { strictEqual, throws } from 'node:assert';

export const legacyJsEndpointsGated = {
  test() {
    throws(() => new ReadableStream({}), {
      message: /streams_enable_constructors/,
    });
    throws(() => new WritableStream({}), {
      message: /streams_enable_constructors/,
    });
  },
};

export const legacyNativePipeTo = {
  async test() {
    const its = new IdentityTransformStream();
    const pipeP = new Response('hello').body.pipeTo(its.writable);
    const text = await new Response(its.readable).text();
    strictEqual(text, 'hello');
    await pipeP;
  },
};

export const legacyNativePipeThrough = {
  async test() {
    const readable = new Response('hello').body.pipeThrough(
      new IdentityTransformStream()
    );
    strictEqual(await new Response(readable).text(), 'hello');
  },
};
