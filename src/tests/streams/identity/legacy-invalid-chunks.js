// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Pre-flag invalid chunk handling, as seen by workers predating
// capture_async_api_throws (2022-10-31): write() of an invalid chunk type
// THROWS synchronously (and emits a console warning) instead of returning a
// rejected promise. The stream itself is unaffected. Legacy suite: C++
// implementation only; see identity-cpp-legacy.wd-test.

import { strictEqual, throws } from 'node:assert';

export const legacyInvalidChunkThrowsSynchronously = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    throws(() => writer.write(42), TypeError);
    throws(() => writer.write({ data: 'nope' }), TypeError);
    // The stream survives: a subsequent valid write still flows.
    const reader = readable.getReader();
    const writePromise = writer.write(new Uint8Array([9]));
    const { value, done } = await reader.read();
    strictEqual(done, false);
    strictEqual(value[0], 9);
    await writePromise;
    await writer.close();
  },
};
