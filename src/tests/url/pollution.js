// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// URL and URLSearchParams call nothing user code can patch on the
// built-ins, and make their results without running Object.prototype
// setters (ledger #9).

import { strictEqual, deepStrictEqual } from 'node:assert';
import { usingTsImpl, fastJsgStruct } from 'which-impl';

// The test's own bookkeeping uses captures and index loops, so that it does
// not trip the patches it installs.
const ReflectApply = Reflect.apply;
const ArrayPush = Array.prototype.push;
const ObjectDefineProperty = Object.defineProperty;

// Replaces each named member with a function that records the call, runs
// `body`, and restores the originals.
function withPatched(targets, body) {
  const calls = [];
  const saved = [];
  for (let i = 0; i < targets.length; i++) {
    const object = targets[i][0];
    const key = targets[i][1];
    const descriptor = Object.getOwnPropertyDescriptor(object, key);
    ReflectApply(ArrayPush, saved, [{ object, key, descriptor }]);
    ObjectDefineProperty(object, key, {
      configurable: true,
      writable: true,
      value: function () {
        ReflectApply(ArrayPush, calls, [String(key)]);
        return ReflectApply(descriptor.value, this, arguments);
      },
    });
  }
  try {
    body();
  } finally {
    for (let i = 0; i < saved.length; i++) {
      ObjectDefineProperty(saved[i].object, saved[i].key, saved[i].descriptor);
    }
  }
  return calls;
}

function exercise() {
  const url = new URL('http://u:p@a.com:81/p?b=2&a=1#h');
  void [url.href, url.origin, url.host, url.pathname, url.search, url.hash];
  url.pathname = '/q';
  url.search = 'c=3&a=1';
  const params = url.searchParams;
  params.append('d', '4');
  params.set('a', '5');
  params.delete('c');
  params.sort();
  void [params.get('a'), params.getAll('d'), params.has('b'), params.size];
  void params.toString();
  params.forEach(() => {});
  const iterators = [params.entries(), params.keys(), params.values()];
  for (let i = 0; i < iterators.length; i++) {
    for (let r = iterators[i].next(); !r.done; r = iterators[i].next());
  }
  return url.href;
}

export const builtinMethodsAreNotCalled = {
  test() {
    let href;
    const calls = withPatched(
      [
        [Array.prototype, 'push'],
        [Array.prototype, 'splice'],
        [Array.prototype, 'sort'],
        [Array.prototype, 'join'],
        [Array.prototype, Symbol.iterator],
        [String.prototype, 'slice'],
        [String.prototype, 'indexOf'],
        [String.prototype, 'replace'],
        [String.prototype, 'replaceAll'],
        [String.prototype, 'split'],
        [String.prototype, 'toWellFormed'],
        [String.prototype, 'charCodeAt'],
        [Object, 'defineProperty'],
        [Object, 'defineProperties'],
        [globalThis, 'encodeURIComponent'],
        [globalThis, 'decodeURIComponent'],
      ],
      () => {
        href = exercise();
      }
    );
    deepStrictEqual(calls, []);
    strictEqual(href, 'http://u:p@a.com:81/q?a=5&d=4#h');
  },
};

// Percent-decoding a name or value does not consult the species constructor
// of the bytes it decodes.
export const percentDecodingIgnoresSpecies = {
  test() {
    let looked = 0;
    const descriptor = Object.getOwnPropertyDescriptor(
      Uint8Array.prototype,
      'constructor'
    );
    ObjectDefineProperty(Uint8Array.prototype, 'constructor', {
      __proto__: null,
      configurable: true,
      get() {
        looked++;
        return {
          [Symbol.species]: function (buffer) {
            return new Uint8Array(buffer, 0, 1);
          },
        };
      },
    });
    let entries;
    let search;
    try {
      entries = [...new URLSearchParams('a%62=%41%42&c=d')];
      const url = new URL('http://a/?x=%41%42');
      void url.searchParams;
      url.searchParams.append('y', '1');
      search = url.search;
    } finally {
      ObjectDefineProperty(Uint8Array.prototype, 'constructor', descriptor);
    }
    deepStrictEqual(entries, [
      ['ab', 'AB'],
      ['c', 'd'],
    ]);
    strictEqual(search, '?x=AB&y=1');
    strictEqual(looked, 0);
  },
};

// Ledger #9: without enable_fast_jsg_struct, JSG makes an iterator result
// by assignment, so inherited setters for `done` and `value` capture both,
// and the result has no own properties. The TypeScript results are object
// literals.
export const resultsRunNoSetters = {
  test() {
    let hits = '';
    const targets = [
      [Object.prototype, ['0', '1', 'value', 'done']],
      [Array.prototype, ['0', '1']],
    ];
    let results = '';
    let all;
    try {
      for (let i = 0; i < targets.length; i++) {
        const [object, keys] = targets[i];
        for (let j = 0; j < keys.length; j++) {
          const key = keys[j];
          // A null-prototype descriptor, so that the setters installed so
          // far are not read as its fields.
          ObjectDefineProperty(object, key, {
            __proto__: null,
            configurable: true,
            set() {
              hits += `${object === Array.prototype ? 'array ' : ''}${key};`;
            },
          });
        }
      }
      // A fixed number of next() calls: under JSG `done` is captured too.
      const it = new URLSearchParams('a=1&b=2').entries();
      for (let i = 0; i < 3; i++) {
        const r = it.next();
        results += `${Reflect.ownKeys(r).join()};`;
      }
      hits += '/';
      all = new URL('http://a/?x=1&x=2').searchParams.getAll('x');
    } finally {
      for (let i = 0; i < targets.length; i++) {
        const [object, keys] = targets[i];
        for (let j = 0; j < keys.length; j++) {
          Reflect.deleteProperty(object, keys[j]);
        }
      }
    }
    deepStrictEqual(all, ['1', '2']);
    if (usingTsImpl) {
      strictEqual(results, 'value,done;value,done;value,done;');
      strictEqual(hits, '/');
    } else if (fastJsgStruct) {
      strictEqual(results, 'done,value;done,value;done,value;');
      strictEqual(hits, '/');
    } else {
      strictEqual(results, ';;;');
      strictEqual(hits, 'done;value;done;value;done;/');
    }
  },
};
