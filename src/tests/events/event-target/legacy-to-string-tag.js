// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Without set_tostring_tag, EventTarget has no Symbol.toStringTag, so
// Object.prototype.toString() reports a target as a plain object.

import { strictEqual } from 'node:assert';

export const legacyNoToStringTag = {
  test() {
    strictEqual(
      Object.getOwnPropertyDescriptor(
        EventTarget.prototype,
        Symbol.toStringTag
      ),
      undefined
    );
    const target = new EventTarget();
    strictEqual(target[Symbol.toStringTag], undefined);
    strictEqual(Object.prototype.toString.call(target), '[object Object]');
  },
};
