// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { deepStrictEqual, strictEqual } from 'node:assert';

// The TextEncoder global is defined as TextDecoder's is and follows it: on the global scope's
// prototype, and under spec_compliant_property_attributes on the global scope too.
export const textEncoderGlobal = {
  test() {
    for (const target of [Object.getPrototypeOf(globalThis), globalThis]) {
      const decoder = Object.getOwnPropertyDescriptor(target, 'TextDecoder');
      const encoder = Object.getOwnPropertyDescriptor(target, 'TextEncoder');
      if (decoder === undefined) {
        strictEqual(encoder, undefined);
        continue;
      }
      deepStrictEqual(encoder, { ...decoder, value: TextEncoder });
      const keys = Reflect.ownKeys(target);
      strictEqual(keys[keys.indexOf('TextDecoder') + 1], 'TextEncoder');
    }
  },
};
