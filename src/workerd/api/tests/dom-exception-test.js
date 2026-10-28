// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The global DOMException is a TypeScript class (src/per_isolate/dom-exception.ts). These tests
// pin the shape it shares with the C++ class it replaced, and check that the DOMExceptions the
// runtime creates are instances of it.

import { deepStrictEqual, ok, strictEqual, throws } from 'node:assert';
import { types } from 'node:util';

const describe = (object, key) => {
  const { value, get, set, ...attributes } = Object.getOwnPropertyDescriptor(
    object,
    key
  );
  return { ...attributes, accessor: get !== undefined && set !== undefined };
};

const DATA = {
  writable: false,
  enumerable: true,
  configurable: true,
  accessor: false,
};

const CODES = [
  'INDEX_SIZE_ERR',
  'DOMSTRING_SIZE_ERR',
  'HIERARCHY_REQUEST_ERR',
  'WRONG_DOCUMENT_ERR',
  'INVALID_CHARACTER_ERR',
  'NO_DATA_ALLOWED_ERR',
  'NO_MODIFICATION_ALLOWED_ERR',
  'NOT_FOUND_ERR',
  'NOT_SUPPORTED_ERR',
  'INUSE_ATTRIBUTE_ERR',
  'INVALID_STATE_ERR',
  'SYNTAX_ERR',
  'INVALID_MODIFICATION_ERR',
  'NAMESPACE_ERR',
  'INVALID_ACCESS_ERR',
  'VALIDATION_ERR',
  'TYPE_MISMATCH_ERR',
  'SECURITY_ERR',
  'NETWORK_ERR',
  'ABORT_ERR',
  'URL_MISMATCH_ERR',
  'QUOTA_EXCEEDED_ERR',
  'TIMEOUT_ERR',
  'INVALID_NODE_TYPE_ERR',
  'DATA_CLONE_ERR',
];

function checkInstance(e, name, message, code) {
  ok(e instanceof DOMException);
  ok(e instanceof Error);
  ok(!types.isNativeError(e));
  deepStrictEqual(Reflect.ownKeys(e), ['code', 'name', 'message', 'stack']);
  deepStrictEqual(describe(e, 'code'), DATA);
  deepStrictEqual(describe(e, 'name'), DATA);
  deepStrictEqual(describe(e, 'message'), DATA);
  strictEqual(describe(e, 'stack').enumerable, true);
  strictEqual(e.name, name);
  strictEqual(e.message, message);
  strictEqual(e.code, code);
  // DOMException.prototype has a Symbol.toStringTag under the set_tostring_tag flag.
  strictEqual(
    Object.prototype.toString.call(e),
    Symbol.toStringTag in DOMException.prototype
      ? '[object DOMException]'
      : '[object Object]'
  );
}

export const constructed = {
  test() {
    const e = new DOMException('msg', 'AbortError');
    checkInstance(e, 'AbortError', 'msg', 20);
    strictEqual(describe(e, 'stack').accessor, true);
    // The stack starts with the caller's frame, not the constructor's.
    const [header, frame] = e.stack.split('\n');
    strictEqual(header, 'AbortError: msg');
    ok(frame.includes('Object.test'), frame);

    checkInstance(new DOMException(), 'Error', '', 0);
    checkInstance(new DOMException(null, undefined), 'Error', 'null', 0);
    checkInstance(new DOMException(undefined, null), 'null', '', 0);
    checkInstance(
      new DOMException(
        { toString: () => 'm' },
        { toString: () => 'DataError' }
      ),
      'DataError',
      'm',
      0
    );
    throws(() => new DOMException(Symbol()), TypeError);
    throws(() => DOMException('m'), {
      name: 'TypeError',
      message:
        "Failed to construct 'DOMException': Please use the 'new' operator, this object constructor cannot be called as a function.",
    });

    throws(() => {
      e.name = 'x';
    }, TypeError);
    strictEqual(
      JSON.stringify(e).startsWith('{"code":20,"name":"AbortError"'),
      true
    );
  },
};

export const subclassed = {
  test() {
    class Sub extends DOMException {
      constructor() {
        super('s', 'NotFoundError');
        this.extra = 1;
      }
    }
    const s = new Sub();
    ok(s instanceof Sub);
    ok(s instanceof DOMException);
    deepStrictEqual(Reflect.ownKeys(s), [
      'code',
      'name',
      'message',
      'stack',
      'extra',
    ]);
    strictEqual(s.code, 8);
    ok(s.stack.split('\n')[1].includes('new Sub'), s.stack);
  },
};

export const classShape = {
  test() {
    // The TypeScript class, whose source shows; the C++ class's showed [native code].
    ok(
      !Function.prototype.toString.call(DOMException).includes('[native code]')
    );
    strictEqual(DOMException.name, 'DOMException');
    strictEqual(DOMException.length, 0);
    strictEqual(Object.getPrototypeOf(DOMException), Function.prototype);
    strictEqual(Object.getPrototypeOf(DOMException.prototype), Error.prototype);
    deepStrictEqual(Reflect.ownKeys(DOMException), [
      'length',
      'name',
      'prototype',
      ...CODES,
    ]);
    deepStrictEqual(Reflect.ownKeys(DOMException.prototype).slice(0, 26), [
      ...CODES,
      'constructor',
    ]);
    CODES.forEach((name, i) => {
      strictEqual(DOMException[name], i + 1);
      strictEqual(DOMException.prototype[name], i + 1);
    });
    strictEqual(DOMException.prototype.constructor, DOMException);
    ok(Symbol.for('cloudflare:internal-class') in DOMException.prototype);

    // The global lives on the global object's prototype, as JSG puts it.
    strictEqual(
      Object.getOwnPropertyDescriptor(globalThis, 'DOMException'),
      undefined
    );
    deepStrictEqual(
      describe(Object.getPrototypeOf(globalThis), 'DOMException'),
      { writable: true, enumerable: true, configurable: true, accessor: false }
    );
  },
};

export const createdByTheRuntime = {
  test() {
    let e;
    try {
      atob('*');
    } catch (err) {
      e = err;
    }
    checkInstance(e, 'InvalidCharacterError', e.message, 5);

    try {
      structuredClone(() => {});
    } catch (err) {
      e = err;
    }
    checkInstance(e, 'DataCloneError', e.message, 25);

    const reason = AbortSignal.abort().reason;
    ok(reason instanceof DOMException);
    strictEqual(reason.name, 'AbortError');
  },
};

export const userCodeCannotRedirectTheRuntime = {
  test() {
    const original = DOMException;
    const originalPrototype = DOMException.prototype;
    try {
      globalThis.DOMException = class Fake {};
      original.prototype = {};
      let e;
      try {
        atob('*');
      } catch (err) {
        e = err;
      }
      strictEqual(Object.getPrototypeOf(e), originalPrototype);
    } finally {
      original.prototype = originalPrototype;
      globalThis.DOMException = original;
    }
  },
};

export const cloned = {
  test() {
    const e = new DOMException('cl', 'DataCloneError');
    const c = structuredClone(e);
    checkInstance(c, 'DataCloneError', 'cl', 25);
    strictEqual(c.stack, e.stack);

    // The clone has the name and message the DOMException was created with.
    const overridden = new DOMException('m', 'DataError');
    Object.defineProperty(overridden, 'name', { value: 'TypeError' });
    strictEqual(structuredClone(overridden).name, 'DataError');

    // A plain object that looks like one is cloned as a plain object.
    const plain = structuredClone({ name: 'AbortError', message: 'm' });
    ok(!(plain instanceof DOMException));
  },
};
