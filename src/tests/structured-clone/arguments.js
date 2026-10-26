// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The argument count and the conversion of the options dictionary, the same
// in both.

import { strictEqual, throws } from 'node:assert';

const BAD_OPTIONS =
  "Failed to execute 'structuredClone' on 'ServiceWorkerGlobalScope': " +
  "parameter 2 is not of type 'StructuredCloneOptions'.";

export const valueIsRequired = {
  test() {
    throws(() => structuredClone(), {
      name: 'TypeError',
      message:
        "Failed to execute 'structuredClone' on 'ServiceWorkerGlobalScope': " +
        "parameter 1 is not of type 'Value'.",
    });
    strictEqual(structuredClone(undefined), undefined);
  },
};

export const extraArgumentsIgnored = {
  test() {
    strictEqual(structuredClone(1, undefined, 3), 1);
  },
};

export const absentOptions = {
  test() {
    strictEqual(structuredClone(1, undefined), 1);
    strictEqual(structuredClone(1, null), 1);
  },
};

export const objectOptions = {
  test() {
    // Any object is a dictionary, including functions and arrays, and an
    // absent or undefined transfer member transfers nothing.
    for (const options of [{}, [], () => {}, { transfer: undefined }]) {
      strictEqual(structuredClone(1, options), 1);
    }
  },
};

export const primitiveOptionsRejected = {
  test() {
    for (const options of [1, 's', true, Symbol('x'), 1n]) {
      throws(() => structuredClone(1, options), {
        name: 'TypeError',
        message: BAD_OPTIONS,
      });
    }
  },
};

export const transferMemberLookup = {
  test() {
    // Read once, through the prototype chain, before the value is serialized.
    const log = [];
    const options = Object.create({
      get transfer() {
        log.push('transfer');
        return [];
      },
    });
    const value = {
      get x() {
        log.push('value');
        return 1;
      },
    };
    strictEqual(structuredClone(value, options).x, 1);
    strictEqual(log.join(), 'transfer,value');
  },
};

export const transferGetterThrows = {
  test() {
    const error = new RangeError('boom');
    throws(
      () =>
        structuredClone(1, {
          get transfer() {
            throw error;
          },
        }),
      (e) => e === error
    );
  },
};
