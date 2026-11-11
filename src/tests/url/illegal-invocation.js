// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Every member is brand-checked: any other receiver, an object inheriting
// from the prototype included, throws JSG's illegal-invocation TypeError.
// The statics are not brand-checked.

import { throws, strictEqual, deepStrictEqual } from 'node:assert';
import { usingTsImpl, urlStandard } from 'which-impl';
import {
  kIllegalInvocation,
  kUrlAttributes,
  kReadonlyUrlAttributes,
  kSearchParamsMethods,
} from 'helpers';

const kIllegal = { name: 'TypeError', message: kIllegalInvocation };

function foreignReceivers(proto) {
  return [
    {},
    Object.create(proto),
    proto === URL.prototype ? new URLSearchParams() : new URL('http://a/'),
    1,
    'a',
    null,
    undefined,
  ];
}

export const urlMembersAreBrandChecked = {
  test() {
    const proto = URL.prototype;
    for (const receiver of foreignReceivers(proto)) {
      throws(() => proto.toString.call(receiver), kIllegal);
      throws(() => proto.toJSON.call(receiver), kIllegal);
      for (const name of kUrlAttributes) {
        const d = Object.getOwnPropertyDescriptor(proto, name);
        // The legacy C++ instance layout has no prototype accessors.
        if (d === undefined) continue;
        throws(() => d.get.call(receiver), kIllegal, name);
        if (!kReadonlyUrlAttributes.includes(name)) {
          throws(() => d.set.call(receiver, 'http://b/'), kIllegal, name);
        }
      }
    }
  },
};

export const searchParamsMembersAreBrandChecked = {
  test() {
    const proto = URLSearchParams.prototype;
    const size = Object.getOwnPropertyDescriptor(proto, 'size').get;
    for (const receiver of foreignReceivers(proto)) {
      throws(() => size.call(receiver), kIllegal);
      for (const name of kSearchParamsMethods) {
        const args = name === 'forEach' ? [() => {}] : ['a', 'b'];
        throws(() => proto[name].call(receiver, ...args), kIllegal, name);
      }
      throws(() => proto[Symbol.iterator].call(receiver), kIllegal);
    }
  },
};

export const iteratorNextIsBrandChecked = {
  test() {
    const params = new URLSearchParams('a=1');
    for (const it of [params.entries(), params.keys(), params.values()]) {
      const next = Object.getPrototypeOf(it).next;
      for (const receiver of [
        {},
        Object.create(Object.getPrototypeOf(it)),
        params,
        1,
        null,
        undefined,
      ]) {
        throws(() => next.call(receiver), kIllegal);
      }
    }
    // Ledger #6: one prototype serves every kind of iterator.
    const keys = new URLSearchParams('a=1').keys();
    const entriesNext = Object.getPrototypeOf(params.entries()).next;
    if (usingTsImpl) {
      deepStrictEqual(entriesNext.call(keys), { value: 'a', done: false });
    } else {
      throws(() => entriesNext.call(keys), kIllegal);
    }
  },
};

export const staticsTakeAnyReceiver = {
  test() {
    if (!urlStandard && !usingTsImpl) return;
    strictEqual(URL.canParse.call(undefined, 'http://a/'), true);
    strictEqual(String(URL.parse.call(null, 'http://a/')), 'http://a/');
  },
};
