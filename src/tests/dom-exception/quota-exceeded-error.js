// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

/* global QuotaExceededError */

// QuotaExceededError (WebIDL section 2.8.3), the DOMException subclass that
// only the TypeScript implementation has (ledger #14). It is cloneable as a
// class implemented in JavaScript; dom-exception-bootstrap-test.c++ pins the
// bytes, and what reading them does with either implementation.

import { deepStrictEqual, ok, rejects, strictEqual, throws } from 'node:assert';
import { env } from 'cloudflare:workers';
import {
  setToStringTag,
  specCompliantPropertyAttributes,
  usingTsImpl,
} from 'which-impl';
import { assertDomException, attributesOf } from 'helpers';

// Asserts `error` is a QuotaExceededError as
// `new QuotaExceededError(message, { quota, requested })` would make it, but
// for the keys in `extra` and its stack's frames, and its stack's attributes:
// a deserialized one's is a data property, as a DOMException's is
// (serialization.js).
function assertQuotaExceededError(
  error,
  { message, quota, requested },
  ...extra
) {
  ok(error instanceof QuotaExceededError);
  strictEqual(Object.getPrototypeOf(error), QuotaExceededError.prototype);
  strictEqual(error.constructor, QuotaExceededError);
  deepStrictEqual(
    [error.name, error.message, error.code, error.quota, error.requested],
    ['QuotaExceededError', message, 22, quota, requested]
  );
  strictEqual(Error.isError(error), true);
  deepStrictEqual(Reflect.ownKeys(error).map(String), ['stack', ...extra]);
}

export const quotaExceededErrorPlacement = {
  test() {
    if (!usingTsImpl) {
      strictEqual('QuotaExceededError' in globalThis, false);
      return;
    }
    // Where DOMException is (api-surface.js's globalPlacement).
    strictEqual(
      attributesOf(Object.getPrototypeOf(globalThis), 'QuotaExceededError'),
      'datawec'
    );
    strictEqual(
      attributesOf(globalThis, 'QuotaExceededError'),
      specCompliantPropertyAttributes ? 'datawc' : 'none'
    );
  },
};

export const quotaExceededErrorShape = {
  test() {
    if (!usingTsImpl) return;
    strictEqual(QuotaExceededError.name, 'QuotaExceededError');
    strictEqual(QuotaExceededError.length, 0);
    strictEqual(Object.getPrototypeOf(QuotaExceededError), DOMException);
    strictEqual(attributesOf(QuotaExceededError, 'prototype'), 'data');
    // The constants are DOMException's, inherited.
    strictEqual(Object.hasOwn(QuotaExceededError, 'QUOTA_EXCEEDED_ERR'), false);
    strictEqual(QuotaExceededError.QUOTA_EXCEEDED_ERR, 22);

    const proto = QuotaExceededError.prototype;
    strictEqual(Object.getPrototypeOf(proto), DOMException.prototype);
    deepStrictEqual(Reflect.ownKeys(proto).map(String), [
      'constructor',
      'quota',
      'requested',
      'Symbol(cloudflare:internal-class)',
      ...(setToStringTag ? ['Symbol(Symbol.toStringTag)'] : []),
    ]);
    strictEqual(attributesOf(proto, 'constructor'), 'datawc');
    for (const attribute of ['quota', 'requested']) {
      strictEqual(attributesOf(proto, attribute), 'accessorec');
      strictEqual(
        Object.getOwnPropertyDescriptor(proto, attribute).set,
        undefined
      );
    }
    strictEqual(
      Object.prototype.toString.call(new QuotaExceededError()),
      setToStringTag ? '[object QuotaExceededError]' : '[object Object]'
    );
  },
};

export const quotaExceededErrorInstances = {
  test() {
    if (!usingTsImpl) return;
    const error = new QuotaExceededError();
    ok(error instanceof DOMException);
    ok(error instanceof Error);
    assertQuotaExceededError(error, {
      message: '',
      quota: null,
      requested: null,
    });
    strictEqual(String(error), 'QuotaExceededError');

    const full = new QuotaExceededError('m', { quota: 10, requested: 20 });
    assertQuotaExceededError(full, { message: 'm', quota: 10, requested: 20 });
    strictEqual(String(full), 'QuotaExceededError: m');
    ok(full.stack.startsWith('QuotaExceededError: m\n'), full.stack);

    const amounts = (options) => {
      const e = new QuotaExceededError('m', options);
      return [e.quota, e.requested];
    };
    deepStrictEqual(amounts({ quota: 1 }), [1, null]);
    deepStrictEqual(amounts({ requested: 1 }), [null, 1]);
    deepStrictEqual(amounts({ quota: 5, requested: 5 }), [5, 5]);
    deepStrictEqual(amounts({ quota: 0.5, requested: 1.5 }), [0.5, 1.5]);
    deepStrictEqual(amounts(null), [null, null]);
    deepStrictEqual(amounts(undefined), [null, null]);
    deepStrictEqual(amounts({}), [null, null]);
    ok(Object.is(amounts({ quota: -0 })[0], -0));

    throws(() => QuotaExceededError(), TypeError);
  },
};

export const quotaExceededErrorConversion = {
  test() {
    if (!usingTsImpl) return;
    // The message, then the dictionary's members in lexicographic order,
    // each converted as it is read.
    const log = [];
    const amount = (name, value) => ({
      valueOf() {
        log.push(`${name} valueOf`);
        return value;
      },
    });
    const message = {
      toString() {
        log.push('message');
        return 'm';
      },
    };
    const options = {
      get requested() {
        log.push('requested');
        return amount('requested', 2);
      },
      get quota() {
        log.push('quota');
        return amount('quota', 1);
      },
    };
    const error = new QuotaExceededError(message, options);
    deepStrictEqual(log, [
      'message',
      'quota',
      'quota valueOf',
      'requested',
      'requested valueOf',
    ]);
    deepStrictEqual([error.message, error.quota, error.requested], ['m', 1, 2]);

    // The message is converted before the options are checked.
    log.length = 0;
    throws(() => new QuotaExceededError(message, 1), TypeError);
    deepStrictEqual(log, ['message']);
    throws(() => new QuotaExceededError(Symbol('s')), TypeError);

    // Members are read with [[Get]], from functions too, and converted with
    // ToNumber: a member that is null is 0, not absent.
    const amounts = (options) => {
      const e = new QuotaExceededError('m', options);
      return [e.quota, e.requested];
    };
    deepStrictEqual(amounts(Object.create({ quota: 4 })), [4, null]);
    deepStrictEqual(amounts(Object.assign(() => {}, { quota: 3 })), [3, null]);
    deepStrictEqual(amounts({ quota: null, requested: '5' }), [0, 5]);

    for (const options of [1, 'x', true, Symbol('s'), 1n]) {
      throws(() => new QuotaExceededError('m', options), TypeError);
    }
    for (const value of [NaN, Infinity, -Infinity, 'x', {}, 1n, Symbol('s')]) {
      throws(() => new QuotaExceededError('m', { quota: value }), TypeError);
      throws(
        () => new QuotaExceededError('m', { requested: value }),
        TypeError
      );
    }
    // Every member is converted before the constructor's own checks.
    throws(
      () => new QuotaExceededError('m', { quota: -1, requested: NaN }),
      TypeError
    );
  },
};

export const quotaExceededErrorRangeErrors = {
  test() {
    if (!usingTsImpl) return;
    for (const options of [
      { quota: -1 },
      { quota: -0.5 },
      { requested: -1 },
      { quota: 2, requested: 1 },
    ]) {
      throws(() => new QuotaExceededError('m', options), RangeError);
    }
  },
};

export const quotaExceededErrorBrandChecks = {
  test() {
    if (!usingTsImpl) return;
    const proto = QuotaExceededError.prototype;
    const getters = ['quota', 'requested'].map(
      (attribute) => Object.getOwnPropertyDescriptor(proto, attribute).get
    );
    for (const receiver of [
      new DOMException('m', 'QuotaExceededError'),
      Object.create(proto),
      {},
      null,
      undefined,
    ]) {
      for (const getter of getters) {
        throws(() => getter.call(receiver), TypeError);
      }
    }

    // Read-only; a module is strict.
    const error = new QuotaExceededError('m', { quota: 1, requested: 2 });
    throws(() => {
      error.quota = 5;
    }, TypeError);
    strictEqual(error.quota, 1);

    // DOMException's attributes accept it.
    const nameGetter = Object.getOwnPropertyDescriptor(
      DOMException.prototype,
      'name'
    ).get;
    strictEqual(nameGetter.call(error), 'QuotaExceededError');

    class Sub extends QuotaExceededError {}
    const sub = new Sub('m', { quota: 1 });
    ok(sub instanceof Sub);
    deepStrictEqual(
      [sub.name, sub.code, sub.quota],
      ['QuotaExceededError', 22, 1]
    );
  },
};

export const quotaExceededErrorCloned = {
  async test() {
    if (!usingTsImpl) return;
    const error = new QuotaExceededError('m', {
      quota: 0.1,
      requested: Number.MAX_VALUE,
    });
    const clone = structuredClone(error);
    assertQuotaExceededError(clone, {
      message: 'm',
      quota: 0.1,
      requested: Number.MAX_VALUE,
    });
    strictEqual(clone.stack, error.stack);
    strictEqual(attributesOf(clone, 'stack'), 'datawc');
    strictEqual(
      Object.prototype.toString.call(clone),
      Object.prototype.toString.call(error)
    );

    assertQuotaExceededError(structuredClone(new QuotaExceededError()), {
      message: '',
      quota: null,
      requested: null,
    });
    ok(
      Object.is(
        structuredClone(new QuotaExceededError('m', { quota: -0 })).quota,
        -0
      )
    );

    // Its own name, message and amounts, not properties that shadow them,
    // and nothing else.
    const shadowed = new QuotaExceededError('m', { quota: 1 });
    Object.defineProperty(shadowed, 'message', { value: 'other' });
    Object.defineProperty(shadowed, 'quota', { value: 5 });
    shadowed.extra = 1;
    assertQuotaExceededError(structuredClone(shadowed), {
      message: 'm',
      quota: 1,
      requested: null,
    });

    // A subclass's instance clones as a QuotaExceededError, and a
    // DOMException named QuotaExceededError as a DOMException.
    class Sub extends QuotaExceededError {}
    assertQuotaExceededError(structuredClone(new Sub('m', { quota: 1 })), {
      message: 'm',
      quota: 1,
      requested: null,
    });
    assertDomException(
      structuredClone(new DOMException('m', 'QuotaExceededError')),
      { name: 'QuotaExceededError', message: 'm', code: 22 }
    );

    const nested = structuredClone({ errors: [error, error] });
    strictEqual(nested.errors[0], nested.errors[1]);
    strictEqual(nested.errors[0].quota, 0.1);

    const { port1, port2 } = new MessageChannel();
    try {
      const received = new Promise((resolve) => {
        port2.onmessage = ({ data }) => resolve(data);
      });
      port1.postMessage(error);
      assertQuotaExceededError(await received, {
        message: 'm',
        quota: 0.1,
        requested: Number.MAX_VALUE,
      });
    } finally {
      port1.close();
      port2.close();
    }
  },
};

export const quotaExceededErrorOverRpc = {
  async test() {
    if (!usingTsImpl) return;
    // RPC gives the objects it returns a disposer.
    assertQuotaExceededError(
      await env.RPC.returnQuotaExceededError(),
      { message: 'returned', quota: 1, requested: 2 },
      'Symbol(Symbol.dispose)'
    );
    deepStrictEqual(
      await env.RPC.describeQuotaExceededError(
        new QuotaExceededError('sent', { requested: 3 })
      ),
      [true, 'sent', null, 3]
    );
    // A thrown one tunnels as any DOMException does, by its name and
    // message.
    await rejects(env.RPC.throwQuotaExceededError(), (error) => {
      assertDomException(
        error,
        { name: 'QuotaExceededError', message: 'thrown', code: 22 },
        'remote'
      );
      return true;
    });
  },
};
