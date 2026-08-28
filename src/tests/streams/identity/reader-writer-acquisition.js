// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Acquiring readers and writers on identity streams beyond getReader()/
// getWriter(): the reader/writer classes are directly constructible against
// the stream (in both implementations, without streams_enable_constructors),
// and getReader() validates its mode.

import { ok, strictEqual, throws } from 'node:assert';

export const readerWriterDirectConstructors = {
  test() {
    {
      const t = new IdentityTransformStream();
      const writer = new WritableStreamDefaultWriter(t.writable);
      ok(writer);
      strictEqual(t.writable.locked, true);
    }
    {
      const t = new IdentityTransformStream();
      const reader = new ReadableStreamDefaultReader(t.readable);
      ok(reader);
      strictEqual(t.readable.locked, true);
    }
    {
      const t = new IdentityTransformStream();
      const reader = new ReadableStreamBYOBReader(t.readable);
      ok(reader);
      strictEqual(t.readable.locked, true);
    }
  },
};

export const getReaderInvalidModeThrows = {
  test() {
    const its = new IdentityTransformStream();
    throws(() => its.readable.getReader({ mode: 'nope' }), TypeError);
    // The failed acquisition must not have locked the stream.
    strictEqual(its.readable.locked, false);
    ok(its.readable.getReader());
  },
};
