// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// postMessage()'s second argument converts as JSG converts
// `optional (sequence<any> or PostMessageOptions)`, and a non-empty transfer
// list is refused, since transferring is unsupported.
// Divergence #6 (see AGENTS.md): the TypeScript implementation recognizes a
// proxy for an array as an array (Array.isArray()); JSG does not, and
// converts it as a PostMessageOptions dictionary.

import { deepStrictEqual, strictEqual, throws } from 'node:assert';
import { mock } from 'node:test';
import { usingTsImpl } from 'which-impl';

const kBadOptions =
  "Failed to execute 'postMessage' on 'MessagePort': parameter 2 is not " +
  "of type 'Array or PostMessageOptions'.";
const kBadTransfer =
  "Incorrect type for the 'transfer' field on 'PostMessageOptions': the " +
  "provided value is not of type 'Array'.";
const kTransferUnsupported = {
  name: 'Error',
  message: 'Transfer list is not supported',
};

export const primitiveOptionsAreRejected = {
  test() {
    const { port1 } = new MessageChannel();
    for (const options of [5, 'x', true, Symbol(), 1n]) {
      throws(() => port1.postMessage(1, options), {
        name: 'TypeError',
        message: kBadOptions,
      });
    }
  },
};

export const emptyTransferListsAreAccepted = {
  async test() {
    const { port1, port2 } = new MessageChannel();
    const received = mock.fn();
    port2.onmessage = received;
    const accepted = [
      undefined,
      null,
      [],
      {},
      { transfer: undefined },
      { transfer: [] },
      () => {},
      new Set(),
    ];
    for (const options of accepted) {
      port1.postMessage(1, options);
    }
    await scheduler.wait(10);
    strictEqual(received.mock.callCount(), accepted.length);
    port1.close();
  },
};

export const transferMustBeAnArray = {
  test() {
    const { port1 } = new MessageChannel();
    for (const transfer of [5, null, 'x', {}]) {
      throws(() => port1.postMessage(1, { transfer }), {
        name: 'TypeError',
        message: kBadTransfer,
      });
    }
  },
};

export const nonEmptyTransferListsAreUnsupported = {
  test() {
    const { port1 } = new MessageChannel();
    throws(() => port1.postMessage(1, [1]), kTransferUnsupported);
    throws(() => port1.postMessage(1, { transfer: [1] }), kTransferUnsupported);
  },
};

// The options and every transfer list element are read before the list is
// refused, and the message is not read at all.
export const conversionReadsBeforeRefusing = {
  test() {
    const { port1 } = new MessageChannel();
    const reads = [];
    const list = [];
    for (const index of [0, 1]) {
      Object.defineProperty(list, index, {
        get() {
          reads.push(`element${index}`);
          return index;
        },
      });
    }
    const data = {
      get value() {
        reads.push('data');
        return 1;
      },
    };
    throws(
      () =>
        port1.postMessage(data, {
          get transfer() {
            reads.push('transfer');
            return list;
          },
        }),
      kTransferUnsupported
    );
    deepStrictEqual(reads, ['transfer', 'element0', 'element1']);

    throws(
      () =>
        port1.postMessage(1, {
          get transfer() {
            throw new Error('getter');
          },
        }),
      { name: 'Error', message: 'getter' }
    );
  },
};

export const proxiedArrayOptions = {
  test() {
    const { port1 } = new MessageChannel();
    const options = new Proxy([1], {});
    if (usingTsImpl) {
      throws(() => port1.postMessage(1, options), kTransferUnsupported);
    } else {
      // Read as a dictionary, whose `transfer` is undefined.
      port1.postMessage(1, options);
    }
  },
};
