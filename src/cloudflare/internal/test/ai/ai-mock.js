// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);

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

    const data = await request.json();

    const modelName = request.headers.get('cf-consn-model-id');

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

    return Response.json(
      { response: 'model response' },
      {
        headers: {
          'content-type': 'application/json',
          ...respHeaders,
        },
      }
    );
  },
};
