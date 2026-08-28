// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// WritableStream API surface: globals, direct-construction guards, and the
// bare constructor. WPT writable-streams/properties.any.js and
// general.any.js pass on both implementations and own the full IDL shape;
// this module carries the workerd-side checks migrated from
// streams-js-test.js.

import { ok, throws } from 'node:assert';

// The three writable classes are installed as globals.
export const writableGlobalsExist = {
  test() {
    ok(WritableStream !== undefined);
    ok(WritableStreamDefaultWriter !== undefined);
    ok(WritableStreamDefaultController !== undefined);
  },
};

// The controller is minted only by the stream machinery.
export const controllerNotConstructable = {
  test() {
    throws(() => new WritableStreamDefaultController(), TypeError);
  },
};

// The no-argument constructor works.
export const newWritableStream = {
  test() {
    new WritableStream();
  },
};
