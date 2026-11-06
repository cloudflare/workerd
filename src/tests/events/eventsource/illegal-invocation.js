// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Every member is brand-checked: any other receiver, an object inheriting
// from EventSource.prototype included, gets JSG's illegal-invocation
// TypeError.

import { throws } from 'node:assert';

const kIllegalInvocation = {
  name: 'TypeError',
  message:
    'Illegal invocation: function called with incorrect `this` reference. ' +
    'See https://developers.cloudflare.com/workers/observability/errors/' +
    '#illegal-invocation-errors for details.',
};

export const membersAreBrandChecked = {
  test() {
    const proto = EventSource.prototype;
    const receivers = [
      {},
      Object.create(proto),
      new EventTarget(),
      undefined,
      null,
      1,
    ];
    const members = [];
    for (const key of Reflect.ownKeys(proto)) {
      const { value, get, set } = Object.getOwnPropertyDescriptor(proto, key);
      if (typeof value === 'function' && key !== 'constructor') {
        members.push([String(key), value, []]);
      }
      if (get) members.push([`get ${String(key)}`, get, []]);
      if (set) members.push([`set ${String(key)}`, set, [null]]);
    }
    for (const [name, fn, args] of members) {
      for (const receiver of receivers) {
        throws(() => fn.apply(receiver, args), kIllegalInvocation, name);
      }
    }
  },
};
