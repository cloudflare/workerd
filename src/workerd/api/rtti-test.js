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
