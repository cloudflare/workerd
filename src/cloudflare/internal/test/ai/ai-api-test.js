// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as assert from 'node:assert';

export const tests = {
  async test(_, env) {
    {
      // Test ai run response is object
      const resp = await env.ai.run('testModel', { prompt: 'test' });
      assert.deepStrictEqual(resp, { response: 'model response' });

      // Test request id is present
      assert.deepStrictEqual(
        env.ai.lastRequestId,
        '3a1983d7-1ddd-453a-ab75-c4358c91b582'
      );
      // Test request http status code is present
      assert.deepStrictEqual(env.ai.lastRequestHttpStatusCode, 200);
    }

    {
      // Test ai blob model run response is a blob/stream
      const resp = await env.ai.run('blobResponseModel', { prompt: 'test' });
      assert.deepStrictEqual(resp instanceof ReadableStream, true);
    }

    {
      // Test legacy fetch
      const resp = await env.ai.fetch(
        'http://workers-binding.ai/run?version=2',
        {
          method: 'POST',
          headers: { 'content-type': 'application/json' },
          body: JSON.stringify({
            inputs: { prompt: 'test' },
            options: {},
          }),
        }
      );
      assert.deepStrictEqual(await resp.json(), { response: 'model response' });
    }

    {
      // Test error response
      try {
        await env.ai.run('inputErrorModel', { prompt: 'test' });
      } catch (e) {
        assert.deepEqual(
          {
            name: e.name,
            message: e.message,
          },
          {
            name: 'InvalidInput',
            message: '1001: prompt and messages are mutually exclusive',
          }
        );
        // Test request internal status code is present
        assert.deepEqual;
        assert.deepStrictEqual(env.ai.lastRequestInternalStatusCode, 1001);
      }
    }

    {
      // Test error properties
      const err = await env.ai._parseError(
        Response.json({
          internalCode: 1001,
          message: 'InvalidInput: prompt and messages are mutually exclusive',
          name: 'InvalidInput',
          description: 'prompt and messages are mutually exclusive',
        })
      );
      assert.equal(err.name, 'InvalidInput');
      assert.equal(
        err.message,
        '1001: prompt and messages are mutually exclusive'
      );
    }

    {
      // Test error properties from non json response
      const err = await env.ai._parseError(new Response('Unknown error'));
      assert.equal(err.name, 'InferenceUpstreamError');
      assert.equal(err.message, 'Unknown error');
    }

    {
      // Test raw input
      const resp = await env.ai.run('rawInputs', { prompt: 'test' });

      assert.deepStrictEqual(resp, {
        inputs: { prompt: 'test' },
        options: {},
        requestUrl: 'https://workers-binding.ai/run?version=3',
      });
    }

    {
      // Test gateway option
      const resp = await env.ai.run(
        'rawInputs',
        { prompt: 'test' },
        { gateway: { id: 'my-gateway', skipCache: true } }
      );

      assert.deepStrictEqual(resp, {
        inputs: { prompt: 'test' },
        options: { gateway: { id: 'my-gateway', skipCache: true } },
        requestUrl: 'https://workers-binding.ai/ai-gateway/run?version=3',
      });
    }

    {
      // Test unwanted options not getting sent upstream
      const resp = await env.ai.run(
        'rawInputs',
        { prompt: 'test' },
        {
          extraHeaders: 'test',
          example: 123,
          gateway: { id: 'my-gateway', metadata: { employee: 1233 } },
        }
      );

      assert.deepStrictEqual(resp, {
        inputs: { prompt: 'test' },
        options: {
          example: 123,
          gateway: { id: 'my-gateway', metadata: { employee: 1233 } },
        },
        requestUrl: 'https://workers-binding.ai/ai-gateway/run?version=3',
      });
    }

    {
      // Test models
      const resp = await env.ai.models();

      assert.deepStrictEqual(resp, [
        {
          id: 'f8703a00-ed54-4f98-bdc3-cd9a813286f3',
          source: 1,
          name: '@cf/qwen/qwen1.5-0.5b-chat',
          description:
            'Qwen1.5 is the improved version of Qwen, the large language model series developed by Alibaba Cloud.',
          task: {
            id: 'c329a1f9-323d-4e91-b2aa-582dd4188d34',
            name: 'Text Generation',
            description:
              'Family of generative text models, such as large language models (LLM), that can be adapted for a variety of natural language tasks.',
          },
          tags: [],
          properties: [
            {
              property_id: 'debug',
              value: 'https://workers-binding.ai/ai-api/models/search',
            },
          ],
        },
      ]);
    }

    {
      // Test models with params
      const resp = await env.ai.models({
        search: 'test',
        per_page: 3,
        page: 1,
        task: 'asd',
      });

      assert.deepStrictEqual(resp, [
        {
          id: 'f8703a00-ed54-4f98-bdc3-cd9a813286f3',
          source: 1,
          name: '@cf/qwen/qwen1.5-0.5b-chat',
          description:
            'Qwen1.5 is the improved version of Qwen, the large language model series developed by Alibaba Cloud.',
          task: {
            id: 'c329a1f9-323d-4e91-b2aa-582dd4188d34',
            name: 'Text Generation',
            description:
              'Family of generative text models, such as large language models (LLM), that can be adapted for a variety of natural language tasks.',
          },
          tags: [],
          properties: [
            {
              property_id: 'debug',
              value:
                'https://workers-binding.ai/ai-api/models/search?search=test&per_page=3&page=1&task=asd',
            },
          ],
        },
      ]);
    }

    {
      // Test `returnRawResponse` option is returning a Response object
      const resp = await env.ai.run(
        'rawInputs',
        { prompt: 'test' },
        { returnRawResponse: true }
      );

      assert.ok(resp instanceof Response);
    }
  },
};
