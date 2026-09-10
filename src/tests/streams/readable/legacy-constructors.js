// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Without streams_enable_constructors the JS-backed ReadableStream
// cannot be constructed: the constructor and ReadableStream.from()
// throw plain Errors pointing at the flag, and
// ReadableStreamDefaultController is not even exposed. Native-backed
// streams (Response bodies etc.) remain fully usable, including tee().

import { strictEqual, ok, throws } from 'node:assert';

export const legacyConstructorsThrow = {
  test() {
    const expected = {
      name: 'Error',
      message:
        'To use the new ReadableStream() constructor, enable the ' +
        'streams_enable_constructors compatibility flag. Refer to the docs ' +
        'for more information: https://developers.cloudflare.com/workers/' +
        'platform/compatibility-dates/#compatibility-flags',
    };
    throws(() => new ReadableStream(), expected);
    throws(() => new ReadableStream({}), expected);
    throws(() => ReadableStream.from(['a']), expected);
    strictEqual(typeof ReadableStream.from, 'function');
    strictEqual(typeof ReadableStreamDefaultController, 'undefined');
    strictEqual(typeof ReadableStreamDefaultReader, 'function');
  },
};

export const legacyNativeStreamsStillWork = {
  async test() {
    const resp = new Response('hello');
    ok(resp.body instanceof ReadableStream);
    strictEqual(resp.body.locked, false);
    const [b1, b2] = resp.body.tee();
    const r = await b1.getReader().read();
    strictEqual(new TextDecoder().decode(r.value), 'hello');
    const r2 = await b2.getReader().read();
    strictEqual(new TextDecoder().decode(r2.value), 'hello');
  },
};
