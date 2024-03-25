// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as assert from 'node:assert'

export const tests = {
    async test(_, env) {
        {
            // Test ai run response is object
            const resp = await env.ai.run('testModel', {prompt: 'test'})
            assert.deepStrictEqual(resp, { response: 'model response' });

            // Test logs is empty
            assert.deepStrictEqual(env.ai.getLogs(), []);

            // Test request id is present
            assert.deepStrictEqual(env.ai.lastRequestId, '3a1983d7-1ddd-453a-ab75-c4358c91b582');
        }

        {
            // Test ai blob model run response is a blob/stream
            const resp = await env.ai.run('blobResponseModel', {prompt: 'test'})
            assert.deepStrictEqual(resp instanceof ReadableStream, true);
        }

        {
            // Test logs
            await env.ai.run('testModel', {prompt: 'test'}, {debug: true})
            assert.deepStrictEqual(env.ai.getLogs(),  [ 'Model started', 'Model run successfully' ]);
        }

        {
            // Test legacy fetch
            const resp = await env.ai.fetch("http://workers-binding.ai/run?version=2", {
                method: 'POST',
                headers: {'content-type': 'application/json'},
                body: JSON.stringify({
                    inputs: {prompt: 'test'},
                    options: {}
                })
            })
            assert.deepStrictEqual(await resp.json(),  { response: 'model response' });
        }
    },
}
