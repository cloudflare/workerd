// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Surface shape of ReadableStream and its default reader/controller.

import { strictEqual, throws } from 'node:assert';

export const readableGlobalsExist = {
  test() {
    strictEqual(typeof ReadableStream, 'function');
    strictEqual(typeof ReadableStreamDefaultReader, 'function');
    strictEqual(typeof ReadableStreamDefaultController, 'function');
    strictEqual(typeof ReadableStream.from, 'function');
  },
};

// The controller is not user-constructable (parity).
export const controllerNotConstructable = {
  test() {
    throws(() => new ReadableStreamDefaultController(), TypeError);
  },
};

// getReader() with a bad mode throws TypeError; the stream stays
// unlocked and usable (migrated from streams-test.js).
export const getReaderBadModeThrows = {
  test() {
    const rs = new ReadableStream();
    throws(() => rs.getReader({ mode: 'bad' }), TypeError);
    strictEqual(rs.locked, false);
    rs.getReader();
    strictEqual(rs.locked, true);
  },
};

// A default (value) stream does not support BYOB readers.
export const defaultStreamNoByobReader = {
  test() {
    const rs = new ReadableStream();
    throws(() => rs.getReader({ mode: 'byob' }), TypeError);
    strictEqual(rs.locked, false);
  },
};

// locked flips with getReader()/releaseLock(), and a locked stream
// rejects a second getReader() and tee() (migrated from streams-test.js
// and streams-js-test.js readableStreamReleaseLock).
export const lockedLifecycle = {
  test() {
    const rs = new ReadableStream();
    strictEqual(rs.locked, false);
    const reader = rs.getReader();
    strictEqual(rs.locked, true);
    throws(() => rs.getReader(), TypeError);
    throws(() => rs.tee(), TypeError);
    reader.releaseLock();
    strictEqual(rs.locked, false);
    rs.getReader();
  },
};
