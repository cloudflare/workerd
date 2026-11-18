// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The errors atob() and btoa() throw, and their stacks.

import { deepStrictEqual, ok, strictEqual } from 'node:assert';
import {
  attributesOf,
  kInvalidBase64,
  kNotLatin1,
  throwsInvalidCharacter,
} from 'helpers';
import { usingTsImpl } from 'which-impl';

function catching(fn) {
  try {
    fn();
  } catch (error) {
    return error;
  }
  throw new Error('did not throw');
}

// Every way atob() and btoa() throw an error of their own making, each
// from a function called `thrower`.
const kThrowers = [
  ['InvalidCharacterError', () => atob('!')],
  ['InvalidCharacterError', () => btoa('Ā')],
  ['TypeError', () => atob()],
  ['TypeError', () => btoa()],
  ['TypeError', () => atob.call({}, '')],
  ['TypeError', () => btoa.call({}, '')],
  ['TypeError', () => atob(Symbol())],
  ['TypeError', () => btoa(Symbol())],
].map(([name, call]) => [
  name,
  function thrower() {
    call();
  },
]);

export const invalidCharacterError = {
  test() {
    throwsInvalidCharacter(() => atob('!'), kInvalidBase64);
    throwsInvalidCharacter(() => btoa('Ā'), kNotLatin1);
    for (const error of [
      catching(() => atob('!')),
      catching(() => btoa('Ā')),
    ]) {
      // As a DOMException that script constructs.
      const constructed = new DOMException(
        error.message,
        'InvalidCharacterError'
      );
      ok(error instanceof Error);
      strictEqual(error.constructor, DOMException);
      deepStrictEqual(Reflect.ownKeys(error), Reflect.ownKeys(constructed));
      deepStrictEqual(Object.keys(error), Object.keys(constructed));
      for (const key of ['name', 'message', 'code', 'stack']) {
        strictEqual(
          attributesOf(error, key),
          attributesOf(constructed, key),
          key
        );
      }
      strictEqual(String(error), `InvalidCharacterError: ${error.message}`);
      strictEqual(
        Object.prototype.toString.call(error),
        Object.prototype.toString.call(constructed)
      );
      strictEqual(Error.isError(error), Error.isError(constructed));
      strictEqual(structuredClone(error).name, 'InvalidCharacterError');
      strictEqual(structuredClone(error).message, error.message);
    }
  },
};

export const typeErrors = {
  test() {
    for (const [name, thrower] of kThrowers) {
      if (name !== 'TypeError') continue;
      const error = catching(thrower);
      strictEqual(Object.getPrototypeOf(error), TypeError.prototype);
      strictEqual(error.constructor, TypeError);
      deepStrictEqual(Reflect.ownKeys(error), ['stack', 'message']);
      strictEqual(attributesOf(error, 'message'), 'datawc');
      strictEqual(Error.isError(error), true);
    }
  },
};

export const freshErrorEachTime = {
  test() {
    for (const [, thrower] of kThrowers) {
      const first = catching(thrower);
      const second = catching(thrower);
      ok(first !== second);
      // What one catcher does to an error, the next does not see.
      first.extra = 1;
      strictEqual('extra' in catching(thrower), false);
    }
  },
};

export const stacksStartAtTheCaller = {
  test() {
    // The header, then the frame that called atob() or btoa(): no frame of
    // the runtime's.
    for (const [name, thrower] of kThrowers) {
      const error = (function calledFromHere() {
        return catching(thrower);
      })();
      strictEqual(typeof error.stack, 'string');
      const lines = error.stack.split('\n');
      strictEqual(lines[0], `${name}: ${error.message}`);
      // The arrow function that called atob() or btoa() has no name.
      ok(/^ {4}at \S*errors:\d+:\d+$/.test(lines[1]), error.stack);
      ok(lines[2].startsWith('    at thrower ('), error.stack);
      ok(lines[3].startsWith('    at catching ('), error.stack);
      ok(lines[4].startsWith('    at calledFromHere ('), error.stack);
      ok(!error.stack.includes('per-isolate'), error.stack);
      ok(!error.stack.includes('<anonymous>'), error.stack);
    }
  },
};

// The name and file of each function on the stack when `capture` (which
// the tests pass as a nameless arrow function) runs, innermost first, down
// to this module's `innermost` and `outermost`.
function framesOf(capture) {
  const original = Error.prepareStackTrace;
  Error.prepareStackTrace = (_, sites) =>
    sites.map((site) => [site.getFunctionName(), site.getFileName()]);
  try {
    const frames = (function outermost() {
      return (function innermost() {
        return capture();
      })();
    })();
    const end = frames.findIndex(([name]) => name === 'outermost');
    return frames.slice(0, end + 1);
  } finally {
    Error.prepareStackTrace = original;
  }
}

export const stackFramesOfThrownErrors = {
  test() {
    // Through Error.prepareStackTrace too.
    for (const [, thrower] of kThrowers) {
      const frames = framesOf(() => catching(thrower).stack);
      deepStrictEqual(
        frames.map(([name]) => name),
        [null, 'thrower', 'catching', null, 'innermost', 'outermost']
      );
    }
  },
};

export const stackFramesDuringConversion = {
  test() {
    // Ledger #2: code that the conversion of the argument runs sees a
    // frame of the TypeScript function between it and the caller.
    for (const [name, fn] of [
      ['atob', atob],
      ['btoa', btoa],
    ]) {
      const frames = framesOf(() => {
        let stack;
        fn({
          toString() {
            stack = new Error().stack;
            return '';
          },
        });
        return stack;
      });
      const names = frames.map(([frame]) => frame);
      strictEqual(names[0], 'toString');
      deepStrictEqual(names.slice(-3), [null, 'innermost', 'outermost']);
      const between = frames.slice(1, -3);
      if (usingTsImpl) {
        strictEqual(between.at(-1)[0], name);
        for (const [, file] of between) {
          strictEqual(file, 'workerd:per-isolate/atob-btoa');
        }
      } else {
        deepStrictEqual(between, []);
      }

      // So does an error the engine throws for a conversion that fails
      // after running user code.
      const error = catching(() => fn({ toString: () => Symbol() }));
      strictEqual(
        error.stack.includes('per-isolate/atob-btoa'),
        usingTsImpl,
        error.stack
      );
    }
  },
};

export const stackTraceLimit = {
  test() {
    const original = Error.stackTraceLimit;
    Error.stackTraceLimit = 0;
    try {
      for (const [name, thrower] of kThrowers) {
        const error = catching(thrower);
        strictEqual(error.stack, `${name}: ${error.message}`);
      }
    } finally {
      Error.stackTraceLimit = original;
    }
    Error.stackTraceLimit = 1;
    try {
      for (const [, thrower] of kThrowers) {
        const lines = catching(thrower).stack.split('\n');
        strictEqual(lines.length, 2);
        ok(/^ {4}at \S*errors:\d+:\d+$/.test(lines[1]), lines[1]);
      }
    } finally {
      Error.stackTraceLimit = original;
    }
  },
};
