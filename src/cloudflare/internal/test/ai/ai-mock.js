// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { WorkerEntrypoint } from 'cloudflare:workers';

const claudePricing = {
  type: 'tokens',
  currency: 'USD',
  unit: 'per million tokens',
  input_tokens: 15,
  output_tokens: 75,
};

const claudeModel = {
  id: 'anthropic/claude',
  name: 'Claude',
  author: 'anthropic',
  source: 'aig-catalog',
  modality: 'text-generation',
  tags: ['LLM'],
  description: 'Claude language model',
  properties: { contextLength: 200000, maxOutputTokens: 8192 },
  pricing: claudePricing,
  created_at: '2026-11-04T20:00:00Z',
};

const claudeProvider = {
  id: 'anthropic',
  name: 'Anthropic',
  slug: 'anthropic',
  is_default: true,
  is_custom: false,
  account_specific: false,
  pricing: claudePricing,
  supported_formats: ['anthropic-messages'],
  capabilities: { streaming: true, tool_use: true },
  performance: {
    latency_ms: { avg: 234, p50: 198, p95: 456 },
    uptime_percent_7d: 99.8,
    sample_size: 145623,
  },
};

const aliasModel = {
  ...claudeModel,
  id: 'custom/my-alias',
  name: 'My alias',
  author: 'custom',
};

const customProvider = {
  ...claudeProvider,
  id: 'custom',
  name: 'Custom',
  slug: 'custom',
  is_custom: true,
  account_specific: true,
};

const workersAiModel = {
  ...claudeModel,
  id: '@cf/meta/llama-3.1-8b-instruct',
  name: 'Llama 3.1 8B Instruct',
  author: 'workers-ai',
  source: 'workers-ai',
  task: 'text-generation',
  pricing: { ...claudePricing, input_tokens: 5, output_tokens: 25 },
};

const workersAiProvider = {
  ...claudeProvider,
  id: 'workers-ai',
  name: 'Workers AI',
  slug: 'workers-ai',
  pricing: workersAiModel.pricing,
  supported_formats: ['openai-chat'],
};

export default class AiMock extends WorkerEntrypoint {
  async websearch({ gatewayId, provider, query, limit, byokAlias }, params) {
    if (typeof gatewayId !== 'string' || gatewayId.trim() === '') {
      throw new Error('Invalid gateway ID');
    }

    return Response.json({
      requestUrl: `https://workers-binding.ai/ai-gateway/gateways/${encodeURIComponent(gatewayId)}/websearch`,
      body: {
        provider,
        query,
        ...(limit !== undefined ? { limit } : {}),
        ...(byokAlias !== undefined ? { byokAlias } : {}),
      },
      extraHeaders: params?.extraHeaders,
    });
  }

  async fetch(request) {
    const url = new URL(request.url);

    if (url.pathname === '/ai-api/catalog/models') {
      return Response.json({
        success: true,
        result: [claudeModel],
        result_info: { count: 1, page: 2, per_page: 1, total_count: 3 },
        requestUrl: request.url,
      });
    }

    if (url.pathname === '/ai-api/catalog/gateways/my-gateway/models') {
      return Response.json({
        success: true,
        result: [aliasModel],
        result_info: { count: 1, page: 1, per_page: 50, total_count: 1 },
        requestUrl: request.url,
      });
    }

    if (
      url.pathname ===
      '/ai-api/catalog/gateways/my-gateway/models/anthropic/claude'
    ) {
      return Response.json({
        success: true,
        result: { ...claudeModel, providers: [customProvider] },
      });
    }

    if (url.pathname === '/ai-api/catalog/models/anthropic/claude') {
      return Response.json({
        success: true,
        result: { ...claudeModel, providers: [claudeProvider] },
      });
    }

    if (
      [
        '/ai-api/catalog/models/@cf%2Fmeta/llama-3.1-8b-instruct',
        '/ai-api/catalog/models/%40cf%2Fmeta/llama-3.1-8b-instruct',
      ].includes(url.pathname)
    ) {
      return Response.json({
        success: true,
        result: { ...workersAiModel, providers: [workersAiProvider] },
      });
    }

    if (url.pathname === '/ai-api/catalog/models/anthropic/missing') {
      return Response.json(
        { success: false, errors: [{ message: 'Model not found' }] },
        { status: 404 }
      );
    }

    if (url.pathname.startsWith('/ai-api/catalog/')) {
      throw new Error(`Unexpected catalog path: ${url.pathname}`);
    }

    if (url.pathname === '/ai-api/models/search') {
      return Response.json({
        success: true,
        result: [
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
                value: request.url,
              },
            ],
          },
        ],
      });
    }

    if (url.pathname === '/ai-gateway/gateways/my-gateway/websearch') {
      return Response.json({
        requestUrl: request.url,
        body: await request.json(),
      });
    }

    const reqContentType = request.headers.get('content-type');

    let data = {};
    if (reqContentType === 'application/json') {
      data = await request.json();
    } else {
      data = {
        inputs: request.body,
        options: Object.fromEntries(url.searchParams),
      };
    }

    const modelName = request.headers.get('cf-consn-model-id');
    const isWebsocket = request.headers.get('Upgrade') === 'websocket';

    const respHeaders = {
      'cf-ai-req-id': '3a1983d7-1ddd-453a-ab75-c4358c91b582',
    };

    if (modelName === 'blobResponseModel') {
      let utf8Encode = new TextEncoder();
      utf8Encode.encode('hello world');

      return new Response(utf8Encode, {
        headers: respHeaders,
      });
    }

    if (modelName === 'rawInputs') {
      return Response.json(
        {
          ...data,
          requestUrl: request.url,
        },
        {
          headers: respHeaders,
        }
      );
    }

    if (modelName === 'readableStreamIputs') {
      return Response.json(
        {
          inputs: {},
          options: { ...data.options },
          requestUrl: request.url,
        },
        {
          headers: respHeaders,
        }
      );
    }

    if (modelName === 'formDataInputs') {
      return Response.json(
        {
          inputs: {},
          options: { ...data.options },
          requestUrl: request.url,
        },
        {
          headers: respHeaders,
        }
      );
    }

    if (modelName === 'hangingModel') {
      // Never resolve — used to test abort signal on an in-flight request.
      // We can't just return a hanging promise because the runtime actually detects that and
      // errors. We use an excessively long timeout instead so that the runtime thinks that it's
      // actually waiting for something.
      await scheduler.wait(1e8);
    }

    if (modelName === 'inputErrorModel') {
      return Response.json(
        {
          internalCode: 1001,
          message: 'InvalidInput: prompt and messages are mutually exclusive',
          name: 'InvalidInput',
          description: 'prompt and messages are mutually exclusive',
        },
        {
          status: 400,
          headers: {
            'content-type': 'application/json',
            ...respHeaders,
          },
        }
      );
    }

    // Handle websocket requests
    if (isWebsocket && modelName === '@cf/test/websocket') {
      // For websocket requests, extract data from URL 'body' parameter
      const bodyParam = url.searchParams.get('body');
      let websocketData = {};
      if (bodyParam) {
        try {
          // The AI API doesn't URL-encode the body parameter, so parse directly
          websocketData = JSON.parse(bodyParam);
        } catch (_e) {
          websocketData = { inputs: {}, options: {} };
        }
      }

      return Response.json(
        {
          ...websocketData,
          requestUrl: request.url,
          headers: {
            'cf-consn-sdk-version': '2.0.0',
            'cf-consn-model-id': '@cf/test/websocket',
            upgrade: 'websocket',
          },
        },
        {
          headers: respHeaders,
        }
      );
    }

    return Response.json(
      { response: 'model response' },
      {
        headers: {
          'content-type': 'application/json',
          ...respHeaders,
        },
      }
    );
  }
}
