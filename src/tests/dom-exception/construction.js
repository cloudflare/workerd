// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// new DOMException(message = '', name = 'Error'): both arguments are
// DOMStrings, converted message first; `code` follows from the name.

import { deepStrictEqual, strictEqual, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';
import { kLegacyCodes, kLoneSurrogate } from 'helpers';

export const constructorRequiresNew = {
  test() {
    throws(() => DOMException('m'), {
      name: 'TypeError',
      // Ledger #13: the language's message for a class.
      message: usingTsImpl
        ? "Class constructor DOMException cannot be invoked without 'new'"
        : "Failed to construct 'DOMException': Please use the 'new' operator, " +
          'this object constructor cannot be called as a function.',
    });
  },
};

export const defaults = {
  test() {
    for (const error of [
      new DOMException(),
      new DOMException(undefined, undefined),
    ]) {
      strictEqual(error.message, '');
      strictEqual(error.name, 'Error');
      strictEqual(error.code, 0);
    }
  },
};

export const argumentConversion = {
  test() {
    const error = new DOMException(
      {
        toString() {
          return 'x';
        },
      },
      5
    );
    strictEqual(error.message, 'x');
    strictEqual(error.name, '5');
    strictEqual(error.code, 0);

    const nulls = new DOMException(null, null);
    strictEqual(nulls.message, 'null');
    strictEqual(nulls.name, 'null');

    // The second argument is a name, not an options bag.
    strictEqual(new DOMException('m', { name: 'X' }).name, '[object Object]');

    throws(() => new DOMException(Symbol()), TypeError);
    throws(() => new DOMException('m', Symbol()), TypeError);
  },
};

export const argumentConversionOrder = {
  test() {
    const log = [];
    const arg = (label) => ({
      toString() {
        log.push(label);
        return label;
      },
    });
    new DOMException(arg('message'), arg('name'));
    deepStrictEqual(log, ['message', 'name']);

    // A failing message conversion means the name is not converted.
    log.length = 0;
    throws(
      () =>
        new DOMException(
          {
            toString() {
              throw new RangeError('boom');
            },
          },
          arg('name')
        ),
      RangeError
    );
    deepStrictEqual(log, []);
  },
};

export const codes = {
  test() {
    for (const [, value, name] of kLegacyCodes) {
      strictEqual(new DOMException('', name).code, value);
    }
    // Names without a legacy code, including the modern ones.
    for (const name of [
      'Error',
      'EncodingError',
      'NotReadableError',
      'UnknownError',
      'ConstraintError',
      'DataError',
      'TransactionInactiveError',
      'ReadOnlyError',
      'VersionError',
      'OperationError',
      'NotAllowedError',
      'OptOutError',
      'abortError',
      'ABORT_ERR',
    ]) {
      strictEqual(new DOMException('', name).code, 0, name);
    }
  },
};

export const loneSurrogates = {
  test() {
    const error = new DOMException(kLoneSurrogate, `${kLoneSurrogate}x`);
    if (usingTsImpl) {
      // Ledger #9: DOMStrings keep lone surrogates.
      strictEqual(error.message, kLoneSurrogate);
      strictEqual(error.name, `${kLoneSurrogate}x`);
    } else {
      // Through WTF-8: one U+FFFD per byte of the surrogate's encoding.
      strictEqual(error.message, '\uFFFD\uFFFD\uFFFD');
      strictEqual(error.name, '\uFFFD\uFFFD\uFFFDx');
    }
  },
};
