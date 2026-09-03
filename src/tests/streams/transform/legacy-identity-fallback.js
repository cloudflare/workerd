// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Without transformstream_enable_standard_constructor the TransformStream
// constructor falls back to the internal identity pipe
// (transform.c++): any transformer/strategy arguments are silently
// ignored (with a one-time warning logged), and the result behaves like
// an IdentityTransformStream — byte-oriented pass-through — although the
// wrapper is branded TransformStream (jsg constructor calls wrap as the
// called class, so it is NOT an IdentityTransformStream instance).

import { strictEqual, ok, deepStrictEqual } from 'node:assert';

export const legacyCtorFallsBackToIdentity = {
  async test() {
    let transformCalled = false;
    let startCalled = false;
    const ts = new TransformStream({
      start() {
        startCalled = true;
      },
      transform() {
        transformCalled = true;
      },
    });

    // Branded as TransformStream, not IdentityTransformStream.
    ok(ts instanceof TransformStream);
    ok(!(ts instanceof IdentityTransformStream));

    // Byte pass-through; the transformer hooks never run.
    const writer = ts.writable.getWriter();
    const body = new Response(ts.readable).arrayBuffer();
    await writer.write(new Uint8Array([1, 2, 3]));
    await writer.close();
    deepStrictEqual(new Uint8Array(await body), new Uint8Array([1, 2, 3]));
    strictEqual(startCalled, false);
    strictEqual(transformCalled, false);
  },
};

// The fallback is byte-oriented but accepts strings by UTF-8 encoding
// them (an internal-pipe convenience; the standard constructor instead
// passes any value through untouched).
export const legacyFallbackEncodesStrings = {
  async test() {
    const ts = new TransformStream();
    const writer = ts.writable.getWriter();
    // Consume in parallel so the write is not blocked on backpressure.
    const body = new Response(ts.readable).arrayBuffer();
    await writer.write('str');
    await writer.close();
    deepStrictEqual(
      new Uint8Array(await body),
      new TextEncoder().encode('str')
    );
  },
};
