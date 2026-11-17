// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// URLSearchParams on its own: the initializer union, the list operations,
// delete() and has() with a value, sort() and serialization.

import { throws, strictEqual, deepStrictEqual } from 'node:assert';
import { usingTsImpl, urlStandard, deleteHasValueArg } from 'which-impl';

// The legacy C++ initializer is OneOf<URLSearchParams, string, Dict, Array
// of Arrays>; the standard one and the TypeScript one are WebIDL's union.
const legacyCpp = !urlStandard && !usingTsImpl;

const str = (init) => new URLSearchParams(init).toString();

export const sequenceInitializers = {
  test() {
    strictEqual(
      str([
        ['a', '1'],
        ['b', 2],
      ]),
      'a=1&b=2'
    );
    strictEqual(str(new URLSearchParams('a=1&b=2')), 'a=1&b=2');
    strictEqual(str([[undefined, null]]), 'undefined=null');
    strictEqual(str([[1, {}]]), '1=%5Bobject+Object%5D');
    // Ledger #16: the legacy C++ reads a non-array as a dict of its own
    // enumerable properties, and never reads @@iterator.
    const iterables = [
      new Map([['a', '1']]),
      new Set([['a', '1']]),
      {
        *[Symbol.iterator]() {
          yield ['a', '1'];
        },
      },
    ];
    for (const init of iterables) {
      strictEqual(str(init), legacyCpp ? '' : 'a=1');
    }
    if (legacyCpp) {
      throws(() => str([new Set(['a', 'b'])]), TypeError);
    } else {
      strictEqual(str([new Set(['a', 'b'])]), 'a=b');
    }
  },
};

export const badSequences = {
  test() {
    const message = usingTsImpl
      ? undefined
      : urlStandard
        ? 'Invalid URL search params sequence.'
        : 'To initialize a URLSearchParams object from an array-of-arrays, ' +
          'each inner array must have exactly two elements.';
    for (const init of [[['a']], [['a', 'b', 'c']], [[]]]) {
      throws(
        () => str(init),
        message ? { name: 'TypeError', message } : TypeError
      );
    }
    // A string element is not a sequence.
    throws(() => str(['ab']), TypeError);
    // Ledger #16: a String object is iterable, so it is a sequence whose
    // elements are not.
    if (legacyCpp) {
      strictEqual(str(new String('a=1')), '0=a&1=%3D&2=1');
    } else {
      throws(() => str(new String('a=1')), TypeError);
    }
    // Ledger #21.
    if (usingTsImpl) {
      throws(() => str({ [Symbol.iterator]: 1 }), TypeError);
    } else {
      strictEqual(str({ [Symbol.iterator]: 1 }), '');
    }
  },
};

export const sequenceIteratorProtocol = {
  test() {
    // Ledger #16: the legacy C++ reads these iterables as empty dicts.
    // The whole sequence converts before any pair's length is checked, so
    // the iterator's own error wins over the bad pair before it.
    const throwing = {
      *[Symbol.iterator]() {
        yield ['a'];
        throw new RangeError('iterator failed');
      },
    };
    if (legacyCpp) {
      strictEqual(str(throwing), '');
    } else {
      throws(() => str(throwing), {
        name: 'RangeError',
        message: 'iterator failed',
      });
    }

    // A truthy `done` ends the iteration. Ledger #27: the standard C++
    // also appends the value of that final result, as it does a
    // generator's return value.
    const truthyDone = {
      [Symbol.iterator]() {
        let calls = 0;
        return {
          next() {
            calls++;
            if (calls > 2) throw new Error('iterated past done');
            return calls === 1
              ? { done: false, value: ['a', '1'] }
              : { done: 1, value: ['b', '2'] };
          },
        };
      },
    };
    const returning = {
      *[Symbol.iterator]() {
        yield ['a', '1'];
        return ['b', '2'];
      },
    };
    for (const init of [truthyDone, returning]) {
      strictEqual(str(init), legacyCpp ? '' : usingTsImpl ? 'a=1' : 'a=1&b=2');
    }
  },
};

export const recordInitializers = {
  test() {
    strictEqual(str({ a: 1, b: 'x' }), 'a=1&b=x');
    // Ledger #26: WebIDL converts every own enumerable key, and a Symbol
    // does not convert to a string; JSG skips them. An undefined
    // Symbol.iterator makes the object a record with such a key.
    for (const init of [
      { [Symbol('s')]: '1', a: '2' },
      { [Symbol.iterator]: undefined, a: '2' },
    ]) {
      if (usingTsImpl) {
        throws(() => str(init), TypeError);
      } else {
        strictEqual(str(init), 'a=2');
      }
    }
    strictEqual(
      str(
        Object.defineProperty({ a: '2' }, Symbol('hidden'), {
          value: '1',
          enumerable: false,
        })
      ),
      'a=2'
    );
    strictEqual(
      str(
        Object.defineProperty({ b: '2' }, 'a', {
          value: '1',
          enumerable: false,
        })
      ),
      'b=2'
    );
    strictEqual(
      str(() => 1),
      ''
    );
    strictEqual(str(Object.create({ inherited: '1' })), '');
  },
};

export const recordReadOrder = {
  test() {
    const log = [];
    const init = new Proxy(
      { a: '1', b: '2' },
      {
        ownKeys(t) {
          log.push('ownKeys');
          return Reflect.ownKeys(t);
        },
        getOwnPropertyDescriptor(t, k) {
          log.push(`gopd ${String(k)}`);
          return Reflect.getOwnPropertyDescriptor(t, k);
        },
        get(t, k) {
          log.push(`get ${String(k)}`);
          return Reflect.get(t, k);
        },
      }
    );
    strictEqual(str(init), 'a=1&b=2');
    // Ledger #25: WebIDL reads each key's descriptor and then its value; JSG
    // reads every descriptor first.
    const record = usingTsImpl
      ? ['ownKeys', 'gopd a', 'get a', 'gopd b', 'get b']
      : ['ownKeys', 'gopd a', 'gopd b', 'get a', 'get b'];
    deepStrictEqual(
      log,
      legacyCpp ? record : ['get Symbol(Symbol.iterator)', ...record]
    );
  },
};

export const stringInitializers = {
  test() {
    strictEqual(str('?a=1&&b=&=c&d'), 'a=1&b=&=c&d=');
    strictEqual(str('a&b&c=1=2&d&e='), 'a=&b=&c=1%3D2&d=&e=');
    strictEqual(str('??a=1'), '%3Fa=1');
    // The legacy backend's parseQueryString() drops a malformed escape's
    // `%`; the facade keeps each backend's string parsing.
    strictEqual(
      str('a=b+c%20d&e=%zz'),
      urlStandard ? 'a=b+c+d&e=%25zz' : 'a=b+c+d&e=zz'
    );
    strictEqual(str(12), '12=');
    strictEqual(str(''), '');
  },
};

export const listOperations = {
  test() {
    const params = new URLSearchParams('a=1&b=2&a=3');
    strictEqual(params.size, 3);
    strictEqual(params.get('a'), '1');
    strictEqual(params.get('z'), null);
    deepStrictEqual(params.getAll('a'), ['1', '3']);
    deepStrictEqual(params.getAll('z'), []);
    strictEqual(params.has('b'), true);
    params.set('a', '4');
    strictEqual(params.toString(), 'a=4&b=2');
    params.set('c', '5');
    strictEqual(params.toString(), 'a=4&b=2&c=5');
    params.append('a', 6);
    strictEqual(params.toString(), 'a=4&b=2&c=5&a=6');
    params.delete('a');
    strictEqual(params.toString(), 'b=2&c=5');
    strictEqual(params.size, 2);
  },
};

export const deleteAndHasWithAValue = {
  test() {
    // Ledger #23: the legacy C++ ignores the value under the flag too.
    const honored = deleteHasValueArg && (urlStandard || usingTsImpl);
    const params = new URLSearchParams('a=1&a=2');
    params.delete('a', '1');
    strictEqual(params.toString(), honored ? 'a=2' : '');

    strictEqual(new URLSearchParams('a=1').has('a', '2'), !honored);
    strictEqual(new URLSearchParams('a=1').has('a', '1'), true);
    strictEqual(new URLSearchParams('a=null').has('a', null), true);

    // An undefined value is an absent one.
    const all = new URLSearchParams('a=1&a=2');
    all.delete('a', undefined);
    strictEqual(all.toString(), '');
    strictEqual(new URLSearchParams('a=1').has('a', undefined), true);
  },
};

export const sortIsStableByCodeUnits = {
  test() {
    const params = new URLSearchParams(
      'z=1&a=2&\uFFFD=3&a=1&\u{1F600}=4&\uFB03=5'
    );
    params.sort();
    deepStrictEqual(
      [...params],
      [
        ['a', '2'],
        ['a', '1'],
        ['z', '1'],
        ['\u{1F600}', '4'],
        ['\uFB03', '5'],
        ['\uFFFD', '3'],
      ]
    );
  },
};

export const serialization = {
  test() {
    strictEqual(
      str([
        ['a b', 'c+d'],
        ['\u00E9', '~*-._'],
        ['k', ''],
        ['', ''],
        ['&=', '#%'],
      ]),
      'a+b=c%2Bd&%C3%A9=%7E*-._&k=&=&%26%3D=%23%25'
    );
  },
};

export const missingArgumentsThrow = {
  test() {
    // Ledger #7: JSG already rejects a missing required argument.
    const type = urlStandard || usingTsImpl ? 'USVString' : 'string';
    const params = new URLSearchParams('undefined=1');
    const cases = [
      ['append', 1, () => params.append()],
      ['append', 2, () => params.append('a')],
      ['delete', 1, () => params.delete()],
      ['get', 1, () => params.get()],
      ['getAll', 1, () => params.getAll()],
      ['has', 1, () => params.has()],
      ['set', 1, () => params.set()],
      ['set', 2, () => params.set('a')],
    ];
    for (const [name, index, call] of cases) {
      throws(
        call,
        usingTsImpl
          ? { name: 'TypeError' }
          : {
              name: 'TypeError',
              message: `Failed to execute '${name}' on 'URLSearchParams': parameter ${index} is not of type '${type}'.`,
            }
      );
    }
    strictEqual(params.toString(), 'undefined=1');
    // An explicit undefined is converted.
    strictEqual(params.get(undefined), '1');
    params.append('a', undefined);
    strictEqual(params.get('a'), 'undefined');
  },
};
