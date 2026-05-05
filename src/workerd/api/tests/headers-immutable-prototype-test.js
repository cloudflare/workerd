// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { strictEqual } from 'node:assert';

export const test = {
  test() {
    strictEqual(
      Reflect.getOwnPropertyDescriptor(Headers, 'prototype').writable,
      false
    );
  },
};
