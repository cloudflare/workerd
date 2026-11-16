// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Every member is brand-checked: any other receiver, an object inheriting
// from the prototype included, throws JSG's illegal-invocation TypeError,
// before any argument is converted.

import { throws } from 'node:assert';
import { kIllegalInvocation, kAttributes } from 'helpers';

const kIllegal = { name: 'TypeError', message: kIllegalInvocation };

export const membersAreBrandChecked = {
  test() {
    const proto = URLPattern.prototype;
    const poisoned = {
      get pathname() {
        throw new Error('converted');
      },
    };
    for (const receiver of [
      {},
      Object.create(proto),
      new URL('http://a/'),
      1,
      'a',
      null,
      undefined,
    ]) {
      throws(() => proto.test.call(receiver, poisoned), kIllegal);
      throws(() => proto.exec.call(receiver, poisoned), kIllegal);
      for (const name of kAttributes) {
        const d = Object.getOwnPropertyDescriptor(proto, name);
        throws(() => d.get.call(receiver), kIllegal, name);
      }
    }
  },
};
