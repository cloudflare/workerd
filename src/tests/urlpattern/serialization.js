// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// URLPattern is not serializable: structuredClone() and postMessage()
// throw DataCloneError, naming the constructor. Its results are plain
// objects, which are.

import { throws, deepStrictEqual } from 'node:assert';

function uncloneable(type) {
  return {
    name: 'DataCloneError',
    message: `Could not serialize object of type "${type}". This type does not support serialization.`,
  };
}

const kUncloneable = uncloneable('URLPattern');

export const structuredCloneThrows = {
  test() {
    const pattern = new URLPattern({});
    throws(() => structuredClone(pattern), kUncloneable);
    throws(() => structuredClone({ nested: [pattern] }), kUncloneable);
    class Sub extends URLPattern {}
    throws(() => structuredClone(new Sub({})), uncloneable('Sub'));
  },
};

export const postMessageThrows = {
  test() {
    const { port1, port2 } = new MessageChannel();
    try {
      throws(() => port1.postMessage(new URLPattern({})), kUncloneable);
    } finally {
      port1.close();
      port2.close();
    }
  },
};

export const resultsAreCloneable = {
  test() {
    const result = new URLPattern({ pathname: '/:a' }).exec('https://h/x');
    deepStrictEqual(structuredClone(result), result);
  },
};
