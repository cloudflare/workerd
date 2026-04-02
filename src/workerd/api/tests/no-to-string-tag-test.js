// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { strictEqual } from 'node:assert';

export default {
  test() {
    const h = new Headers();
    strictEqual(h[Symbol.toStringTag], undefined);
  },
};
