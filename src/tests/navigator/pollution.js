// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The navigator and sendBeacon() do not depend on anything user code can
// replace. first-read.js covers the first read of `navigator`, which makes
// the instance.
//
// While built-ins are replaced, this module uses only what it captured
// here, syntax, and index loops: no assertion, no iterator, no method of a
// built-in. Each test collects what happened and asserts once everything
// is restored.

import { ok, strictEqual } from 'node:assert';
import { hasLanguage } from 'which-impl';
import {
  kIllegalInvocation,
  kMissingUrl,
  kSymbolArgument,
  received,
} from 'helpers';

const { defineProperty, getOwnPropertyDescriptor } = Object;
const { deleteProperty } = Reflect;
const NavigatorConstructor = Navigator;
const { sendBeacon } = Navigator.prototype;

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

function caught(thrower) {
  try {
    thrower();
  } catch (error) {
    return error;
  }
  return undefined;
}

function construct() {
  new NavigatorConstructor();
}
function wrongReceiver() {
  sendBeacon.call({}, 'http://sink/');
}
function missing() {
  navigator.sendBeacon();
}
function symbolUrl() {
  navigator.sendBeacon(kSymbol);
}
function symbolBody() {
  navigator.sendBeacon('http://sink/', kSymbol);
}

// Everything the navigator does, collected without anything a test here
// replaces. `path` is where the beacon goes.
function exercise(path) {
  const languages = hasLanguage ? navigator.languages : undefined;
  return {
    results: [
      navigator.userAgent === 'Cloudflare-Workers',
      navigator.hardwareConcurrency === 1,
      navigator.platform === '',
      !hasLanguage || navigator.language === 'en',
      !hasLanguage || (languages.length === 1 && languages[0] === 'en'),
      !hasLanguage || languages !== navigator.languages,
      navigator.sendBeacon(path, 'polluted') === true,
    ],
    errors: [
      caught(construct),
      caught(wrongReceiver),
      caught(missing),
      caught(symbolUrl),
      caught(symbolBody),
    ],
  };
}

const kMessages = [
  'Illegal constructor',
  kIllegalInvocation,
  kMissingUrl,
  kSymbolArgument,
  kSymbolArgument,
];

async function verify({ results, errors }, path) {
  strictEqual(results.length, 7);
  for (const result of results) strictEqual(result, true);
  strictEqual(errors.length, kMessages.length);
  for (let i = 0; i < errors.length; i++) {
    ok(errors[i] !== kCalled);
    ok(errors[i] instanceof TypeError);
    strictEqual(errors[i].message, kMessages[i]);
    // The stack starts at the function that made the call, which caught()
    // called.
    ok(!errors[i].stack.includes('per-isolate'), errors[i].stack);
    ok(errors[i].stack.split('\n')[2].includes('caught'), errors[i].stack);
  }
  const beacon = await received(path);
  strictEqual(beacon.method, 'POST');
  strictEqual(beacon.body, 'polluted');
}

export const unreplaced = {
  async test() {
    await verify(exercise('http://sink/unreplaced'), '/unreplaced');
  },
};

export const replacedGlobals = {
  async test() {
    const collected = withReplaced(
      [
        [globalThis, 'TypeError', Fake],
        [globalThis, 'Error', Fake],
        [globalThis, 'Object', Fake],
        [globalThis, 'Array', Fake],
        [globalThis, 'String', Fake],
        [globalThis, 'Symbol', Fake],
        [globalThis, 'Reflect', {}],
        [globalThis, 'fetch', fail],
        [globalThis, 'Request', Fake],
        [globalThis, 'Navigator', Fake],
      ],
      () => exercise('http://sink/replaced-globals')
    );
    await verify(collected, '/replaced-globals');
  },
};

export const replacedStatics = {
  async test() {
    const collected = withReplaced(
      [
        [Object, 'create', fail],
        [Object, 'defineProperty', fail],
        [Object, 'defineProperties', fail],
        [Object, 'setPrototypeOf', fail],
        [Object, 'getOwnPropertyDescriptor', fail],
        [Object, 'freeze', fail],
        [Error, 'captureStackTrace', fail],
        [Error, 'prepareStackTrace', undefined],
        [Reflect, 'apply', fail],
        [Array, 'isArray', fail],
        [Array, 'of', fail],
        [Array, 'from', fail],
      ],
      () => exercise('http://sink/replaced-statics')
    );
    await verify(collected, '/replaced-statics');
  },
};

export const replacedPrototypeMethods = {
  async test() {
    const collected = withReplaced(
      [
        [Function.prototype, 'call', fail],
        [Function.prototype, 'apply', fail],
        [Function.prototype, 'bind', fail],
        [String.prototype, 'toString', fail],
        [String.prototype, 'valueOf', fail],
        [Array.prototype, 'push', fail],
        [Array.prototype, Symbol.iterator, fail],
        [Object.prototype, 'toString', fail],
        [Object.prototype, 'valueOf', fail],
        [Object.prototype, 'hasOwnProperty', fail],
        [NavigatorConstructor.prototype, 'constructor', Fake],
      ],
      () => {
        // sendBeacon() called as a method of the navigator need not use
        // Function.prototype.call, which is replaced here.
        const languages = hasLanguage ? navigator.languages : undefined;
        return {
          results: [
            navigator.userAgent === 'Cloudflare-Workers',
            navigator.hardwareConcurrency === 1,
            navigator.platform === '',
            !hasLanguage || navigator.language === 'en',
            !hasLanguage || (languages.length === 1 && languages[0] === 'en'),
            !hasLanguage || languages !== navigator.languages,
            navigator.sendBeacon(
              'http://sink/replaced-prototype-methods',
              'polluted'
            ) === true,
          ],
          errors: [
            caught(construct),
            caught(() =>
              NavigatorConstructor.prototype.sendBeacon('http://sink/')
            ),
            caught(missing),
            caught(symbolUrl),
            caught(symbolBody),
          ],
        };
      }
    );
    await verify(collected, '/replaced-prototype-methods');
  },
};

export const replacedMembers = {
  async test() {
    // The members user code can assign: the method goes on working through
    // a reference to it, and the instance through its own properties.
    const collected = withReplaced(
      [
        [NavigatorConstructor.prototype, 'sendBeacon', fail],
        [NavigatorConstructor.prototype, 'constructor', Fake],
        [NavigatorConstructor.prototype, Symbol.toStringTag, 'Replaced'],
        [NavigatorConstructor, 'name', 'Replaced'],
      ],
      () => {
        const languages = hasLanguage ? navigator.languages : undefined;
        return {
          results: [
            navigator.userAgent === 'Cloudflare-Workers',
            navigator.hardwareConcurrency === 1,
            navigator.platform === '',
            !hasLanguage || navigator.language === 'en',
            !hasLanguage || (languages.length === 1 && languages[0] === 'en'),
            !hasLanguage || languages !== navigator.languages,
            sendBeacon.call(
              navigator,
              'http://sink/replaced-members',
              'polluted'
            ) === true,
          ],
          errors: [
            caught(construct),
            caught(wrongReceiver),
            caught(() => sendBeacon.call(navigator)),
            caught(() => sendBeacon.call(navigator, kSymbol)),
            caught(() => sendBeacon.call(navigator, 'http://sink/', kSymbol)),
          ],
        };
      }
    );
    await verify(collected, '/replaced-members');
  },
};

export const objectPrototypeAccessors = {
  async test() {
    // Accessors on Object.prototype with the names of what the
    // implementation defines or reads: none of them runs.
    const names = [
      'value',
      'writable',
      'enumerable',
      'configurable',
      'get',
      'set',
      'method',
      'body',
      'stack',
      'message',
      'name',
      'then',
      'length',
      '0',
      'userAgent',
      'languages',
      'storage',
    ];
    const replacements = [];
    for (let i = 0; i < names.length; i++) {
      replacements[i] = [Object.prototype, names[i], undefined];
    }
    const saved = [];
    let collected;
    let ran = false;
    for (let i = 0; i < names.length; i++) {
      saved[i] = getOwnPropertyDescriptor(Object.prototype, names[i]);
      defineProperty(Object.prototype, names[i], {
        __proto__: null,
        get() {
          ran = true;
          return undefined;
        },
        set() {
          ran = true;
        },
        configurable: true,
      });
    }
    let stillOwn;
    try {
      collected = exercise('http://sink/object-prototype-accessors');
      // What an accessor on Object.prototype observes: nothing here reads
      // or assigns one of these names on an object that lacks it.
      stillOwn = getOwnPropertyDescriptor(navigator, 'userAgent').value;
    } finally {
      for (let i = names.length - 1; i >= 0; i--) {
        if (saved[i] === undefined) {
          deleteProperty(Object.prototype, names[i]);
        } else {
          defineProperty(Object.prototype, names[i], saved[i]);
        }
      }
    }
    strictEqual(stillOwn, 'Cloudflare-Workers');
    strictEqual(ran, false);
    await verify(collected, '/object-prototype-accessors');
  },
};
