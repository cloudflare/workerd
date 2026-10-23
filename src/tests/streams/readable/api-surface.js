// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Surface shape of ReadableStream and its default reader/controller.

import { strictEqual, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

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

// getReader()'s options convert as a WebIDL dictionary (parity): null
// means no options, a function's members are read, and every other
// primitive throws TypeError (messages differ).
export const getReaderOptionsShapes = {
  test() {
    let modeReads = 0;
    const options = Object.defineProperty(function options() {}, 'mode', {
      get() {
        modeReads++;
        return undefined;
      },
    });
    for (const opts of [null, options]) {
      const rs = new ReadableStream();
      rs.getReader(opts);
      strictEqual(rs.locked, true);
    }
    strictEqual(modeReads, 1);
    for (const opts of [5, 'x', true, 1n, Symbol('s')]) {
      throws(
        () => new ReadableStream().getReader(opts),
        usingTsImpl
          ? { name: 'TypeError', message: 'Reader options must be an object' }
          : TypeError
      );
    }
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
