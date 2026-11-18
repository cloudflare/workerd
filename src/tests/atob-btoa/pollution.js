// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// atob() and btoa() do not depend on anything user code can replace.
//
// While built-ins are replaced, this module uses only what it captured
// here, syntax, and index loops: no assertion, no iterator, no method of a
// built-in. Each test collects what happened and asserts once everything
// is restored.

import { ok, strictEqual, throws } from 'node:assert';
import {
  binaryString,
  kInvalidBase64,
  kNotLatin1,
  referenceEncode,
} from 'helpers';

const { defineProperty, getOwnPropertyDescriptor } = Object;
const { deleteProperty } = Reflect;

const kBinary = binaryString(20000);
const kEncoded = referenceEncode(kBinary);
const kSymbol = Symbol('argument');
const kCalled = new Error('a replaced built-in was called');

function fail() {
  throw kCalled;
}

class Fake {
  constructor() {
    throw kCalled;
  }
}

// Replaces each `[object, key, value]` while `fn` runs.
function withReplaced(replacements, fn) {
  const saved = [];
  for (let i = 0; i < replacements.length; i++) {
    saved[i] = getOwnPropertyDescriptor(replacements[i][0], replacements[i][1]);
  }
  let replaced = 0;
  try {
    for (; replaced < replacements.length; replaced++) {
      defineProperty(replacements[replaced][0], replacements[replaced][1], {
        __proto__: null,
        value: replacements[replaced][2],
        writable: true,
        configurable: true,
      });
    }
    return fn();
  } finally {
    for (let i = replaced - 1; i >= 0; i--) {
      if (saved[i] === undefined) {
        deleteProperty(replacements[i][0], replacements[i][1]);
      } else {
        defineProperty(replacements[i][0], replacements[i][1], saved[i]);
      }
    }
  }
}

// An object that is not the global, with the functions as its methods.
const kHolder = { atob, btoa };

function atobInvalid() {
  atob('!');
}
function btoaNotLatin1() {
  btoa('Ā');
}
function atobMissing() {
  atob();
}
function btoaReceiver() {
  kHolder.btoa('');
}
function atobSymbol() {
  atob(kSymbol);
}

function caught(thrower) {
  try {
    thrower();
  } catch (error) {
    return error;
  }
  return undefined;
}

// Everything atob() and btoa() do, collected without anything a test here
// replaces.
function exercise() {
  return {
    results: [
      atob('YWJj') === 'abc',
      btoa('abc') === 'YWJj',
      atob(' YQ = = ') === 'a',
      atob(kEncoded) === kBinary,
      btoa(kBinary) === kEncoded,
      btoa(123) === 'MTIz',
      atob(1234) === '\xd7\x6d\xf8',
    ],
    errors: [
      caught(atobInvalid),
      caught(btoaNotLatin1),
      caught(atobMissing),
      caught(btoaReceiver),
      caught(atobSymbol),
    ],
  };
}

function verify({ results, errors }) {
  strictEqual(results.length, 7);
  for (const result of results) strictEqual(result, true);
  for (const error of errors) ok(error !== kCalled);
  strictEqual(Object.getPrototypeOf(errors[0]), DOMException.prototype);
  strictEqual(errors[0].name, 'InvalidCharacterError');
  strictEqual(errors[0].message, kInvalidBase64);
  strictEqual(errors[0].code, 5);
  strictEqual(Object.getPrototypeOf(errors[1]), DOMException.prototype);
  strictEqual(errors[1].name, 'InvalidCharacterError');
  strictEqual(errors[1].message, kNotLatin1);
  for (const error of errors.slice(2)) {
    strictEqual(Object.getPrototypeOf(error), TypeError.prototype);
  }
  const names = [
    'atobInvalid',
    'btoaNotLatin1',
    'atobMissing',
    'btoaReceiver',
    'atobSymbol',
  ];
  for (const [index, error] of errors.entries()) {
    // No frame of the runtime's, as when nothing is replaced.
    const lines = error.stack.split('\n');
    strictEqual(lines[0], `${error.name}: ${error.message}`);
    ok(lines[1].startsWith(`    at ${names[index]} (`), error.stack);
    ok(lines[2].startsWith('    at caught ('), error.stack);
  }
}

export const unreplaced = {
  test() {
    // What the other tests verify holds, to begin with.
    verify(exercise());
  },
};

export const replacedGlobals = {
  test() {
    verify(
      withReplaced(
        [
          [globalThis, 'DOMException', Fake],
          [globalThis, 'TypeError', Fake],
          [globalThis, 'Error', Fake],
          [globalThis, 'Uint8Array', Fake],
          [globalThis, 'ArrayBuffer', Fake],
          [globalThis, 'String', Fake],
          [globalThis, 'Reflect', {}],
          [globalThis, 'Object', Fake],
          [globalThis, 'Symbol', Fake],
          [globalThis, 'TextDecoder', Fake],
          [globalThis, 'TextEncoder', Fake],
          [globalThis, 'Buffer', Fake],
          [globalThis, 'self', {}],
        ],
        exercise
      )
    );
  },
};

export const replacedStatics = {
  test() {
    verify(
      withReplaced(
        [
          [Uint8Array, 'fromBase64', fail],
          [Uint8Array, 'from', fail],
          [Uint8Array, 'of', fail],
          [String, 'fromCharCode', fail],
          [String, 'fromCodePoint', fail],
          [String, 'raw', fail],
          [Reflect, 'apply', fail],
          [Reflect, 'construct', fail],
          [Object, 'defineProperty', fail],
          [Object, 'defineProperties', fail],
          [Object, 'getOwnPropertyDescriptor', fail],
          [Object, 'getPrototypeOf', fail],
          [Object, 'setPrototypeOf', fail],
          [Error, 'captureStackTrace', fail],
          [Array, 'from', fail],
          [Array, 'isArray', fail],
        ],
        exercise
      )
    );
  },
};

export const replacedPrototypeMethods = {
  test() {
    const typedArray = Object.getPrototypeOf(Uint8Array.prototype);
    verify(
      withReplaced(
        [
          [Uint8Array.prototype, 'toBase64', fail],
          [Uint8Array.prototype, 'setFromBase64', fail],
          [Uint8Array.prototype, 'toHex', fail],
          [typedArray, 'subarray', fail],
          [typedArray, 'slice', fail],
          [typedArray, 'set', fail],
          [typedArray, Symbol.iterator, fail],
          [String.prototype, 'toString', fail],
          [String.prototype, 'valueOf', fail],
          [String.prototype, 'charCodeAt', fail],
          [String.prototype, 'codePointAt', fail],
          [String.prototype, 'replace', fail],
          [String.prototype, 'replaceAll', fail],
          [String.prototype, 'slice', fail],
          [String.prototype, 'split', fail],
          [String.prototype, 'trim', fail],
          [String.prototype, 'concat', fail],
          [String.prototype, 'isWellFormed', fail],
          [String.prototype, 'toWellFormed', fail],
          [String.prototype, Symbol.iterator, fail],
          [RegExp.prototype, 'exec', fail],
          [RegExp.prototype, 'test', fail],
          [RegExp.prototype, Symbol.replace, fail],
          [Array.prototype, 'join', fail],
          [Array.prototype, 'push', fail],
          [Array.prototype, Symbol.iterator, fail],
          [Function.prototype, 'call', fail],
          [Function.prototype, 'apply', fail],
          [Function.prototype, 'bind', fail],
          [Error.prototype, 'toString', fail],
          [DOMException.prototype, 'constructor', fail],
        ],
        exercise
      )
    );
  },
};

export const objectPrototypeProperties = {
  test() {
    // Properties that make a string or an error look like something else.
    verify(
      withReplaced(
        [
          [String.prototype, 'then', fail],
          [String.prototype, 'toJSON', fail],
          [Object.prototype, 'then', fail],
          [Object.prototype, 'toJSON', fail],
          [Object.prototype, 'enumerable', true],
          [Object.prototype, 'writable', true],
        ],
        exercise
      )
    );
  },
};

export const objectPrototypeAccessors = {
  test() {
    // Accessors that a property definition, a descriptor read or an
    // assignment to a missing property would find on Object.prototype.
    const keys = [
      'value',
      'get',
      'set',
      'writable',
      'enumerable',
      'configurable',
      'stack',
      'name',
      'message',
      'code',
      'length',
      'alphabet',
      'lastChunkHandling',
      'omitPadding',
      '0',
    ];
    let touched = 0;
    let defined = 0;
    let outcome;
    let touchedByCalls;
    try {
      for (; defined < keys.length; defined++) {
        defineProperty(Object.prototype, keys[defined], {
          __proto__: null,
          get() {
            touched++;
            return undefined;
          },
          set() {
            touched++;
          },
          configurable: true,
        });
      }
      outcome = exercise();
      touchedByCalls = touched;
    } finally {
      for (let i = defined - 1; i >= 0; i--) {
        deleteProperty(Object.prototype, keys[i]);
      }
    }
    strictEqual(touchedByCalls, 0);
    for (const error of outcome.errors) ok(error !== kCalled);
    strictEqual(outcome.errors[0].message, kInvalidBase64);
    strictEqual(outcome.errors[1].message, kNotLatin1);
    for (const result of outcome.results) strictEqual(result, true);
    verify(exercise());
  },
};

export const replacedFunctionsStillThrowTheirOwnErrors = {
  test() {
    // The functions do not look themselves up on the global: replacing
    // one does not change what the other, or a saved reference, does.
    const savedAtob = atob;
    const savedBtoa = btoa;
    globalThis.atob = fail;
    globalThis.btoa = fail;
    try {
      strictEqual(savedAtob('YWJj'), 'abc');
      strictEqual(savedBtoa('abc'), 'YWJj');
      throws(() => savedAtob('!'), { name: 'InvalidCharacterError' });
      throws(() => savedBtoa('Ā'), { name: 'InvalidCharacterError' });
    } finally {
      delete globalThis.atob;
      delete globalThis.btoa;
    }
    strictEqual(atob, savedAtob);
    strictEqual(btoa, savedBtoa);
  },
};
