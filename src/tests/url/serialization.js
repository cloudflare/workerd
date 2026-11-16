// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// URL, URLSearchParams and their iterators are not serializable: each
// throws DataCloneError, naming its constructor (ledger #4, #4a, #10).

import { throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

function uncloneable(type) {
  return {
    name: 'DataCloneError',
    message: `Could not serialize object of type "${type}". This type does not support serialization.`,
  };
}

function values() {
  const params = new URLSearchParams('a=1');
  return [
    [new URL('http://a/'), uncloneable('URL')],
    [params, uncloneable('URLSearchParams')],
    [new URL('http://a/?q').searchParams, uncloneable('URLSearchParams')],
    [
      params.entries(),
      usingTsImpl ? { name: 'DataCloneError' } : uncloneable('EntryIterator'),
    ],
    [
      params.keys(),
      usingTsImpl ? { name: 'DataCloneError' } : uncloneable('KeyIterator'),
    ],
  ];
}

export const structuredCloneThrows = {
  test() {
    for (const [value, error] of values()) {
      throws(() => structuredClone(value), error);
      throws(() => structuredClone({ nested: [value] }), error);
    }
  },
};

export const postMessageThrows = {
  test() {
    const { port1, port2 } = new MessageChannel();
    try {
      for (const [value, error] of values()) {
        throws(() => port1.postMessage(value), error);
      }
    } finally {
      port1.close();
      port2.close();
    }
  },
};

export const subclassInstancesThrow = {
  test() {
    class Sub extends URL {}
    throws(() => structuredClone(new Sub('http://a/')), uncloneable('Sub'));
  },
};
