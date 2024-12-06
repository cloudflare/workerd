// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as assert from 'node:assert';

export const tests = {
  async test(_, env) {
    {
      // Test gateway get log
      const resp = await env.ai.gateway('my-gateway').getLog('my-log-123');
      assert.deepEqual(resp, {
        cached: false,
        cost: 0,
        created_at: new Date('2019-08-24T14:15:22Z'),
        custom_cost: true,
        duration: 0,
        id: 'string',
        metadata: 'string',
        model: 'string',
        model_type: 'string',
        path: 'string',
        provider: 'string',
        request_content_type: 'string',
        request_head: 'string',
        request_head_complete: true,
        request_size: 0,
        request_type: 'string',
        response_content_type: 'string',
        response_head: 'string',
        response_head_complete: true,
        response_size: 0,
        status_code: 0,
        step: 0,
        success: true,
        tokens_in: 0,
        tokens_out: 0,
      });
    }

    {
      // Test get log error responses
      try {
        await env.ai.gateway('my-gateway').getLog('404');
      } catch (e) {
        assert.deepEqual(
          {
            name: e.name,
            message: e.message,
          },
          {
            name: 'AiGatewayLogNotFound',
            message: 'Not Found',
          }
        );
      }
    }

    {
      try {
        await env.ai.gateway('my-gateway').getLog('500');
      } catch (e) {
        assert.deepEqual(
          {
            name: e.name,
            message: e.message,
          },
          {
            name: 'AiGatewayInternalError',
            message: 'Internal Error',
          }
        );
      }
    }

    {
      // Test patch log error responses
      try {
        await env.ai.gateway('my-gateway').patchLog('404', { feedback: -1 });
      } catch (e) {
        assert.deepEqual(
          {
            name: e.name,
            message: e.message,
          },
          {
            name: 'AiGatewayLogNotFound',
            message: 'Not Found',
          }
        );
      }
    }

    {
      try {
        await env.ai.gateway('my-gateway').patchLog('500', { feedback: -1 });
      } catch (e) {
        assert.deepEqual(
          {
            name: e.name,
            message: e.message,
          },
          {
            name: 'AiGatewayInternalError',
            message: 'Internal Error',
          }
        );
      }
    }

    // Universal Run
    {
      const resp = await env.ai.gateway('my-gateway').run({
        provider: 'workers-ai',
        endpoint: '@cf/meta/llama-3.1-8b-instruct',
        headers: {
          Authorization: 'Bearer abcde',
          'Content-Type': 'application/json',
          'cf-aig-metadata': { user: 123 },
          'cf-aig-custom-cost': { total_cost: 1.22 },
          'cf-aig-skip-cache': false,
          'cf-aig-cache-ttl': 123,
        },
        query: {
          prompt: 'What is Cloudflare?',
        },
      });

      const body = await resp.json();

      assert.deepEqual(body, {
        result: [
          {
            endpoint: '@cf/meta/llama-3.1-8b-instruct',
            headers: {
              'Content-Type': 'application/json',
              'cf-aig-cache-ttl': '123',
              'cf-aig-custom-cost': '{"total_cost":1.22}',
              'cf-aig-metadata': '{"user":123}',
              'cf-aig-skip-cache': 'false',
              Authorization: 'Bearer abcde',
            },
            provider: 'workers-ai',
            query: { prompt: 'What is Cloudflare?' },
          },
        ],
        success: true,
      });
    }
  },
};
