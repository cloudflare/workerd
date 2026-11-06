// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The stack property, which WebIDL asks for wherever native errors have
// one.

import { deepStrictEqual, strictEqual } from 'node:assert';
import { usingTsImpl } from 'which-impl';
import { attributesOf } from 'helpers';

function throwOne() {
  throw new DOMException('m', 'AbortError');
}

function caught(fn) {
  try {
    fn();
  } catch (error) {
    return error;
  }
  throw new Error('did not throw');
}

export const stackHeaderAndFrames = {
  test() {
    const lines = caught(throwOne).stack.split('\n');
    strictEqual(lines[0], 'AbortError: m');
    // The first frame is the code that constructed it, not the
    // constructor.
    strictEqual(lines[1].trim().startsWith('at throwOne '), true, lines[1]);
    strictEqual(new DOMException('m').stack.split('\n')[0], 'Error: m');
    strictEqual(new DOMException().stack.split('\n')[0], 'Error');
    strictEqual(
      new DOMException('', 'AbortError').stack.split('\n')[0],
      'AbortError'
    );
  },
};

export const subclassFrames = {
  test() {
    class Sub extends DOMException {
      constructor() {
        super('m', 'NotFoundError');
      }
    }
    function make() {
      return new Sub();
    }
    const lines = make().stack.split('\n');
    strictEqual(lines[0], 'NotFoundError: m');
    // Ledger #10: V8 leaves out the subclass constructor's frame, as for
    // subclasses of Error.
    const firstFrame = lines[1].trim();
    if (usingTsImpl) {
      strictEqual(firstFrame.startsWith('at make '), true, firstFrame);
    } else {
      strictEqual(firstFrame.startsWith('at new Sub '), true, firstFrame);
    }
  },
};

export const stackProperty = {
  test() {
    const error = new DOMException('m', 'AbortError');
    // Ledger #2: an own accessor, enumerable only in the C++.
    strictEqual(
      attributesOf(error, 'stack'),
      usingTsImpl ? 'accessorc' : 'accessorec'
    );
    error.stack = 'replaced';
    strictEqual(error.stack, 'replaced');
  },
};

export const prepareStackTrace = {
  test() {
    const original = Error.prepareStackTrace;
    const calls = [];
    Error.prepareStackTrace = (error, frames) => {
      calls.push([error, frames.length > 0]);
      return 'prepared';
    };
    try {
      const error = new DOMException('m', 'AbortError');
      strictEqual(error.stack, 'prepared');
      strictEqual(error.stack, 'prepared');
      deepStrictEqual(calls, [[error, true]]);
    } finally {
      Error.prepareStackTrace = original;
    }
  },
};

export const captureStackTrace = {
  test() {
    const error = new DOMException('m', 'AbortError');
    function recapture() {
      Error.captureStackTrace(error);
    }
    recapture();
    strictEqual(attributesOf(error, 'stack'), 'accessorc');
    const lines = error.stack.split('\n');
    strictEqual(lines[0], 'AbortError: m');
    strictEqual(lines[1].trim().startsWith('at recapture '), true, lines[1]);
  },
};
