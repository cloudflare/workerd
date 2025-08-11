// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

function base64ToBlob(base64, mimeType) {
  const binaryString = atob(base64); // Decode Base64
  const len = binaryString.length;
  const bytes = new Uint8Array(len);
  for (let i = 0; i < len; i++) {
    bytes[i] = binaryString.charCodeAt(i);
  }
  return new Blob([bytes], { type: mimeType });
}

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);

    if (url.pathname === '/to-everything/markdown/transformer') {
      const body = await request.json();
      const decoder = new TextDecoder('utf-8');

      const result = [];

      for (const file of body.files) {
        if (file.name === 'headers.md') {
          const newHeaders = new Headers(request.headers);
          newHeaders.delete('content-length');
          result.push({
            name: file.name,
            mimeType: file.mimeType,
            format: 'markdown',
            tokens: 0,
            data: Object.fromEntries(newHeaders.entries()),
          });
        } else {
          const fileblob = await base64ToBlob(file.data, file.mimeType);
          const arr = await fileblob.arrayBuffer();
          result.push({
            name: file.name,
            mimeType: file.mimeType,
            format: 'markdown',
            tokens: 0,
            data: decoder.decode(arr),
          });
        }
      }

      return Response.json({
        success: true,
        result: result,
      });
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
    if (isWebsocket) {
      // For websocket requests, extract data from URL 'body' parameter
      const bodyParam = url.searchParams.get('body');
      let websocketData = {};
      if (bodyParam) {
        try {
          websocketData = JSON.parse(decodeURIComponent(bodyParam));
        } catch (e) {
          websocketData = { inputs: {}, options: {} };
        }
      }

      return Response.json(
        {
          ...websocketData,
          requestUrl: request.url,
          headers: Object.fromEntries(request.headers.entries()),
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
  },
};
