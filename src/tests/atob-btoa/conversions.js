// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The receiver and the argument: what is accepted, what is converted and in
// which order.

import { deepStrictEqual, strictEqual, throws } from 'node:assert';
import { kInvalidBase64, throwsInvalidCharacter } from 'helpers';

const kIllegalInvocation = {
  name: 'TypeError',
  message:
    'Illegal invocation: function called with incorrect `this` reference. ' +
    'See https://developers.cloudflare.com/workers/observability/errors/' +
    '#illegal-invocation-errors for details.',
};

// JSG names the types differently.
const kMissingArgument = {
  atob: {
    name: 'TypeError',
    message:
      "Failed to execute 'atob' on 'ServiceWorkerGlobalScope': parameter 1 " +
      "is not of type 'string'.",
  },
  btoa: {
    name: 'TypeError',
    message:
      "Failed to execute 'btoa' on 'ServiceWorkerGlobalScope': parameter 1 " +
      "is not of type 'String'.",
  },
};

const kSymbol = {
  name: 'TypeError',
  message: 'Cannot convert a Symbol value to a string',
};

export const receiver = {
  test() {
    // The global, or nothing.
    for (const receiver of [undefined, null, globalThis, self]) {
      strictEqual(atob.call(receiver, 'YWJj'), 'abc');
      strictEqual(btoa.call(receiver, 'abc'), 'YWJj');
      strictEqual(Reflect.apply(atob, receiver, ['YWJj']), 'abc');
      strictEqual(Reflect.apply(btoa, receiver, ['abc']), 'YWJj');
    }
    strictEqual(globalThis.atob('YWJj'), 'abc');
    strictEqual(self.btoa('abc'), 'YWJj');
    strictEqual((0, atob)('YWJj'), 'abc');
    strictEqual(atob.bind(undefined)('YWJj'), 'abc');
    strictEqual(['YWJj'].map(atob)[0], 'abc');

    const receivers = [
      {},
      [],
      () => {},
      1,
      0,
      '',
      'string',
      true,
      false,
      1n,
      Symbol('receiver'),
      Object.create(globalThis),
      Object.getPrototypeOf(globalThis),
      Object.create(null),
      new Proxy(globalThis, {}),
      atob,
    ];
    for (const receiver of receivers) {
      throws(() => atob.call(receiver, 'YWJj'), kIllegalInvocation);
      throws(() => btoa.call(receiver, 'abc'), kIllegalInvocation);
    }
    throws(() => ({ atob }).atob('YWJj'), kIllegalInvocation);
    throws(() => ({ btoa }).btoa('abc'), kIllegalInvocation);
  },
};

export const receiverIsCheckedFirst = {
  test() {
    // Before the argument is counted or converted.
    let converted = false;
    const argument = {
      toString() {
        converted = true;
        return '';
      },
    };
    for (const fn of [atob, btoa]) {
      throws(() => fn.call({}), kIllegalInvocation);
      throws(() => fn.call({}, argument), kIllegalInvocation);
      throws(() => fn.call({}, Symbol()), kIllegalInvocation);
    }
    strictEqual(converted, false);
  },
};

export const missingArgument = {
  test() {
    throws(() => atob(), kMissingArgument.atob);
    throws(() => btoa(), kMissingArgument.btoa);
    throws(() => atob.call(undefined), kMissingArgument.atob);
    throws(() => btoa.apply(null, []), kMissingArgument.btoa);
    throws(() => atob(...[]), kMissingArgument.atob);
  },
};

export const undefinedIsConverted = {
  test() {
    // An argument that is present is converted, undefined included.
    strictEqual(btoa(undefined), 'dW5kZWZpbmVk');
    strictEqual(btoa(null), 'bnVsbA==');
    throwsInvalidCharacter(() => atob(undefined), kInvalidBase64);
    // 'null' is four characters of base64.
    strictEqual(atob(null), '\x9e\xe9\x65');
    strictEqual(atob(null), atob('null'));
  },
};

export const primitives = {
  test() {
    strictEqual(btoa(1234), 'MTIzNA==');
    strictEqual(btoa(-0), 'MA==');
    strictEqual(btoa(1.5), 'MS41');
    strictEqual(btoa(NaN), 'TmFO');
    strictEqual(btoa(true), 'dHJ1ZQ==');
    strictEqual(btoa(false), 'ZmFsc2U=');
    strictEqual(btoa(12n), 'MTI=');
    strictEqual(atob(1234), atob('1234'));
    strictEqual(atob(true), atob('true'));
    strictEqual(atob(12n), atob('12'));
    strictEqual(atob(12345678), atob('12345678'));
    throwsInvalidCharacter(() => atob(false), kInvalidBase64);
    throwsInvalidCharacter(() => atob(1.5), kInvalidBase64);
  },
};

export const objects = {
  test() {
    strictEqual(btoa({}), 'W29iamVjdCBPYmplY3Rd');
    strictEqual(btoa([]), '');
    strictEqual(btoa(['a', 'b']), 'YSxi');
    strictEqual(btoa(new String('abc')), 'YWJj');
    strictEqual(btoa(Object(1n)), 'MQ==');
    strictEqual(atob([]), '');
    strictEqual(atob(['YWJj']), 'abc');
    strictEqual(atob(new String('YWJj')), 'abc');
    strictEqual(atob({ toString: () => 'YWJj' }), 'abc');
    throwsInvalidCharacter(() => atob({}), kInvalidBase64);
    // A function's source is not base64; a Date's string has ':' in it.
    throwsInvalidCharacter(() => atob(() => {}), kInvalidBase64);
    throwsInvalidCharacter(() => atob(new Date(0)), kInvalidBase64);
  },
};

export const conversionOrder = {
  test() {
    // ToString: @@toPrimitive with the hint 'string', else toString() and
    // then valueOf(), each read and called once.
    for (const [fn, input, expected] of [
      [atob, 'YWJj', 'abc'],
      [btoa, 'abc', 'YWJj'],
    ]) {
      const log = [];
      const logged = (target) =>
        new Proxy(target, {
          get(object, key, proxy) {
            log.push(`get ${String(key)}`);
            return Reflect.get(object, key, proxy);
          },
        });

      strictEqual(
        fn(
          logged({
            [Symbol.toPrimitive](hint) {
              log.push(`toPrimitive ${hint}`);
              return input;
            },
            toString() {
              log.push('toString');
              return 'unused';
            },
          })
        ),
        expected
      );
      deepStrictEqual(log, [
        'get Symbol(Symbol.toPrimitive)',
        'toPrimitive string',
      ]);

      log.length = 0;
      strictEqual(
        fn(
          logged({
            toString() {
              log.push('toString');
              return {};
            },
            valueOf() {
              log.push('valueOf');
              return input;
            },
          })
        ),
        expected
      );
      deepStrictEqual(log, [
        'get Symbol(Symbol.toPrimitive)',
        'get toString',
        'toString',
        'get valueOf',
        'valueOf',
      ]);

      // A primitive that is not a string is converted in turn.
      strictEqual(fn({ toString: () => 12 }), fn('12'));
      strictEqual(fn({ [Symbol.toPrimitive]: () => null }), fn('null'));
    }
  },
};

export const symbols = {
  test() {
    for (const fn of [atob, btoa]) {
      throws(() => fn(Symbol('YWJj')), kSymbol);
      throws(() => fn(Symbol.iterator), kSymbol);
      throws(() => fn(Object(Symbol())), kSymbol);
      throws(() => fn({ toString: () => Symbol() }), kSymbol);
      throws(() => fn({ [Symbol.toPrimitive]: () => Symbol() }), kSymbol);
    }
  },
};

export const unconvertibleObjects = {
  test() {
    for (const fn of [atob, btoa]) {
      throws(() => fn(Object.create(null)), {
        name: 'TypeError',
        message: 'Cannot convert object to primitive value',
      });
      throws(() => fn({ toString: () => ({}), valueOf: () => ({}) }), {
        name: 'TypeError',
        message: 'Cannot convert object to primitive value',
      });
      throws(() => fn({ [Symbol.toPrimitive]: 1 }), TypeError);
    }
  },
};

export const conversionErrorsPropagate = {
  test() {
    // Whatever the conversion throws, as it is.
    for (const fn of [atob, btoa]) {
      for (const thrown of [new RangeError('boom'), 'a string', undefined, 0]) {
        let caught;
        let threw = false;
        try {
          fn({
            toString() {
              throw thrown;
            },
          });
        } catch (error) {
          threw = true;
          caught = error;
        }
        strictEqual(threw, true);
        strictEqual(caught, thrown);
      }
      const revoked = Proxy.revocable({}, {});
      revoked.revoke();
      throws(() => fn(revoked.proxy), TypeError);
    }
  },
};

export const extraArgumentsAreIgnored = {
  test() {
    const untouched = new Proxy(
      {},
      {
        get() {
          throw new Error('read');
        },
        has() {
          throw new Error('read');
        },
        ownKeys() {
          throw new Error('read');
        },
      }
    );
    strictEqual(atob('YWJj', untouched, untouched), 'abc');
    strictEqual(btoa('abc', untouched, untouched), 'YWJj');
    // Options that other base64 APIs take mean nothing here.
    throwsInvalidCharacter(
      () => atob('YW-_', { alphabet: 'base64url' }),
      kInvalidBase64
    );
    strictEqual(btoa('\xfb\xff', { alphabet: 'base64url' }), '+/8=');
    strictEqual(btoa('a', { omitPadding: true }), 'YQ==');
  },
};

export const stringsAreNotConverted = {
  test() {
    // A string primitive runs no user code: String.prototype is not
    // consulted.
    const descriptors = Object.getOwnPropertyDescriptors(String.prototype);
    const fail = () => {
      throw new Error('called');
    };
    String.prototype.toString = fail;
    String.prototype.valueOf = fail;
    String.prototype[Symbol.toPrimitive] = fail;
    try {
      strictEqual(atob('YWJj'), 'abc');
      strictEqual(btoa('abc'), 'YWJj');
    } finally {
      Reflect.deleteProperty(String.prototype, Symbol.toPrimitive);
      Object.defineProperties(String.prototype, {
        toString: descriptors.toString,
        valueOf: descriptors.valueOf,
      });
    }
  },
};
