// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Without set_tostring_tag, Event and CustomEvent have no
// Symbol.toStringTag, so Object.prototype.toString() reports their
// instances as plain objects.

import { strictEqual } from 'node:assert';

export const legacyNoToStringTag = {
  test() {
    for (const [ctor, event] of [
      [Event, new Event('x')],
      [CustomEvent, new CustomEvent('x')],
    ]) {
      strictEqual(
        Object.getOwnPropertyDescriptor(ctor.prototype, Symbol.toStringTag),
        undefined,
        ctor.name
      );
      strictEqual(event[Symbol.toStringTag], undefined, ctor.name);
      strictEqual(
        Object.prototype.toString.call(event),
        '[object Object]',
        ctor.name
      );
    }
  },
};
