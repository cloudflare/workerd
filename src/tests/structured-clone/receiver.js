// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The receiver check: the global, undefined (a plain call) or null is
// accepted, as WebIDL has it for an operation of the global; anything else is
// an illegal invocation, with the same TypeError in both.

import { strictEqual, throws } from 'node:assert';

const ILLEGAL_INVOCATION =
  'Illegal invocation: function called with incorrect `this` reference. See ' +
  'https://developers.cloudflare.com/workers/observability/errors/' +
  '#illegal-invocation-errors for details.';

export const acceptedReceivers = {
  test() {
    const sc = structuredClone;
    strictEqual(sc(1), 1);
    strictEqual(sc.call(undefined, 2), 2);
    strictEqual(sc.call(null, 3), 3);
    strictEqual(sc.call(globalThis, 3), 3);
    strictEqual(globalThis.structuredClone(4), 4);
    strictEqual(Reflect.apply(sc, globalThis, [5]), 5);
  },
};

export const rejectedReceivers = {
  test() {
    const sc = structuredClone;
    for (const receiver of [
      {},
      42,
      'str',
      Object.create(globalThis),
      Object.getPrototypeOf(globalThis),
    ]) {
      throws(() => sc.call(receiver, 1), {
        name: 'TypeError',
        message: ILLEGAL_INVOCATION,
      });
    }
  },
};

export const receiverCheckedBeforeArguments = {
  test() {
    let read = false;
    const options = {
      get transfer() {
        read = true;
        return [];
      },
    };
    throws(() => structuredClone.call({}, 1, options), {
      message: ILLEGAL_INVOCATION,
    });
    strictEqual(read, false);
  },
};
