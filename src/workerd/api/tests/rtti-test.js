// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import rtti from 'workerd:rtti';

export default {
  async test(ctrl, env, ctx) {
    const buffer = rtti.exportTypes('2023-05-18', ['nodejs_compat']);
    assert(buffer.byteLength > 0);
  },
};

// typescript_implemented_streams swaps the Web Streams implementation without changing its
// API, so it must not change the generated types.
export const typescriptImplementedStreamsDoesNotChangeTypes = {
  test() {
    const without = rtti.exportTypes('2023-05-18', ['nodejs_compat']);
    const withFlag = rtti.exportTypes('2023-05-18', [
      'nodejs_compat',
      'typescript_implemented_streams',
    ]);
    assert.deepStrictEqual(new Uint8Array(withFlag), new Uint8Array(without));
  },
};
