// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as assert from 'node:assert';

export const tests = {
  async test(_, env) {
    {
      // Test create instance
      const instance = await env.workflow.create({
        name: 'foo',
        payload: { bar: 'baz' },
      });
      assert.deepStrictEqual(instance.id, 'foo');
    }

    {
      // Test get instance
      const instance = await env.workflow.get('bar');
      assert.deepStrictEqual(instance.id, 'bar');
    }
  },
};
