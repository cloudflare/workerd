// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Without streams_enable_constructors the JS-backed `new WritableStream()`
// is unavailable: the constructor throws a plain Error pointing at the
// flag (writable.c++ WritableStream::constructor). The class itself and
// its companions still exist as globals.

import { ok, strictEqual, throws } from 'node:assert';

export const legacyConstructorThrows = {
  test() {
    throws(() => new WritableStream(), {
      name: 'Error',
      message:
        'To use the new WritableStream() constructor, enable the ' +
        'streams_enable_constructors compatibility flag. ' +
        'Refer to the docs for more information: https://developers.cloudflare.com/workers/platform/compatibility-dates/#compatibility-flags',
    });
    throws(() => new WritableStream({}), Error);
  },
};

export const legacyGlobalsStillExist = {
  test() {
    ok(WritableStream !== undefined);
    ok(WritableStreamDefaultWriter !== undefined);
    // The controller class is itself gated by the flag: without it the
    // global is not installed at all.
    strictEqual(globalThis.WritableStreamDefaultController, undefined);
  },
};
