// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as assert from 'node:assert'

export const tests = {
    async test(ctr, env) {
        // Test ai run
        assert.deepStrictEqual(await env.ai.run('testModel', {prompt: 'test'}), { response: 'model response' });
    },
}
